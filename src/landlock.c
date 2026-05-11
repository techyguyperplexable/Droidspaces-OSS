/*
 * Droidspaces v6 - High-performance Container Runtime
 *
 * Copyright (C) 2026 Josh Law <joshlaw48@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Landlock LSM sandbox.  Opt-in via --landlock.
 *
 * The jail mask in mount.c already shields /proc and /sys with bind-mounted
 * /dev/null overlays.  Those overlays survive only as long as nobody inside
 * the container can call umount() on them - which is fine until a CVE hands
 * the container CAP_SYS_ADMIN.  Landlock is the second wall: enforced at the
 * LSM hook, inherited across exec, and a process cannot relax its own
 * ruleset no matter what capabilities it picks up later.
 *
 * Direct syscalls (not libc wrappers) on purpose - we ship a static musl
 * binary and some musl releases predate the Landlock helpers.
 */

#include "droidspace.h"
#include <sys/syscall.h>

/* Landlock landed in 5.13 with these syscall numbers; they're stable across
 * every arch this project targets, so hardcoding is fine. */
#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#endif
#ifndef __NR_landlock_add_rule
#define __NR_landlock_add_rule 445
#endif
#ifndef __NR_landlock_restrict_self
#define __NR_landlock_restrict_self 446
#endif

#ifndef LANDLOCK_CREATE_RULESET_VERSION
#define LANDLOCK_CREATE_RULESET_VERSION (1U << 0)
#endif

/* Mirror the bits from <linux/landlock.h> here so we don't have to chase
 * kernel-header versions on every build host.  Grouped by the ABI they
 * shipped in. */
/* v1 (5.13) */
#define DS_LL_FS_EXECUTE     (1ULL << 0)
#define DS_LL_FS_WRITE_FILE  (1ULL << 1)
#define DS_LL_FS_READ_FILE   (1ULL << 2)
#define DS_LL_FS_READ_DIR    (1ULL << 3)
#define DS_LL_FS_REMOVE_DIR  (1ULL << 4)
#define DS_LL_FS_REMOVE_FILE (1ULL << 5)
#define DS_LL_FS_MAKE_CHAR   (1ULL << 6)
#define DS_LL_FS_MAKE_DIR    (1ULL << 7)
#define DS_LL_FS_MAKE_REG    (1ULL << 8)
#define DS_LL_FS_MAKE_SOCK   (1ULL << 9)
#define DS_LL_FS_MAKE_FIFO   (1ULL << 10)
#define DS_LL_FS_MAKE_BLOCK  (1ULL << 11)
#define DS_LL_FS_MAKE_SYM    (1ULL << 12)
/* v2 (5.19) */
#define DS_LL_FS_REFER       (1ULL << 13)
/* v3 (6.2) */
#define DS_LL_FS_TRUNCATE    (1ULL << 14)
/* v5 (6.10) */
#define DS_LL_FS_IOCTL_DEV   (1ULL << 15)

#define DS_LL_FS_V1_ALL                                                        \
  (DS_LL_FS_EXECUTE | DS_LL_FS_WRITE_FILE | DS_LL_FS_READ_FILE |               \
   DS_LL_FS_READ_DIR | DS_LL_FS_REMOVE_DIR | DS_LL_FS_REMOVE_FILE |            \
   DS_LL_FS_MAKE_CHAR | DS_LL_FS_MAKE_DIR | DS_LL_FS_MAKE_REG |                \
   DS_LL_FS_MAKE_SOCK | DS_LL_FS_MAKE_FIFO | DS_LL_FS_MAKE_BLOCK |             \
   DS_LL_FS_MAKE_SYM)

#define DS_LL_FS_READ_ONLY (DS_LL_FS_READ_FILE | DS_LL_FS_READ_DIR)

struct ds_ll_ruleset_attr {
  uint64_t handled_access_fs;
  uint64_t handled_access_net;
  uint64_t scoped;
};

struct ds_ll_path_beneath_attr {
  uint64_t allowed_access;
  int32_t parent_fd;
} __attribute__((packed));

enum ds_ll_rule_type {
  DS_LL_RULE_PATH_BENEATH = 1,
};

static inline long ds_ll_create_ruleset(const struct ds_ll_ruleset_attr *attr,
                                        size_t size, uint32_t flags) {
  return syscall(__NR_landlock_create_ruleset, attr, size, flags);
}

static inline long ds_ll_add_rule(int ruleset_fd, enum ds_ll_rule_type type,
                                  const void *attr, uint32_t flags) {
  return syscall(__NR_landlock_add_rule, ruleset_fd, type, attr, flags);
}

static inline long ds_ll_restrict_self(int ruleset_fd, uint32_t flags) {
  return syscall(__NR_landlock_restrict_self, ruleset_fd, flags);
}

int ds_landlock_supported(void) {
  long abi = ds_ll_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
  if (abi < 1)
    return 0;
  return (int)abi;
}

/* The kernel rejects unknown bits in handled_access_fs with EINVAL, so we
 * trim our wishlist down to what the running kernel actually shipped. */
static uint64_t ds_landlock_handled_mask(int abi) {
  uint64_t mask = DS_LL_FS_V1_ALL;
  if (abi >= 2)
    mask |= DS_LL_FS_REFER;
  if (abi >= 3)
    mask |= DS_LL_FS_TRUNCATE;
  if (abi >= 5)
    mask |= DS_LL_FS_IOCTL_DEV;
  return mask;
}

