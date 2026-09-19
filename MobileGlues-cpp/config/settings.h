// MobileGlues - config/settings.h
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_PLUGIN_SETTINGS_H
#define MOBILEGLUES_PLUGIN_SETTINGS_H

#include <cstddef>
#include <string>

#define DEFAULT_GL_VERSION 40

enum class AngleConfig : int {
    DisableIfPossible = 0,
    EnableIfPossible  = 1,
    ForceDisable      = 2,
    ForceEnable       = 3
};

enum class AngleMode : int { Disabled = 0, Enabled = 1 };

enum class IgnoreErrorLevel : int { None = 0, Partial = 1, Full = 2 };

enum class NoErrorConfig : int {
    Auto = 0,
    DoNotIgnore = 1,
    IgnoreShaderProgram = 2,
    IgnoreShaderProgramFramebuffer = 3
};

enum class AngleDepthClearFixMode : int { Disabled = 0, Mode1 = 1 };

enum class HideMGEnvLevel : int { Disabled = 0, Level1 = 1 };

enum class FSR1_Quality_Preset : int {
    Disabled = 0,
    UltraQuality = 1,
    Quality = 2,
    Balanced = 3,
    Performance = 4
};

enum class MG_MultiDrawMode : int {
    LEGACY_MOBILEGLUES = 0,
    MG_VMDI_OPTIMIZED  = 1,
    MG_IMDBI_OPTIMIZED = 2
};

struct Version {
    int Major = 0;
    int Minor = 0;
    int Patch = 0;
    Version() = default;
    Version(int ma, int mi, int pa) : Major(ma), Minor(mi), Patch(pa) {}
    explicit Version(const std::string& s);
};

struct global_settings_t {
    // Layer 2 — básicos
    AngleMode              angle = AngleMode::Disabled;
    AngleConfig            angle_config = AngleConfig::DisableIfPossible;
    bool                   angle_supported = false;
    IgnoreErrorLevel       ignore_error = IgnoreErrorLevel::Partial;
    bool                   ext_compute_shader = false;
    std::size_t            max_glsl_cache_size = 32u * 1024u * 1024u;
    AngleDepthClearFixMode angle_depth_clear_fix_mode = AngleDepthClearFixMode::Disabled;
    bool                   ext_direct_state_access = false;
    Version                custom_gl_version {0, 0, 0};
    FSR1_Quality_Preset    fsr1_setting = FSR1_Quality_Preset::Disabled;
    float                  fsr1_sharpness = 0.75f;
    HideMGEnvLevel         hide_mg_env_level = HideMGEnvLevel::Disabled;

    // Multidraw
    bool             enable_vmdi  = false;
    bool             enable_imdbi = false;
    MG_MultiDrawMode multidraw_mode = MG_MultiDrawMode::LEGACY_MOBILEGLUES;

    // Extensões extras
    bool enable_ext_timer_query = false;
    bool enable_ext_gl43 = false;

    // Avançado
    int  buffer_upload_mode = 0;
    int  texture_swizzle_mode = 0;
    int  max_anisotropy_override = 0;
    bool force_gl_get_error_skip = true;
    bool force_depth_precision_fix = false;

    // Layer 3 — Debug
    bool        diag_enabled = false;
    bool        diag_frame_profiler = false;
    bool        diag_draw_call_count = false;
    bool        diag_shader_recompiles = false;
    bool        diag_backend_tier = false;
    bool        diag_cpu_gpu_load = false;
    bool        diag_log_backend_selection = false;
    bool        diag_log_shader_recompiles = false;
    bool        diag_log_draw_call_count = false;
    bool        diag_log_gl_trace = false;
    std::string diag_log_level = "info";
    bool        diag_capability_report = false;
    bool        diag_perfetto_enabled = false;
    int         diag_perfetto_max_duration = 30;
};

extern global_settings_t global_settings;

void init_settings();

#endif
