/*
 * Droidspaces v6 - High-performance Container Runtime
 *
 * Copyright (C) 2026 Josh Law <joshlaw48@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "droidspace.h"
#include <linux/capability.h>

struct ds_cap_rule {
  int cap;
  const char *name;
  const char *reason;
};

static const struct ds_cap_rule universal_drops[] = {
    {CAP_SYS_MODULE, "CAP_SYS_MODULE", "kernel module loading"},
    {-1, NULL, NULL},
};

static const struct ds_cap_rule standard_drops[] = {
    {CAP_SYS_RAWIO, "CAP_SYS_RAWIO", "raw hardware access"},
    {CAP_SYS_PTRACE, "CAP_SYS_PTRACE", "cross-namespace tracing"},
    {CAP_SYS_PACCT, "CAP_SYS_PACCT", "host process accounting"},
    {CAP_MAC_ADMIN, "CAP_MAC_ADMIN", "MAC policy changes"},
    {CAP_MAC_OVERRIDE, "CAP_MAC_OVERRIDE", "MAC policy bypass"},
    {CAP_WAKE_ALARM, "CAP_WAKE_ALARM", "host wake alarm control"},
    {CAP_BLOCK_SUSPEND, "CAP_BLOCK_SUSPEND", "host suspend blocking"},
    {CAP_AUDIT_READ, "CAP_AUDIT_READ", "kernel audit log access"},
    {CAP_DAC_READ_SEARCH, "CAP_DAC_READ_SEARCH",
     "host filesystem handle reads"},
    {-1, NULL, NULL},
};

static int drop_capability_rule(const struct ds_cap_rule *rule) {
  if (prctl(PR_CAPBSET_DROP, rule->cap, 0, 0, 0) < 0) {
    if (errno != EINVAL)
      ds_warn("[SEC] Failed to drop %s: %s", rule->name, strerror(errno));
    return 0;
  }

  return 1;
}

static int drop_capability_rules(const struct ds_cap_rule *rules) {
  int total = 0;

  for (int i = 0; rules[i].cap != -1; i++)
    total += drop_capability_rule(&rules[i]);

  return total;
}

/*
 * Drops dangerous capabilities from the bounding set to reduce the
 * container's attack surface.
 */
void ds_apply_capability_hardening(int hw_access, int privileged_mask) {
  if (privileged_mask & DS_PRIV_NOCAPS) {
    ds_log("[SEC] --privileged=nocaps: skipping capability drops.");
    return;
  }

  int total_dropped = drop_capability_rules(universal_drops);

  if (hw_access) {
    ds_log("[SEC] Hardware Mode: preserved bounding set (dropped %d universal "
           "caps).",
           total_dropped);
    return;
  }

  total_dropped += drop_capability_rules(standard_drops);
  ds_log("[SEC] Bounding set hardened (dropped %d caps).", total_dropped);
}

static void print_rule_list(const char *title, const struct ds_cap_rule *rules,
                            int enabled) {
  printf("\n" C_BOLD "%s" C_RESET "\n", title);

  for (int i = 0; rules[i].cap != -1; i++) {
    printf("  %s%-20s%s %s\n", enabled ? C_RED : C_DIM, rules[i].name,
           C_RESET, rules[i].reason);
  }
}

int ds_capabilities_show(int hw_access, int privileged_mask) {
  int nocaps = !!(privileged_mask & DS_PRIV_NOCAPS);
  const char *profile =
      nocaps ? "nocaps" : (hw_access ? "hardware" : "standard");

  printf(C_BOLD "Droidspaces capability policy" C_RESET "\n");
  printf("Profile: %s\n", profile);
  printf("Applied to: container init, enter sessions, and run commands\n");

  if (nocaps) {
    printf("\n" C_YELLOW
           "Capability bounding-set drops are disabled by --privileged=nocaps."
           C_RESET "\n");
    print_rule_list("Normally dropped in every mode", universal_drops, 0);
    print_rule_list("Normally dropped in standard mode", standard_drops, 0);
    return 0;
  }

  print_rule_list("Dropped in every mode", universal_drops, 1);

  if (hw_access) {
    printf("\n" C_BOLD "Preserved in hardware mode" C_RESET "\n");
    printf("  Most other capabilities stay available so direct hardware access "
           "keeps working.\n");
    printf("  CAP_SYS_BOOT is preserved so reboot(2) works inside the PID "
           "namespace.\n");
    print_rule_list("Dropped only in standard mode", standard_drops, 0);
    return 0;
  }

  print_rule_list("Dropped in standard mode", standard_drops, 1);
  printf("\n" C_BOLD "Preserved by design" C_RESET "\n");
  printf("  CAP_SYS_BOOT       in-container reboot inside the PID namespace\n");

  return 0;
}