/* Open the path, hand the fd to the kernel, then forget about it.  Missing
 * paths are common on minimal rootfses (no /home, no /boot, etc.) - those
 * are just skipped, no warning. */
static int ds_landlock_allow_path(int ruleset_fd, const char *path,
                                  uint64_t requested, uint64_t handled) {
  int fd = open(path, O_PATH | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT)
      return 0;
    ds_warn("[SEC][landlock] open(%s): %s", path, strerror(errno));
    return -1;
  }

  struct ds_ll_path_beneath_attr pb = {
      .allowed_access = requested & handled,
      .parent_fd = fd,
  };
  long ret = ds_ll_add_rule(ruleset_fd, DS_LL_RULE_PATH_BENEATH, &pb, 0);
  int saved_errno = errno;
  close(fd);
  if (ret != 0) {
    ds_warn("[SEC][landlock] add_rule(%s): %s", path, strerror(saved_errno));
    return -1;
  }
  return 0;
}

/* Anything a normal Linux distro keeps user data and binaries in.  Full
 * rights on each - the container needs to behave like a real machine. */
static const char *const ds_landlock_rootfs_paths[] = {
    "/bin",  "/sbin", "/lib",   "/lib32", "/lib64", "/libx32", "/usr",
    "/etc",  "/var",  "/tmp",   "/root",  "/home",  "/opt",    "/srv",
    "/run",  "/mnt",  "/media", "/dev",   "/boot",
};

/* /sys subtrees that actually need to be writable.  /sys/fs covers cgroups
 * and fuse; the rest is hardware enumeration that programs like udev poke.
 * Anything under /sys NOT listed here inherits read-only from the /sys rule
 * below - which is the whole point: writes to /sys/kernel, /sys/firmware,
 * /sys/module, /sys/power are quietly killed even if someone unmounts the
 * jail-mask overlays sitting on top. */
static const char *const ds_landlock_sys_subtrees[] = {
    "/sys/class", "/sys/devices", "/sys/bus", "/sys/dev", "/sys/fs",
};

int ds_landlock_apply(const struct ds_config *cfg) {
  if (!cfg || !cfg->landlock)
    return 0;

  int abi = ds_landlock_supported();
  if (abi < 1) {
    ds_warn("[SEC][landlock] kernel does not support Landlock - skipping "
            "(--landlock requested)");
    return 0;
  }

  uint64_t handled = ds_landlock_handled_mask(abi);
  struct ds_ll_ruleset_attr attr = {
      .handled_access_fs = handled,
      .handled_access_net = 0,
      .scoped = 0,
  };

  long rs = ds_ll_create_ruleset(&attr, sizeof(attr), 0);
  if (rs < 0) {
    ds_warn("[SEC][landlock] create_ruleset: %s", strerror(errno));
    return -1;
  }
  int ruleset_fd = (int)rs;

  for (size_t i = 0;
       i < sizeof(ds_landlock_rootfs_paths) / sizeof(ds_landlock_rootfs_paths[0]);
       i++) {
    ds_landlock_allow_path(ruleset_fd, ds_landlock_rootfs_paths[i],
                           DS_LL_FS_V1_ALL | DS_LL_FS_REFER |
                               DS_LL_FS_TRUNCATE | DS_LL_FS_IOCTL_DEV,
                           handled);
  }

  /* /proc is namespace-isolated and full of dynamic per-PID dentries that
   * Landlock can't enumerate ahead of time.  Don't fight it - grant full
   * rights and let the jail mask handle the sharp edges in there. */
  ds_landlock_allow_path(ruleset_fd, "/proc",
                         DS_LL_FS_V1_ALL | DS_LL_FS_REFER | DS_LL_FS_TRUNCATE |
                             DS_LL_FS_IOCTL_DEV,
                         handled);

  /* Read everything in /sys, but only let the allowlist below promote to
   * read-write.  The kernel-config and firmware-blob corners stay read-only
   * by exclusion. */
  ds_landlock_allow_path(ruleset_fd, "/sys", DS_LL_FS_READ_ONLY, handled);
  for (size_t i = 0;
       i < sizeof(ds_landlock_sys_subtrees) / sizeof(ds_landlock_sys_subtrees[0]);
       i++) {
    ds_landlock_allow_path(ruleset_fd, ds_landlock_sys_subtrees[i],
                           DS_LL_FS_V1_ALL | DS_LL_FS_REFER |
                               DS_LL_FS_TRUNCATE | DS_LL_FS_IOCTL_DEV,
                           handled);
  }

  /* restrict_self() refuses to run without NO_NEW_PRIVS unless we hold
   * CAP_SYS_ADMIN in the init userns.  Always set it - cheap, idempotent,
   * and it kills setuid escalation as a bonus. */
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
    ds_warn("[SEC][landlock] prctl(NO_NEW_PRIVS): %s", strerror(errno));
    close(ruleset_fd);
    return -1;
  }

  if (ds_ll_restrict_self(ruleset_fd, 0) < 0) {
    ds_warn("[SEC][landlock] restrict_self: %s", strerror(errno));
    close(ruleset_fd);
    return -1;
  }

  close(ruleset_fd);
  ds_log("[SEC] Landlock filesystem sandbox active (ABI v%d).", abi);
  return 0;
}
