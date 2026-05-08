/*
 * Droidspaces v6 - High-performance Container Runtime
 *
 * Read-only workspace diagnostics.
 *
 * Copyright (C) 2026 Josh Law <joshlaw48@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "droidspace.h"

struct doctor_ip_owner {
  char ip[INET_ADDRSTRLEN];
  char name[256];
};

struct doctor_ctx {
  int issues;
  int warnings;
  int config_count;
  int pidfile_count;
  int running_count;
  int lock_count;
  int pf_state_count;
  struct doctor_ip_owner ip_owners[DS_MAX_CONTAINERS];
  int ip_owner_count;
};

static void doctor_vstatus(const char *label, const char *color,
                           const char *fmt, va_list ap) {
  printf("  %s%s%s ", color, label, C_RESET);
  vprintf(fmt, ap);
  putchar('\n');
}

static void doctor_ok(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  doctor_vstatus("OK  ", C_GREEN, fmt, ap);
  va_end(ap);
}

static void doctor_warn(struct doctor_ctx *ctx, const char *fmt, ...) {
  if (ctx)
    ctx->warnings++;

  va_list ap;
  va_start(ap, fmt);
  doctor_vstatus("WARN", C_YELLOW, fmt, ap);
  va_end(ap);
}

static void doctor_fail(struct doctor_ctx *ctx, const char *fmt, ...) {
  if (ctx)
    ctx->issues++;

  va_list ap;
  va_start(ap, fmt);
  doctor_vstatus("FAIL", C_RED, fmt, ap);
  va_end(ap);
}

static int path_is_dir(const char *path) {
  struct stat st;
  return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

static int path_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static int join_path2(char *out, size_t size, const char *a, const char *b) {
  size_t alen, blen, slash;

  if (!out || !a || !b || size == 0)
    return -1;

  alen = strlen(a);
  blen = strlen(b);
  slash = (alen > 0 && a[alen - 1] == '/') ? 0 : 1;

  if (alen + slash + blen + 1 > size)
    return -1;

  memcpy(out, a, alen);
  if (slash)
    out[alen++] = '/';
  memcpy(out + alen, b, blen + 1);
  return 0;
}

static int join_path3(char *out, size_t size, const char *a, const char *b,
                      const char *c) {
  char tmp[PATH_MAX];
  if (join_path2(tmp, sizeof(tmp), a, b) < 0)
    return -1;
  return join_path2(out, size, tmp, c);
}

static int has_suffix(const char *s, const char *suffix) {
  size_t slen, xlen;

  if (!s || !suffix)
    return 0;

  slen = strlen(s);
  xlen = strlen(suffix);
  return slen >= xlen && strcmp(s + slen - xlen, suffix) == 0;
}

static int read_pid_value(const char *path, pid_t *pid_out) {
  char buf[64];
  char *end = NULL;
  long val;

  if (pid_out)
    *pid_out = 0;

  if (read_file(path, buf, sizeof(buf)) <= 0)
    return -1;

  errno = 0;
  val = strtol(buf, &end, 10);
  if (errno != 0 || !end || *end != '\0' || val <= 0)
    return -1;

  if (pid_out)
    *pid_out = (pid_t)val;
  return 0;
}

static void strip_suffix(char *dst, size_t size, const char *src,
                         const char *suffix) {
  size_t slen = strlen(src);
  size_t xlen = strlen(suffix);
  size_t n = (slen >= xlen) ? slen - xlen : slen;

  if (size == 0)
    return;
  if (n >= size)
    n = size - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static int ip_owner_index(struct doctor_ctx *ctx, const char *ip) {
  for (int i = 0; i < ctx->ip_owner_count; i++) {
    if (strcmp(ctx->ip_owners[i].ip, ip) == 0)
      return i;
  }
  return -1;
}

static void remember_static_ip(struct doctor_ctx *ctx, const char *name,
                               const char *ip) {
  int idx;

  if (!ip || !ip[0])
    return;

  idx = ip_owner_index(ctx, ip);
  if (idx >= 0) {
    doctor_fail(ctx, "static NAT IP %s is assigned to both '%s' and '%s'", ip,
                ctx->ip_owners[idx].name, name);
    return;
  }

  if (ctx->ip_owner_count >= DS_MAX_CONTAINERS) {
    doctor_warn(ctx, "too many static NAT IP owners to track completely");
    return;
  }

  safe_strncpy(ctx->ip_owners[ctx->ip_owner_count].ip, ip,
               sizeof(ctx->ip_owners[ctx->ip_owner_count].ip));
  safe_strncpy(ctx->ip_owners[ctx->ip_owner_count].name, name,
               sizeof(ctx->ip_owners[ctx->ip_owner_count].name));
  ctx->ip_owner_count++;
}

static int static_ip_is_known(struct doctor_ctx *ctx, const char *ip) {
  return ip_owner_index(ctx, ip) >= 0;
}

static void check_workspace_dirs(struct doctor_ctx *ctx, char *containers_dir,
                                 size_t containers_size) {
  const char *workspace = get_workspace_dir();
  char path[PATH_MAX];

  printf(C_BOLD "Workspace" C_RESET "\n");

  if (!path_is_dir(workspace)) {
    doctor_ok("workspace is not initialized: %s", workspace);
    return;
  }

  doctor_ok("workspace: %s", workspace);

  if (join_path2(containers_dir, containers_size, workspace, "Containers") < 0) {
    doctor_fail(ctx, "Containers path is too long under %s", workspace);
    containers_dir[0] = '\0';
  }

  if (containers_dir[0]) {
    if (path_is_dir(containers_dir))
      doctor_ok("containers directory present");
    else
      doctor_warn(ctx, "containers directory is missing: %s", containers_dir);
  }

  if (path_is_dir(get_pids_dir()))
    doctor_ok("pid directory present");
  else
    doctor_warn(ctx, "pid directory is missing: %s", get_pids_dir());

  if (path_is_dir(get_net_dir()))
    doctor_ok("network state directory present");
  else
    doctor_warn(ctx, "network state directory is missing: %s", get_net_dir());

  if (path_is_dir(get_logs_dir()))
    doctor_ok("log directory present");
  else
    doctor_warn(ctx, "log directory is missing: %s", get_logs_dir());

  if (join_path2(path, sizeof(path), workspace, DS_VOLATILE_SUBDIR) == 0 &&
      path_exists(path) && !path_is_dir(path))
    doctor_fail(ctx, "volatile state path exists but is not a directory: %s",
                path);
}

static void check_configs(struct doctor_ctx *ctx, const char *containers_dir) {
  DIR *d;
  struct dirent *ent;

  printf("\n" C_BOLD "Configs" C_RESET "\n");

  if (!containers_dir || !containers_dir[0] || !path_is_dir(containers_dir)) {
    doctor_warn(ctx, "no container config directory to inspect");
    return;
  }

  d = opendir(containers_dir);
  if (!d) {
    doctor_fail(ctx, "cannot open %s: %s", containers_dir, strerror(errno));
    return;
  }

  while ((ent = readdir(d)) != NULL) {
    char config_path[PATH_MAX];
    struct ds_config cfg;
    char safe_name[256];

    if (ent->d_name[0] == '.')
      continue;

    if (join_path3(config_path, sizeof(config_path), containers_dir,
                   ent->d_name, "container.config") < 0) {
      doctor_fail(ctx, "config path for '%s' is too long", ent->d_name);
      continue;
    }

    if (!path_exists(config_path)) {
      doctor_warn(ctx, "missing config for container directory '%s'",
                  ent->d_name);
      continue;
    }

    memset(&cfg, 0, sizeof(cfg));
    if (ds_config_load(config_path, &cfg) < 0) {
      doctor_fail(ctx, "failed to load %s", config_path);
      continue;
    }

    ctx->config_count++;

    if (!cfg.container_name[0]) {
      doctor_fail(ctx, "%s has no valid container name", config_path);
    } else {
      sanitize_container_name(cfg.container_name, safe_name, sizeof(safe_name));
      if (strcmp(safe_name, ent->d_name) != 0) {
        doctor_warn(ctx, "%s name '%s' maps to directory '%s'", config_path,
                    cfg.container_name, safe_name);
      }
      remember_static_ip(ctx, cfg.container_name, cfg.static_nat_ip);
    }

    if (cfg.rootfs_path[0] && !path_exists(cfg.rootfs_path))
      doctor_warn(ctx, "%s rootfs_path is missing: %s", cfg.container_name,
                  cfg.rootfs_path);
    if (cfg.rootfs_img_path[0] && !path_exists(cfg.rootfs_img_path))
      doctor_warn(ctx, "%s rootfs image is missing: %s", cfg.container_name,
                  cfg.rootfs_img_path);

    free_config_binds(&cfg);
    free_config_env_vars(&cfg);
    free_config_unknown_lines(&cfg);
  }

  closedir(d);

  if (ctx->config_count == 0)
    doctor_warn(ctx, "no container configs found");
  else
    doctor_ok("%d container config(s) inspected", ctx->config_count);
}

static void check_pidfiles(struct doctor_ctx *ctx, const char *containers_dir) {
  DIR *d;
  struct dirent *ent;

  printf("\n" C_BOLD "Runtime Metadata" C_RESET "\n");

  d = opendir(get_pids_dir());
  if (!d) {
    doctor_warn(ctx, "no pid directory to inspect");
    return;
  }

  while ((ent = readdir(d)) != NULL) {
    char pid_path[PATH_MAX];
    char name[256];
    char config_path[PATH_MAX];
    pid_t pid = 0;

    if (ent->d_name[0] == '.' || !has_suffix(ent->d_name, DS_EXT_PID))
      continue;

    ctx->pidfile_count++;
    strip_suffix(name, sizeof(name), ent->d_name, DS_EXT_PID);

    if (!validate_container_name(name))
      doctor_fail(ctx, "pidfile has invalid container name: %s", ent->d_name);

    if (join_path2(pid_path, sizeof(pid_path), get_pids_dir(), ent->d_name) <
        0) {
      doctor_fail(ctx, "pidfile path is too long: %s", ent->d_name);
      continue;
    }

    if (read_pid_value(pid_path, &pid) < 0) {
      doctor_warn(ctx, "pidfile %s contains an invalid PID", pid_path);
      continue;
    }

    if (kill(pid, 0) < 0 && errno == ESRCH) {
      doctor_warn(ctx, "pidfile %s points to dead PID %d", pid_path, pid);
      continue;
    }

    if (!is_valid_container_pid(pid)) {
      doctor_warn(ctx, "pidfile %s points to non-Droidspaces PID %d", pid_path,
                  pid);
      continue;
    }

    ctx->running_count++;

    if (containers_dir && containers_dir[0] &&
        join_path3(config_path, sizeof(config_path), containers_dir, name,
                   "container.config") == 0 &&
        !path_exists(config_path)) {
      doctor_warn(ctx, "running container '%s' has no saved config", name);
    }
  }

  closedir(d);

  if (ctx->pidfile_count == 0)
    doctor_ok("no pidfiles present");
  else
    doctor_ok("%d pidfile(s), %d live Droidspaces container(s)",
              ctx->pidfile_count, ctx->running_count);
}

static void check_locks(struct doctor_ctx *ctx) {
  DIR *d;
  struct dirent *ent;

  d = opendir(get_pids_dir());
  if (!d)
    return;

  while ((ent = readdir(d)) != NULL) {
    char lock_path[PATH_MAX];
    char pid_path[PATH_MAX];
    char name[256];
    pid_t holder = 0;

    if (ent->d_name[0] == '.' || !has_suffix(ent->d_name, DS_EXT_LOCK))
      continue;

    ctx->lock_count++;
    strip_suffix(name, sizeof(name), ent->d_name, DS_EXT_LOCK);

    if (join_path2(lock_path, sizeof(lock_path), get_pids_dir(), ent->d_name) <
        0) {
      doctor_fail(ctx, "lock path is too long: %s", ent->d_name);
      continue;
    }

    if (path_is_dir(lock_path)) {
      if (join_path2(pid_path, sizeof(pid_path), lock_path, "pid") < 0 ||
          read_pid_value(pid_path, &holder) < 0) {
        doctor_warn(ctx, "lock for '%s' has no readable holder PID", name);
        continue;
      }
    } else if (read_pid_value(lock_path, &holder) < 0) {
      doctor_warn(ctx, "legacy lock for '%s' has no readable holder PID", name);
      continue;
    }

    if (kill(holder, 0) < 0 && errno == ESRCH)
      doctor_warn(ctx, "lock for '%s' is stale; holder PID %d is dead", name,
                  holder);
  }

  closedir(d);

  if (ctx->lock_count == 0)
    doctor_ok("no external command locks present");
  else
    doctor_ok("%d external command lock(s) inspected", ctx->lock_count);
}

static void check_portforward_state(struct doctor_ctx *ctx) {
  DIR *d;
  struct dirent *ent;

  printf("\n" C_BOLD "Network State" C_RESET "\n");

  d = opendir(get_net_dir());
  if (!d) {
    doctor_warn(ctx, "no network state directory to inspect");
    return;
  }

  while ((ent = readdir(d)) != NULL) {
    char ip[INET_ADDRSTRLEN];
    size_t len;

    if (ent->d_name[0] == '.' || strncmp(ent->d_name, "pf_", 3) != 0 ||
        !has_suffix(ent->d_name, ".state"))
      continue;

    ctx->pf_state_count++;
    len = strlen(ent->d_name) - strlen("pf_") - strlen(".state");
    if (len == 0 || len >= sizeof(ip)) {
      doctor_warn(ctx, "port-forward state file has invalid name: %s",
                  ent->d_name);
      continue;
    }

    memcpy(ip, ent->d_name + strlen("pf_"), len);
    ip[len] = '\0';

    if (!static_ip_is_known(ctx, ip)) {
      doctor_warn(ctx, "port-forward state %s has no matching container config",
                  ent->d_name);
    }
  }

  closedir(d);

  if (ctx->pf_state_count == 0)
    doctor_ok("no port-forward state files present");
  else
    doctor_ok("%d port-forward state file(s) inspected", ctx->pf_state_count);
}

int ds_doctor_run(void) {
  struct doctor_ctx *ctx = calloc(1, sizeof(*ctx));
  char containers_dir[PATH_MAX] = {0};

  if (!ctx) {
    ds_error("doctor: out of memory");
    return 1;
  }

  printf(C_BOLD "%s doctor" C_RESET "\n\n", DS_PROJECT_NAME);

  check_workspace_dirs(ctx, containers_dir, sizeof(containers_dir));
  if (!path_is_dir(get_workspace_dir())) {
    printf("\n" C_GREEN "No issues found." C_RESET "\n");
    free(ctx);
    return 0;
  }

  check_configs(ctx, containers_dir);
  check_pidfiles(ctx, containers_dir);
  check_locks(ctx);
  check_portforward_state(ctx);

  printf("\n" C_BOLD "Summary" C_RESET "\n");
  printf("  configs: %d\n", ctx->config_count);
  printf("  running: %d\n", ctx->running_count);
  printf("  warnings: %d\n", ctx->warnings);
  printf("  issues: %d\n", ctx->issues);

  if (ctx->issues > 0) {
    printf("\n" C_RED "Doctor found issues." C_RESET "\n");
    free(ctx);
    return 1;
  }

  printf("\n" C_GREEN "Doctor completed without hard failures." C_RESET "\n");
  free(ctx);
  return 0;
}
