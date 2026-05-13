/*
 * Droidspaces v6 - High-performance Container Runtime
 *
 * Copyright (C) 2026 Josh Law <joshlaw48@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * In-container Pulse-protocol gateway.
 *
 * Speaks just enough of the PulseAudio native protocol that libpulse-simple
 * clients (paplay, parec, ffmpeg's pulse output, most apps that use the
 * "default" PA backend) can open a playback or record stream.  Whatever
 * those clients push or pull gets translated to/from our minimal framed
 * PCM protocol (audio_proto.h) and shuttled over the bind-mounted host
 * socket to the Android AudioBridgeService.
 *
 * What's implemented well enough to demo:
 *   AUTH, SET_CLIENT_NAME, CREATE_PLAYBACK_STREAM, CREATE_RECORD_STREAM,
 *   DRAIN_PLAYBACK_STREAM, DELETE_*_STREAM, inline PCM data frames, and
 *   the server -> client REQUEST flow that libpulse uses for backpressure.
 *
 * What's stubbed - we return ERROR_NOT_SUPPORTED:
 *   anything to do with sink/source enumeration, modules, volume control,
 *   subscriptions, extensions.  Real libpulse will gripe in its logs but
 *   libpulse-simple's hot path doesn't care.
 *
 * Architecture: single-threaded epoll loop.  All client sockets and the
 * host socket are in one epoll fd.  Per-stream state lives in arrays
 * indexed by the channel id we hand out.  No threads, no allocator
 * gymnastics, no shared-mem fast path - just blocking reads and inline
 * data frames.
 */

#include "audio_proto.h"
#include "droidspace.h"

#include <sys/epoll.h>
#include <sys/un.h>

/* ---------------------------------------------------------------------------
 * Pulse native protocol constants - subset we actually look at
 * ---------------------------------------------------------------------------*/

#define PA_PROTOCOL_VERSION 35

#define PA_TAG_STRING            't'
#define PA_TAG_STRING_NULL       'N'
#define PA_TAG_U32               'L'
#define PA_TAG_U8                'B'
#define PA_TAG_U64               'R'
#define PA_TAG_S64               'r'
#define PA_TAG_SAMPLE_SPEC       'a'
#define PA_TAG_ARBITRARY         'x'
#define PA_TAG_BOOLEAN_TRUE      '1'
#define PA_TAG_BOOLEAN_FALSE     '0'
#define PA_TAG_TIMEVAL           'T'
#define PA_TAG_USEC              'U'
#define PA_TAG_CHANNEL_MAP       'm'
#define PA_TAG_CVOLUME           'v'
#define PA_TAG_PROPLIST          'P'
#define PA_TAG_VOLUME            'V'
#define PA_TAG_FORMAT_INFO       'f'

#define PA_COMMAND_ERROR                 0
#define PA_COMMAND_REPLY                 2
#define PA_COMMAND_CREATE_PLAYBACK_STREAM 3
#define PA_COMMAND_DELETE_PLAYBACK_STREAM 4
#define PA_COMMAND_CREATE_RECORD_STREAM  5
#define PA_COMMAND_DELETE_RECORD_STREAM  6
#define PA_COMMAND_AUTH                  8
#define PA_COMMAND_SET_CLIENT_NAME       9
#define PA_COMMAND_DRAIN_PLAYBACK_STREAM 13
#define PA_COMMAND_REQUEST               61

#define PA_ERR_NOT_SUPPORTED  19

#define PA_SAMPLE_U8        0
#define PA_SAMPLE_S16LE     3
#define PA_SAMPLE_FLOAT32LE 5
#define PA_SAMPLE_S32LE     7

#define PA_FRAME_HDR_SIZE 20
#define PA_NO_CHANNEL 0xffffffffu

/* ---------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------------*/

#define MAX_STREAMS 16
#define MAX_CLIENTS 8

enum stream_dir { STREAM_PLAYBACK, STREAM_RECORD };

struct ds_stream {
  int      in_use;
  int      client_idx;
  uint32_t channel;
  uint32_t pulse_stream_index;
  enum stream_dir dir;
  uint8_t  host_stream_id;
  uint32_t sample_rate;
  uint8_t  channels;
  uint8_t  format;
  uint32_t buffer_bytes;
  uint32_t bytes_requested;
};

