/*
 * Droidspaces v6 - High-performance Container Runtime
 *
 * Copyright (C) 2026 Josh Law <joshlaw48@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Audio bridge - CLI side.
 *
 * The Android app hosts a unix socket from its foreground AudioBridgeService.
 * We bind-mount that socket into the container, then fork off the in-process
 * Pulse gateway (a subcommand of this same binary) which translates between
 * what libpulse clients in the container expect and our custom wire protocol.
 *
 * Linux desktop: there is no Android app, so --audio is a no-op with a clear
 * warning rather than a hard error.  Keeps the same binary working in both
 * environments.
 */

#include "audio_proto.h"
#include "droidspace.h"

/* Where the Android AudioBridgeService creates its listening socket.
 * Matches the app's package name (com.droidspaces.app) and the standard
 * Android app-private files dir. */
#define DS_AUDIO_ANDROID_PKG  "com.droidspaces.app"
#define DS_AUDIO_ANDROID_SOCK "/data/data/" DS_AUDIO_ANDROID_PKG "/files/audio.sock"

int ds_audio_get_host_socket(char *out, size_t size) {
  if (!is_android()) {
    return -1;
  }
  int n = snprintf(out, size, "%s", DS_AUDIO_ANDROID_SOCK);
  if (n < 0 || (size_t)n >= size)
    return -1;
  return 0;
}

/* Make sure /run/droidspaces and /run/pulse exist in the container, then
 * place the host socket at /run/droidspaces/audio.host.sock via a bind
 * mount.  The bind target is a 0-byte regular file we create as the mount
 * anchor - mount(2) needs the destination to exist. */
int ds_audio_setup(const struct ds_config *cfg) {
  if (!cfg || !cfg->audio)
    return 0;

  if (!is_android()) {
    ds_warn("[AUDIO] --audio is Android-only (no bridge service on Linux "
            "desktop); skipping");
    return 0;
  }

  char host[PATH_MAX];
  if (ds_audio_get_host_socket(host, sizeof(host)) < 0) {
    ds_warn("[AUDIO] failed to resolve host socket path");
    return -1;
  }

  if (access(host, F_OK) != 0) {
    ds_warn("[AUDIO] host socket %s missing - is the AudioBridgeService "
            "running in the app? Audio disabled for this boot.",
            host);
    return -1;
  }

  if (mkdir_p("/run/droidspaces", 0755) < 0 && errno != EEXIST) {
    ds_warn("[AUDIO] mkdir /run/droidspaces: %s", strerror(errno));
    return -1;
  }
  if (mkdir_p("/run/pulse", 0755) < 0 && errno != EEXIST) {
    ds_warn("[AUDIO] mkdir /run/pulse: %s", strerror(errno));
    return -1;
  }

  /* Create the anchor file the bind mount lands on. */
  int afd = open(DS_AUDIO_HOST_SOCK, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (afd < 0) {
    ds_warn("[AUDIO] create anchor %s: %s", DS_AUDIO_HOST_SOCK,
            strerror(errno));
    return -1;
  }
  close(afd);

  if (bind_mount(host, DS_AUDIO_HOST_SOCK) < 0) {
    ds_warn("[AUDIO] bind_mount(%s -> %s): %s", host, DS_AUDIO_HOST_SOCK,
            strerror(errno));
    unlink(DS_AUDIO_HOST_SOCK);
    return -1;
  }

  ds_log("[AUDIO] host socket mounted at %s", DS_AUDIO_HOST_SOCK);
  return 0;
}

/* Append PULSE_SERVER to /run/droidspaces.env so every shell that sources
 * profile.d/droidspaces_env.sh picks it up.  Also set it in the current env
 * so init's children inherit immediately. */
void ds_audio_apply_env(const struct ds_config *cfg) {
  if (!cfg || !cfg->audio || !is_android())
    return;
  FILE *f = fopen("/run/droidspaces.env", "a");
  if (f) {
    fprintf(f, "PULSE_SERVER=unix:%s\n", DS_AUDIO_PULSE_SOCK);
    fclose(f);
  }
  setenv("PULSE_SERVER", "unix:" DS_AUDIO_PULSE_SOCK, 1);
}

/* Fork a detached gateway daemon.  It re-execs /proc/self/exe with the
 * internal __pulse-gateway subcommand, which keeps all the protocol code in
 * pulse_gateway.c instead of bloating this file. */
int ds_audio_spawn_gateway(const struct ds_config *cfg) {
  if (!cfg || !cfg->audio || !is_android())
    return 0;
  if (access(DS_AUDIO_HOST_SOCK, F_OK) != 0) {
    /* Setup failed earlier - don't spawn a gateway with nothing to talk to. */
    return 0;
  }

  pid_t pid = fork();
  if (pid < 0) {
    ds_warn("[AUDIO] fork gateway: %s", strerror(errno));
    return -1;
  }
  if (pid == 0) {
    /* Detach from the container's controlling terminal so init doesn't reap
     * us as a foreground child.  setsid + double-fork. */
    if (setsid() < 0)
      _exit(127);
    pid_t p2 = fork();
    if (p2 < 0)
      _exit(127);
    if (p2 > 0)
      _exit(0);

    /* Grandchild: close stdio, exec the gateway subcommand. */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, 0);
      dup2(devnull, 1);
      dup2(devnull, 2);
      if (devnull > 2)
        close(devnull);
    }
    char *const argv[] = {(char *)"droidspaces", (char *)"__pulse-gateway",
                          NULL};
    execv("/proc/self/exe", argv);
    _exit(127);
  }

  /* Reap the intermediate fork so we don't leave a zombie behind. */
  int status;
  waitpid(pid, &status, 0);
  ds_log("[AUDIO] Pulse gateway spawned");
  return 0;
}
