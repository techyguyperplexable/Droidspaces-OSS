/*
 * Droidspaces v6 - High-performance Container Runtime
 *
 * Copyright (C) 2026 Josh Law <joshlaw48@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Wire protocol between the in-container Pulse gateway and the Android
 * AudioBridge service.  Framed PCM over a unix stream socket.
 *
 * Endianness: everything little-endian.  Android phones are all LE in
 * practice and so are all archs Droidspaces targets, so we don't bother
 * with byte-swapping helpers.
 *
 * Stream lifecycle:
 *   client -> HELLO        -> server
 *   client <- HELLO_ACK    <- server
 *   client -> OPEN_PLAY    -> server    (or OPEN_REC)
 *   client <- OPEN_OK      <- server    (assigns stream_id, returns format)
 *   client <-> PCM <-> server           (data flows, possibly XRUN messages)
 *   client -> CLOSE        -> server
 *   client <- CLOSE_OK     <- server
 *
 * The server pushes capture frames at the negotiated rate; clients push
 * playback frames as they have them.  If a consumer can't keep up, the
 * sender drops frames and emits XRUN.
 */

#ifndef DS_AUDIO_PROTO_H
#define DS_AUDIO_PROTO_H

#include <stdint.h>

#define DS_AUDIO_MAGIC       0x55415344u /* "DSAU" little-endian */
#define DS_AUDIO_PROTO_VER   1

/* Default socket paths */
#define DS_AUDIO_HOST_SOCK   "/run/droidspaces/audio.host.sock"
#define DS_AUDIO_PULSE_SOCK  "/run/pulse/native"

/* Frame header.  Every message on the wire starts with this. */
struct ds_audio_frame_hdr {
  uint8_t  type;        /* DS_AUDIO_MSG_* */
  uint8_t  stream_id;   /* 0 for global messages, 1-255 per-stream */
  uint16_t flags;       /* reserved, message-specific */
  uint32_t payload_len; /* bytes immediately following this header */
};

/* Message types */
enum {
  DS_AUDIO_MSG_HELLO     = 0x01, /* client -> server */
  DS_AUDIO_MSG_HELLO_ACK = 0x02, /* server -> client */

  DS_AUDIO_MSG_OPEN_PLAY = 0x10, /* client -> server, payload: open_req */
  DS_AUDIO_MSG_OPEN_REC  = 0x11, /* client -> server, payload: open_req */
  DS_AUDIO_MSG_OPEN_OK   = 0x12, /* server -> client, payload: open_ok */
  DS_AUDIO_MSG_OPEN_FAIL = 0x13, /* server -> client, payload: u32 errno */

  DS_AUDIO_MSG_CLOSE     = 0x20, /* either direction */
  DS_AUDIO_MSG_CLOSE_OK  = 0x21, /* peer ack */

  DS_AUDIO_MSG_PCM       = 0x30, /* bidir, payload = raw samples */

  DS_AUDIO_MSG_XRUN      = 0x40, /* server -> client, payload: xrun_info */

  DS_AUDIO_MSG_PING      = 0x50, /* either direction */
  DS_AUDIO_MSG_PONG      = 0x51,
};

/* Sample formats */
enum {
  DS_AUDIO_FMT_S16LE = 0, /* default */
  DS_AUDIO_FMT_S32LE = 1,
  DS_AUDIO_FMT_F32LE = 2,
  DS_AUDIO_FMT_U8    = 3,
};

/* HELLO payload */
struct ds_audio_hello {
  uint32_t magic;   /* DS_AUDIO_MAGIC */
  uint16_t version; /* DS_AUDIO_PROTO_VER */
  uint16_t flags;   /* reserved */
};

/* HELLO_ACK payload */
struct ds_audio_hello_ack {
  uint32_t magic;        /* DS_AUDIO_MAGIC echoed */
  uint16_t server_ver;   /* highest version server speaks */
  uint16_t caps;         /* DS_AUDIO_CAP_* bits */
};

#define DS_AUDIO_CAP_PLAYBACK (1u << 0)
#define DS_AUDIO_CAP_CAPTURE  (1u << 1)
#define DS_AUDIO_CAP_MIXING   (1u << 2)

/* OPEN_PLAY / OPEN_REC payload */
struct ds_audio_open_req {
  uint32_t sample_rate; /* Hz, e.g. 48000 */
  uint8_t  channels;    /* 1 = mono, 2 = stereo */
  uint8_t  format;      /* DS_AUDIO_FMT_* */
  uint16_t buffer_ms;   /* requested buffer depth in ms, server may adjust */
};

/* OPEN_OK payload - server confirms the negotiated format and assigns id */
struct ds_audio_open_ok {
  uint8_t  stream_id;
  uint8_t  channels;    /* final */
  uint8_t  format;      /* final */
  uint8_t  reserved;
  uint32_t sample_rate; /* final */
  uint16_t buffer_ms;   /* final */
  uint16_t reserved2;
};

/* XRUN notification */
struct ds_audio_xrun {
  uint32_t dropped_frames; /* number of PCM frames lost since last XRUN */
};

/* Helper - bytes per sample for a given format */
static inline uint32_t ds_audio_bytes_per_sample(uint8_t format) {
  switch (format) {
  case DS_AUDIO_FMT_S16LE: return 2;
  case DS_AUDIO_FMT_S32LE: return 4;
  case DS_AUDIO_FMT_F32LE: return 4;
  case DS_AUDIO_FMT_U8:    return 1;
  default:                 return 0;
  }
}

#endif /* DS_AUDIO_PROTO_H */