struct ds_pulse_client {
  int      fd;
  int      authed;
  uint32_t client_index;
  uint8_t  hdr[PA_FRAME_HDR_SIZE];
  size_t   hdr_filled;
  uint8_t *payload;
  size_t   payload_size;
  size_t   payload_filled;
  uint32_t cur_channel;
};

static struct ds_stream g_streams[MAX_STREAMS];
static struct ds_pulse_client g_clients[MAX_CLIENTS];

static int g_listen_fd = -1;
static int g_host_fd   = -1;
static int g_epoll_fd  = -1;

static uint32_t g_next_channel = 1;

/* ---------------------------------------------------------------------------
 * Endian helpers - Pulse uses big-endian on the wire
 * ---------------------------------------------------------------------------*/

static inline uint32_t rd32be(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void wr32be(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

/* ---------------------------------------------------------------------------
 * Robust I/O
 * ---------------------------------------------------------------------------*/

static ssize_t read_n(int fd, void *buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, (char *)buf + got, n - got);
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (r == 0) return (ssize_t)got;
    got += (size_t)r;
  }
  return (ssize_t)got;
}

static int write_n(int fd, const void *buf, size_t n) {
  size_t sent = 0;
  while (sent < n) {
    ssize_t w = write(fd, (const char *)buf + sent, n - sent);
    if (w < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    sent += (size_t)w;
  }
  return 0;
}

/* ---------------------------------------------------------------------------
 * Tagstruct writer
 * ---------------------------------------------------------------------------*/

struct ts {
  uint8_t *buf;
  size_t   len;
  size_t   cap;
};

static int ts_reserve(struct ts *t, size_t extra) {
  if (t->len + extra <= t->cap) return 0;
  size_t need = t->len + extra;
  size_t cap = t->cap ? t->cap : 64;
  while (cap < need) cap *= 2;
  uint8_t *nb = realloc(t->buf, cap);
  if (!nb) return -1;
  t->buf = nb;
  t->cap = cap;
  return 0;
}

static int ts_putu32(struct ts *t, uint32_t v) {
  if (ts_reserve(t, 5) < 0) return -1;
  t->buf[t->len++] = PA_TAG_U32;
  wr32be(t->buf + t->len, v);
  t->len += 4;
  return 0;
}

/* ---------------------------------------------------------------------------
 * Tagstruct reader
 * ---------------------------------------------------------------------------*/

struct tr {
  const uint8_t *buf;
  size_t   len;
  size_t   pos;
  int      err;
};

static int tr_read_u32(struct tr *r, uint32_t *out) {
  if (r->pos + 5 > r->len || r->buf[r->pos] != PA_TAG_U32) {
    r->err = 1;
    return -1;
  }
  *out = rd32be(r->buf + r->pos + 1);
  r->pos += 5;
  return 0;
}

static int tr_skip_value(struct tr *r) {
  if (r->pos >= r->len) { r->err = 1; return -1; }
  uint8_t tag = r->buf[r->pos++];
  switch (tag) {
  case PA_TAG_STRING_NULL:
  case PA_TAG_BOOLEAN_TRUE:
  case PA_TAG_BOOLEAN_FALSE:
    return 0;
  case PA_TAG_U32: case PA_TAG_VOLUME:
    if (r->pos + 4 > r->len) { r->err = 1; return -1; }
    r->pos += 4;
    return 0;
  case PA_TAG_U8:
    if (r->pos + 1 > r->len) { r->err = 1; return -1; }
    r->pos += 1;
    return 0;
  case PA_TAG_U64: case PA_TAG_S64: case PA_TAG_USEC: case PA_TAG_TIMEVAL:
    if (r->pos + 8 > r->len) { r->err = 1; return -1; }
    r->pos += 8;
    return 0;
  case PA_TAG_STRING: {
    while (r->pos < r->len && r->buf[r->pos] != 0) r->pos++;
    if (r->pos >= r->len) { r->err = 1; return -1; }
    r->pos++;
    return 0;
  }
  case PA_TAG_SAMPLE_SPEC:
    if (r->pos + 6 > r->len) { r->err = 1; return -1; }
    r->pos += 6;
    return 0;
  case PA_TAG_ARBITRARY: {
    if (r->pos + 4 > r->len) { r->err = 1; return -1; }
    uint32_t alen = rd32be(r->buf + r->pos);
    r->pos += 4;
    if (r->pos + alen > r->len) { r->err = 1; return -1; }
    r->pos += alen;
    return 0;
  }
  case PA_TAG_CHANNEL_MAP: {
    if (r->pos + 1 > r->len) { r->err = 1; return -1; }
    uint8_t n = r->buf[r->pos++];
    if (r->pos + n > r->len) { r->err = 1; return -1; }
    r->pos += n;
    return 0;
  }
  case PA_TAG_CVOLUME: {
    if (r->pos + 1 > r->len) { r->err = 1; return -1; }
    uint8_t n = r->buf[r->pos++];
    if (r->pos + 4u * n > r->len) { r->err = 1; return -1; }
    r->pos += 4u * n;
    return 0;
  }
  case PA_TAG_PROPLIST: {
    while (r->pos < r->len) {
      uint8_t t2 = r->buf[r->pos];
      if (t2 == PA_TAG_STRING_NULL) { r->pos++; return 0; }
      if (tr_skip_value(r) < 0) return -1;
      if (tr_skip_value(r) < 0) return -1;
      if (tr_skip_value(r) < 0) return -1;
    }
    r->err = 1;
    return -1;
  }
  case PA_TAG_FORMAT_INFO: {
    if (tr_skip_value(r) < 0) return -1;
    if (tr_skip_value(r) < 0) return -1;
    return 0;
  }
  default:
    r->err = 1;
    return -1;
  }
}

/* ---------------------------------------------------------------------------
 * Frame I/O
 * ---------------------------------------------------------------------------*/

static int send_control_frame(int fd, uint32_t channel, const uint8_t *body,
                              size_t body_len) {
  uint8_t hdr[PA_FRAME_HDR_SIZE];
  wr32be(hdr + 0, (uint32_t)body_len);
  wr32be(hdr + 4, channel);
  wr32be(hdr + 8, 0);
  wr32be(hdr + 12, 0);
  wr32be(hdr + 16, 0);
  if (write_n(fd, hdr, sizeof(hdr)) < 0) return -1;
  if (body_len && write_n(fd, body, body_len) < 0) return -1;
  return 0;
}

static int send_simple_reply(int fd, uint32_t tag) {
  struct ts t = {0};
  ts_putu32(&t, PA_COMMAND_REPLY);
  ts_putu32(&t, tag);
  int r = send_control_frame(fd, PA_NO_CHANNEL, t.buf, t.len);
  free(t.buf);
  return r;
}

static int send_error(int fd, uint32_t tag, uint32_t code) {
  struct ts t = {0};
  ts_putu32(&t, PA_COMMAND_ERROR);
  ts_putu32(&t, tag);
  ts_putu32(&t, code);
  int r = send_control_frame(fd, PA_NO_CHANNEL, t.buf, t.len);
  free(t.buf);
  return r;
}

/* ---------------------------------------------------------------------------
 * Host protocol I/O - our own wire format
 * ---------------------------------------------------------------------------*/

static int host_send(uint8_t type, uint8_t stream_id, uint16_t flags,
                     const void *payload, uint32_t payload_len) {
  struct ds_audio_frame_hdr h;
  h.type = type;
  h.stream_id = stream_id;
  h.flags = flags;
  h.payload_len = payload_len;
  if (write_n(g_host_fd, &h, sizeof(h)) < 0) return -1;
  if (payload_len && write_n(g_host_fd, payload, payload_len) < 0) return -1;
  return 0;
}

static int host_handshake(void) {
  struct ds_audio_hello hello = {DS_AUDIO_MAGIC, DS_AUDIO_PROTO_VER, 0};
  if (host_send(DS_AUDIO_MSG_HELLO, 0, 0, &hello, sizeof(hello)) < 0)
    return -1;

  struct ds_audio_frame_hdr h;
  if (read_n(g_host_fd, &h, sizeof(h)) != (ssize_t)sizeof(h))
    return -1;
  if (h.type != DS_AUDIO_MSG_HELLO_ACK) return -1;

  if (h.payload_len) {
    uint8_t scratch[64];
    uint32_t left = h.payload_len;
    while (left) {
      uint32_t chunk = left > sizeof(scratch) ? (uint32_t)sizeof(scratch) : left;
      if (read_n(g_host_fd, scratch, chunk) != (ssize_t)chunk) return -1;
      left -= chunk;
    }
  }
  return 0;
}

static uint8_t pulse_fmt_to_ours(uint8_t pa_fmt) {
  switch (pa_fmt) {
  case PA_SAMPLE_U8:        return DS_AUDIO_FMT_U8;
  case PA_SAMPLE_S16LE:     return DS_AUDIO_FMT_S16LE;
  case PA_SAMPLE_S32LE:     return DS_AUDIO_FMT_S32LE;
  case PA_SAMPLE_FLOAT32LE: return DS_AUDIO_FMT_F32LE;
  default:                  return DS_AUDIO_FMT_S16LE;
  }
}

/* ---------------------------------------------------------------------------
 * Stream/client housekeeping
 * ---------------------------------------------------------------------------*/

static struct ds_stream *alloc_stream(int client_idx, enum stream_dir dir) {
  for (int i = 0; i < MAX_STREAMS; i++) {
    if (!g_streams[i].in_use) {
      memset(&g_streams[i], 0, sizeof(g_streams[i]));
      g_streams[i].in_use = 1;
      g_streams[i].client_idx = client_idx;
      g_streams[i].dir = dir;
      g_streams[i].channel = g_next_channel++;
      g_streams[i].pulse_stream_index = g_streams[i].channel;
      g_streams[i].host_stream_id = (uint8_t)(i + 1);
      return &g_streams[i];
    }
  }
  return NULL;
}

static struct ds_stream *find_stream_by_channel(int client_idx, uint32_t ch) {
  for (int i = 0; i < MAX_STREAMS; i++) {
    if (g_streams[i].in_use && g_streams[i].client_idx == client_idx &&
        g_streams[i].channel == ch)
      return &g_streams[i];
  }
  return NULL;
}

static struct ds_stream *find_stream_by_host_id(uint8_t host_id) {
  for (int i = 0; i < MAX_STREAMS; i++) {
    if (g_streams[i].in_use && g_streams[i].host_stream_id == host_id)
      return &g_streams[i];
  }
  return NULL;
}

static void free_stream(struct ds_stream *s) {
  if (!s) return;
  host_send(DS_AUDIO_MSG_CLOSE, s->host_stream_id, 0, NULL, 0);
  s->in_use = 0;
}

static int send_playback_request(struct ds_pulse_client *c,
                                 struct ds_stream *s, uint32_t bytes) {
  struct ts t = {0};
  ts_putu32(&t, PA_COMMAND_REQUEST);
  ts_putu32(&t, (uint32_t)-1);
  ts_putu32(&t, s->channel);
  ts_putu32(&t, bytes);
  int r = send_control_frame(c->fd, PA_NO_CHANNEL, t.buf, t.len);
  free(t.buf);
  if (r == 0) s->bytes_requested += bytes;
  return r;
}

/* ---------------------------------------------------------------------------
 * Command handlers
 * ---------------------------------------------------------------------------*/

static int handle_auth(struct ds_pulse_client *c, struct tr *r, uint32_t tag) {
  uint32_t client_version = 0;
  if (tr_read_u32(r, &client_version) < 0)
    return send_error(c->fd, tag, PA_ERR_NOT_SUPPORTED);
  c->authed = 1;

  struct ts t = {0};
  ts_putu32(&t, PA_COMMAND_REPLY);
  ts_putu32(&t, tag);
  ts_putu32(&t, PA_PROTOCOL_VERSION);
  int rv = send_control_frame(c->fd, PA_NO_CHANNEL, t.buf, t.len);
  free(t.buf);
  return rv;
}

static int handle_set_client_name(struct ds_pulse_client *c, struct tr *r,
                                  uint32_t tag) {
  (void)r;
  c->client_index = (uint32_t)(c - g_clients) + 1;

  struct ts t = {0};
  ts_putu32(&t, PA_COMMAND_REPLY);
  ts_putu32(&t, tag);
  ts_putu32(&t, c->client_index);
  int rv = send_control_frame(c->fd, PA_NO_CHANNEL, t.buf, t.len);
  free(t.buf);
  return rv;
}

static int handle_create_playback(struct ds_pulse_client *c, struct tr *r,
                                  uint32_t tag) {
  if (!c->authed) return send_error(c->fd, tag, PA_ERR_NOT_SUPPORTED);

  if (r->pos + 7 > r->len || r->buf[r->pos] != PA_TAG_SAMPLE_SPEC)
    return send_error(c->fd, tag, PA_ERR_NOT_SUPPORTED);
  uint8_t pa_fmt = r->buf[r->pos + 1];
  uint8_t channels = r->buf[r->pos + 2];
  uint32_t rate = rd32be(r->buf + r->pos + 3);
  r->pos += 7;

  if (tr_skip_value(r) < 0)
    return send_error(c->fd, tag, PA_ERR_NOT_SUPPORTED);
  if (tr_skip_value(r) < 0)
    return send_error(c->fd, tag, PA_ERR_NOT_SUPPORTED);
  if (tr_skip_value(r) < 0)
    return send_error(c->fd, tag, PA_ERR_NOT_SUPPORTED);

  uint32_t maxlength = 0, tlength = 0;
  if (tr_read_u32(r, &maxlength) < 0)
    return send_error(c->fd, tag, PA_ERR_NOT_SUPPORTED);
  if (tr_read_u32(r, &tlength) < 0)
    return send_error(c->fd, tag, PA_ERR_NOT_SUPPORTED);

  if (tlength == 0 || tlength > 4 * 1024 * 1024) tlength = 64 * 1024;

  struct ds_stream *s = alloc_stream((int)(c - g_clients), STREAM_PLAYBACK);
  if (!s) return send_error(c->fd, tag, PA_ERR_NOT_SUPPORTED);
  s->sample_rate  = rate ? rate : 48000;
  s->channels     = channels ? channels : 2;
  s->format       = pulse_fmt_to_ours(pa_fmt);
  s->buffer_bytes = tlength;

  struct ds_audio_open_req req;
  req.sample_rate = s->sample_rate;
  req.channels    = s->channels;
  req.format      = s->format;
  req.buffer_ms   = 20;
  host_send(DS_AUDIO_MSG_OPEN_PLAY, s->host_stream_id, 0, &req, sizeof(req));

  struct ts t = {0};
  ts_putu32(&t, PA_COMMAND_REPLY);
  ts_putu32(&t, tag);
  ts_putu32(&t, s->channel);
  ts_putu32(&t, s->pulse_stream_index);
  ts_putu32(&t, tlength);
  ts_putu32(&t, maxlength ? maxlength : tlength * 4);
  ts_putu32(&t, tlength);
  ts_putu32(&t, tlength / 2);
  ts_putu32(&t, tlength / 8);

  int rv = send_control_frame(c->fd, PA_NO_CHANNEL, t.buf, t.len);
  free(t.buf);
  if (rv < 0) { free_stream(s); return -1; }

  send_playback_request(c, s, tlength);
  return 0;
}

static int handle_create_record(struct ds_pulse_client *c, struct tr *r,
                                uint32_t tag) {
  if (!c->authed) return send_error(c->fd, tag, PA_ERR_NOT_SUPPORTED);

  if (r->pos + 7 > r->len || r->buf[r->pos] != PA_TAG_SAMPLE_SPEC)
    return send_error(c->fd, tag, PA_ERR_NOT_SUPPORTED);
  uint8_t pa_fmt = r->buf[r->pos + 1];
  uint8_t channels = r->buf[r->pos + 2];
  uint32_t rate = rd32be(r->buf + r->pos + 3);
  r->pos += 7;

  struct ds_stream *s = alloc_stream((int)(c - g_clients), STREAM_RECORD);
  if (!s) return send_error(c->fd, tag, PA_ERR_NOT_SUPPORTED);
  s->sample_rate = rate ? rate : 48000;
  s->channels    = channels ? channels : 1;
  s->format      = pulse_fmt_to_ours(pa_fmt);
  s->buffer_bytes = 64 * 1024;

  struct ds_audio_open_req req;
  req.sample_rate = s->sample_rate;
  req.channels    = s->channels;
  req.format      = s->format;
  req.buffer_ms   = 20;
  host_send(DS_AUDIO_MSG_OPEN_REC, s->host_stream_id, 0, &req, sizeof(req));

  struct ts t = {0};
  ts_putu32(&t, PA_COMMAND_REPLY);
  ts_putu32(&t, tag);
  ts_putu32(&t, s->channel);
  ts_putu32(&t, s->pulse_stream_index);
  ts_putu32(&t, s->buffer_bytes);
  ts_putu32(&t, s->buffer_bytes / 2);

  int rv = send_control_frame(c->fd, PA_NO_CHANNEL, t.buf, t.len);
  free(t.buf);
  if (rv < 0) { free_stream(s); return -1; }
  return 0;
}

static int handle_delete_stream(struct ds_pulse_client *c, struct tr *r,
                                uint32_t tag) {
  uint32_t channel = 0;
  if (tr_read_u32(r, &channel) < 0)
    return send_error(c->fd, tag, PA_ERR_NOT_SUPPORTED);
  struct ds_stream *s = find_stream_by_channel((int)(c - g_clients), channel);
  if (s) free_stream(s);
  return send_simple_reply(c->fd, tag);
}

static int handle_drain(struct ds_pulse_client *c, struct tr *r, uint32_t tag) {
  (void)r;
  return send_simple_reply(c->fd, tag);
}

static int dispatch_control(struct ds_pulse_client *c, const uint8_t *body,
                            size_t body_len) {
  struct tr r = { body, body_len, 0, 0 };
  uint32_t cmd = 0, tag = 0;
  if (tr_read_u32(&r, &cmd) < 0) return -1;
  if (tr_read_u32(&r, &tag) < 0) return -1;

  switch (cmd) {
  case PA_COMMAND_AUTH:                  return handle_auth(c, &r, tag);
  case PA_COMMAND_SET_CLIENT_NAME:       return handle_set_client_name(c, &r, tag);
  case PA_COMMAND_CREATE_PLAYBACK_STREAM:return handle_create_playback(c, &r, tag);
  case PA_COMMAND_CREATE_RECORD_STREAM:  return handle_create_record(c, &r, tag);
  case PA_COMMAND_DELETE_PLAYBACK_STREAM:
  case PA_COMMAND_DELETE_RECORD_STREAM:  return handle_delete_stream(c, &r, tag);
  case PA_COMMAND_DRAIN_PLAYBACK_STREAM: return handle_drain(c, &r, tag);
  default:                               return send_error(c->fd, tag, PA_ERR_NOT_SUPPORTED);
  }
}

/* ---------------------------------------------------------------------------
 * Per-client read pump
 * ---------------------------------------------------------------------------*/

static void close_client(struct ds_pulse_client *c) {
  if (c->fd < 0) return;
  int idx = (int)(c - g_clients);
  for (int i = 0; i < MAX_STREAMS; i++) {
    if (g_streams[i].in_use && g_streams[i].client_idx == idx)
      free_stream(&g_streams[i]);
  }
  epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
  close(c->fd);
  free(c->payload);
  memset(c, 0, sizeof(*c));
  c->fd = -1;
}

static int client_consume(struct ds_pulse_client *c) {
  if (c->hdr_filled < PA_FRAME_HDR_SIZE) {
    ssize_t r = read(c->fd, c->hdr + c->hdr_filled,
                     PA_FRAME_HDR_SIZE - c->hdr_filled);
    if (r <= 0) return (r == 0) ? -1 : (errno == EAGAIN ? 0 : -1);
    c->hdr_filled += (size_t)r;
    if (c->hdr_filled < PA_FRAME_HDR_SIZE) return 0;

    uint32_t length = rd32be(c->hdr + 0);
    c->cur_channel = rd32be(c->hdr + 4);
    if (length > 16 * 1024 * 1024) return -1;
    if (length > c->payload_size) {
      uint8_t *nb = realloc(c->payload, length ? length : 1);
      if (!nb) return -1;
      c->payload = nb;
      c->payload_size = length;
    }
    c->payload_filled = 0;
    if (length == 0) goto frame_complete;
  }

  uint32_t need = rd32be(c->hdr + 0) - (uint32_t)c->payload_filled;
  if (need) {
    ssize_t r = read(c->fd, c->payload + c->payload_filled, need);
    if (r <= 0) return (r == 0) ? -1 : (errno == EAGAIN ? 0 : -1);
    c->payload_filled += (size_t)r;
    if (c->payload_filled < rd32be(c->hdr + 0)) return 0;
  }

frame_complete:
  if (c->cur_channel == PA_NO_CHANNEL) {
    if (dispatch_control(c, c->payload, c->payload_filled) < 0)
      return -1;
  } else {
    struct ds_stream *s = find_stream_by_channel((int)(c - g_clients),
                                                 c->cur_channel);
    if (s && s->dir == STREAM_PLAYBACK && c->payload_filled) {
      host_send(DS_AUDIO_MSG_PCM, s->host_stream_id, 0, c->payload,
                (uint32_t)c->payload_filled);
      if (s->bytes_requested > c->payload_filled)
        s->bytes_requested -= (uint32_t)c->payload_filled;
      else
        s->bytes_requested = 0;
      if (s->bytes_requested < s->buffer_bytes / 4)
        send_playback_request(c, s, s->buffer_bytes / 2);
    }
  }
  c->hdr_filled = 0;
  c->payload_filled = 0;
  return 0;
}

/* ---------------------------------------------------------------------------
 * Host -> client capture pump
 * ---------------------------------------------------------------------------*/

static int host_consume(void) {
  struct ds_audio_frame_hdr h;
  if (read_n(g_host_fd, &h, sizeof(h)) != (ssize_t)sizeof(h)) return -1;
  if (h.payload_len > 16 * 1024 * 1024) return -1;
  uint8_t *buf = NULL;
  if (h.payload_len) {
    buf = malloc(h.payload_len);
    if (!buf) return -1;
    if (read_n(g_host_fd, buf, h.payload_len) != (ssize_t)h.payload_len) {
      free(buf);
      return -1;
    }
  }
  switch (h.type) {
  case DS_AUDIO_MSG_PCM: {
    struct ds_stream *s = find_stream_by_host_id(h.stream_id);
    if (s && s->dir == STREAM_RECORD) {
      struct ds_pulse_client *c = &g_clients[s->client_idx];
      if (c->fd >= 0)
        send_control_frame(c->fd, s->channel, buf, h.payload_len);
    }
    break;
  }
  case DS_AUDIO_MSG_XRUN:
    break;
  default:
    break;
  }
  free(buf);
  return 0;
}

/* ---------------------------------------------------------------------------
 * Listener / main loop
 * ---------------------------------------------------------------------------*/

static int bind_listen_socket(void) {
  mkdir_p("/run/pulse", 0755);
  unlink(DS_AUDIO_PULSE_SOCK);

  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  safe_strncpy(addr.sun_path, DS_AUDIO_PULSE_SOCK, sizeof(addr.sun_path));
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  chmod(DS_AUDIO_PULSE_SOCK, 0666);
  if (listen(fd, 8) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int connect_host_socket(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  safe_strncpy(addr.sun_path, DS_AUDIO_HOST_SOCK, sizeof(addr.sun_path));
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int accept_client(void) {
  int fd = accept4(g_listen_fd, NULL, NULL, SOCK_CLOEXEC);
  if (fd < 0) return -1;
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (g_clients[i].fd < 0) {
      memset(&g_clients[i], 0, sizeof(g_clients[i]));
      g_clients[i].fd = fd;
      struct epoll_event ev;
      ev.events = EPOLLIN;
      ev.data.ptr = &g_clients[i];
      if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(fd);
        g_clients[i].fd = -1;
        return -1;
      }
      return 0;
    }
  }
  close(fd);
  return -1;
}

int pulse_gateway_main(int argc, char **argv) {
  (void)argc; (void)argv;

  for (int i = 0; i < MAX_CLIENTS; i++) g_clients[i].fd = -1;

  g_host_fd = connect_host_socket();
  if (g_host_fd < 0) {
    return 1;
  }
  if (host_handshake() < 0) {
    close(g_host_fd);
    return 1;
  }

  g_listen_fd = bind_listen_socket();
  if (g_listen_fd < 0) {
    close(g_host_fd);
    return 1;
  }

  g_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (g_epoll_fd < 0) goto fail;

  struct epoll_event ev;
  ev.events = EPOLLIN; ev.data.ptr = NULL;
  if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_listen_fd, &ev) < 0) goto fail;
  ev.events = EPOLLIN; ev.data.ptr = (void *)(intptr_t)1;
  if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_host_fd, &ev) < 0) goto fail;

  for (;;) {
    struct epoll_event events[16];
    int n = epoll_wait(g_epoll_fd, events, 16, -1);
    if (n < 0) {
      if (errno == EINTR) continue;
      goto fail;
    }
    for (int i = 0; i < n; i++) {
      void *p = events[i].data.ptr;
      if (p == NULL) {
        accept_client();
      } else if (p == (void *)(intptr_t)1) {
        if (host_consume() < 0) goto fail;
      } else {
        struct ds_pulse_client *c = p;
        if (client_consume(c) < 0) close_client(c);
      }
    }
  }

fail:
  if (g_epoll_fd >= 0) close(g_epoll_fd);
  if (g_listen_fd >= 0) close(g_listen_fd);
  if (g_host_fd >= 0) close(g_host_fd);
  return 1;
}
