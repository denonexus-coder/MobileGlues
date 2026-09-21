// MobileGlues - gl/diag_report.h
// Full Diagnostic Runtime Report — emitted once at init.
// Copyright (c) 2025-2026 MobileGL-Dev
// SPDX-License-Identifier: LGPL-2.1-only
#ifndef MOBILEGLUES_DIAG_REPORT_H
#define MOBILEGLUES_DIAG_REPORT_H

#ifdef __cplusplus
extern "C" {
#endif

// Emits the 10-section full diagnostic report. Idempotent, safe to call
// once after mg_v3_apply_settings() has populated global_settings.
// Reads only: global_settings, g_gles_caps, /proc/*, stat(), system properties.
// Does not allocate unbounded memory. Does not spawn processes.
void mg_diag_emit_full_report(void);

// Applies stability conflict rules R1-R4 in place on global_settings.
// Returns the number of rules that actually modified state.
int mg_diag_apply_conflict_rules(void);

// Max Vulkan version reported by any physical device.
// Returns 0 if Vulkan is absent; otherwise VK_MAKE_VERSION major*1000000+minor*1000+patch.
int mg_diag_max_vulkan_version(void);

#ifdef __cplusplus
}
#endif

#endif // MOBILEGLUES_DIAG_REPORT_H
