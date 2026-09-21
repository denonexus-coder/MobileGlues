// MobileGlues - config/settings.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "settings.h"
#include <cmath>
#include <strings.h>
#include "config.h"
#include "../gl/log.h"
#include "../gl/envvars.h"
#include "gpu_utils.h"
#include "../gl/getter.h"
#include "mg_vmdi_config.h"
#include "../gl/mg_vmdi.h"
#include "../gl/imdb_engine.h"
#include "../gl/glsl/program_binary_cache.h"

#define DEBUG 0

global_settings_t global_settings;

// --- Strict Config Keys (No Fallback) ---
namespace {
    inline int mg_cfg_int_compat(const char* nested_path, const char* flat_path, int fallback = -1) {
        int v = config_get_int_path(nested_path);
        return v < 0 ? fallback : v;
    }
    inline char* mg_cfg_str_compat(const char* nested_path, const char* flat_path) {
        return config_get_string_path(nested_path);
    }
    inline float mg_cfg_float_compat(const char* nested_path, const char* flat_path, float fallback = 0.0f) {
        float v = config_get_float_path(nested_path);
        return std::isnan(v) ? fallback : v;
    }
    inline int mg_cfg_bool_compat(const char* nested_path, const char* flat_path, int default_val) {
        int v = config_get_int_path(nested_path);
        if (v < 0) return default_val;
        return v > 0 ? 1 : 0;
    }
}

const char* md_backend_name(md_backend_t b) {
    for (const auto& n : k_md_backend_names) {
        if (n.backend == b) return n.name;
    }
    return "(unknown)";
}

const char* md_backend_suffix(md_backend_t b) {
    switch (b) {
    case B::Unroll:
        return "_drawelements"; // historical symbol name, kept so it stays resolvable
    case B::BaseVertex:
        return "_basevertex";
    case B::Indirect:
        return "_indirect";
    case B::MultiIndirect:
        return "_multiindirect";
    case B::MultiBaseVertex:
        return "_multibasevertex";
    case B::MultiArrays:
        return "_multiarrays";
    case B::Compute:
        return "_compute";
    default:
        return nullptr; // Auto never survives resolution
    }
}

// One item of a parsed order list: a concrete backend, or the pseudo item
// "native" (global order only).
struct md_order_item_t {
    bool is_native;
    B backend;
};

// Splits a comma/semicolon separated order list. Unknown names are dropped with
// a warning; "native" is accepted only when allow_native is set. Returns the
// number of items written.
static int md_parse_order_list(const char* key, const std::string& raw, bool allow_native, md_order_item_t* out,
                               int out_max) {
    int n = 0;
    std::string token;
    for (size_t i = 0; i <= raw.size(); ++i) {
        const char ch = i < raw.size() ? raw[i] : ',';
        if (ch != ',' && ch != ';') {
            token += ch;
            continue;
        }
        // Normalise the token the same way md_parse_backend does.
        std::string s;
        for (char c : token) {
            if (c == ' ' || c == '\t' || c == '_' || c == '-') continue;
            s += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        }
        token.clear();
        if (s.empty()) continue;
        if (n >= out_max) break;
        if (s == "native") {
            if (allow_native) {
                out[n++] = {true, B::Auto};
            } else {
                LOG_W_FORCE("%s: 'native' is only meaningful in the global multidrawOrder, ignored", key);
            }
            continue;
        }
        B b;
        if (!md_parse_backend(s, &b) || b == B::Auto) {
            LOG_W_FORCE("%s: '%s' is not a backend name, ignored", key, s.c_str());
            continue;
        }
        out[n++] = {false, b};
    }
    return n;
}

// The requested order per entry point, before capability filtering: parsed from
// config.json by parse_multidraw_orders() (called from init_settings(), when no
// GL is loaded yet), consumed by init_settings_post() once capabilities exist.
static B s_md_requested[MD_ENTRY_COUNT][MD_BACKEND_COUNT];
static int s_md_requested_len[MD_ENTRY_COUNT];

// Expands an order list into a total per-entry order: map "native" to this
// entry's native backend, keep the first occurrence of each backend that is a
// distinct implementation here, then append whatever the list missed by running
// the default global order through the same expansion. The result mentions
// every allowed backend exactly once, so ordering is total and the runtime
// chain always has a next rung.
static void md_expand_order(E e, const md_order_item_t* items, int item_count) {
    const md_entry_desc_t& d = k_md_entries[static_cast<int>(e)];
    B* out = s_md_requested[static_cast<int>(e)];
    int n = 0;
    unsigned seen = 0;

    auto push = [&](B b) {
        if ((d.allowed & md_bit(b)) == 0) return;
        if (seen & md_bit(b)) return;
        seen |= md_bit(b);
        out[n++] = b;
    };

    for (int i = 0; i < item_count; ++i) {
        push(items[i].is_native ? d.native_backend : items[i].backend);
    }
    // Pad with the default order so a hand-edited partial list still ranks every
    // backend. "native" sits first in the default, so the entry's native form
    // leads the padding as well.
    for (const char* name : k_md_default_global_order) {
        if (std::string(name) == "native") {
            push(d.native_backend);
        } else {
            B b;
            if (md_parse_backend(name, &b)) push(b);
        }
    }
    s_md_requested_len[static_cast<int>(e)] = n;
}

// Reads multidrawOrder / multidrawOrder<EntryPoint> into s_md_requested. Runs in
// init_settings(): config.json is loaded but GL is not, so no capability checks
// happen here.
static void parse_multidraw_orders() {
    md_order_item_t global_items[MD_BACKEND_COUNT + 1];
    int global_count = 0;

    const std::string raw_global = md_config_string("multidrawOrder");
    if (!raw_global.empty()) {
        global_count =
            md_parse_order_list("multidrawOrder", raw_global, true, global_items, MD_BACKEND_COUNT + 1);
    }
    if (global_count == 0) {
        for (const char* name : k_md_default_global_order) {
            md_order_item_t item{};
            if (std::string(name) == "native") {
                item.is_native = true;
            } else if (!md_parse_backend(name, &item.backend)) {
                continue;
            }
            global_items[global_count++] = item;
        }
    }

    for (int i = 0; i < MD_ENTRY_COUNT; ++i) {
        const E e = static_cast<E>(i);
        const md_entry_desc_t& d = k_md_entries[i];
        const std::string raw = md_config_string(d.order_key);
        if (!raw.empty()) {
            // Exception order: concrete backends only. Names that are not a
            // distinct implementation here are rejected in md_expand_order, with
            // d.why explaining the reason once below.
            md_order_item_t items[MD_BACKEND_COUNT];
            const int count = md_parse_order_list(d.order_key, raw, false, items, MD_BACKEND_COUNT);
            for (int k = 0; k < count; ++k) {
                if (!items[k].is_native && (d.allowed & md_bit(items[k].backend)) == 0) {
                    LOG_W_FORCE("%s: '%s' is not a distinct strategy for %s (%s), ignored", d.order_key,
                                md_backend_name(items[k].backend), d.label, d.why)
                }
            }
            md_expand_order(e, items, count);
        } else {
            md_expand_order(e, global_items, global_count);
        }
    }
}

void set_multidraw_setting() { // should be called after init_gles_target()
    // The pre-2.0 selection keys are no longer read: ordering replaced them.
    if (config_get_int(const_cast<char*>("multidrawMode")) != -1 ||
        config_get_string(const_cast<char*>("multidrawDisableBackends")) != nullptr) {
        LOG_W_FORCE("multidrawMode/multidrawDisableBackends are no longer used. The selection is an order "
                    "now: multidrawOrder (global, backend names best first, may contain \"native\") and "
                    "multidrawOrder<EntryPoint> for per-function exceptions.")
    }
    for (const auto& d : k_md_entries) {
        if (config_get_string(const_cast<char*>(d.legacy_mode_key)) != nullptr) {
            LOG_W_FORCE("%s is no longer used; see multidrawOrder / %s", d.legacy_mode_key, d.order_key);
            break;
        }
    }

    if (global_settings.enable_vmdi) {
        const char* exts = reinterpret_cast<const char*>(GLES.glGetString ? GLES.GLES.glGetString ? GLES.glGetString(GL_EXTENSIONS) : nullptr : nullptr);
        g_vmdiEngine.Init(exts);
    }
}

md_backend_t md_next_backend(md_entry_t e, md_backend_t cur) {
    const int idx = static_cast<int>(e);
    const int len = global_settings.multidraw_order_len[idx];
    const B* order = global_settings.multidraw_order[idx];
    if (len <= 0) return B::Unroll; // cannot happen after init_settings_post; be safe
    for (int i = 0; i < len; ++i) {
        if (order[i] == cur) {
            return order[i + 1 < len ? i + 1 : len - 1];
        }
    }
    // `cur` is not ranked here: a directly dlsym'ed symbol. Hand it the terminal
    // rung so the walk ends.
    return order[len - 1];
}

void init_settings_post() {
    const bool has_es31 = (g_gles_caps.major > 3) || (g_gles_caps.major == 3 && g_gles_caps.minor >= 1);
    const bool has_es32 = (g_gles_caps.major > 3) || (g_gles_caps.major == 3 && g_gles_caps.minor >= 2);
    const bool has_bv_ext =
        g_gles_caps.GL_EXT_draw_elements_base_vertex || g_gles_caps.GL_OES_draw_elements_base_vertex;

    // A capability counts only when the extension string *and* the resolved entry
    // point agree. The GLES loader uses a plain dlsym, so a driver can advertise
    // GL_EXT_multi_draw_indirect while the symbol is missing from the library that
    // was actually opened; trusting the string alone meant a null jump on the
    // first frame that issued a multi-draw.
    const bool multidraw = g_gles_caps.GL_EXT_multi_draw_indirect && GLES.glMultiDrawElementsIndirectEXT != nullptr;
    const bool basevertex = (has_bv_ext || has_es32) && GLES.glDrawElementsBaseVertex != nullptr;
    const bool indirect = has_es31 && GLES.glDrawElementsIndirect != nullptr;
    // EXT/OES_draw_elements_base_vertex also define the multi-draw form, whose
    // signature matches GL 3.2 core exactly -- but only when EXT_multi_draw_arrays
    // is supported as well. gl/multidraw.cpp checks that string, because neither
    // the resolved symbol nor a runtime probe can: a driver without the extension
    // accepts the call, draws nothing, and reports no error.
    const bool multibasevertex = mg_multi_draw_elements_basevertex_ext_available();

    // Compute mode used to be accepted without checking anything at all.
    bool compute = false;
    if (has_es31 && GLES.glDispatchCompute) {
        GLint ssbo_blocks = 0;
        GLES.glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &ssbo_blocks);
        // The multidraw compute shader declares exactly four shader storage
        // blocks, which is the GLES 3.1 guaranteed minimum.
        compute = ssbo_blocks >= 4;
        if (!compute) LOG_W_FORCE("Compute multidraw needs 4 SSBO blocks, driver reports %d", ssbo_blocks);
    }

    // ---- per-entry-point backend selection ----
    md_caps_t md_caps = {};
    md_caps.basevertex = basevertex;
    md_caps.indirect_elements = indirect;
    md_caps.indirect_arrays = has_es31 && GLES.glDrawArraysIndirect != nullptr;
    md_caps.multiindirect_elements = multidraw;
    md_caps.multiindirect_arrays =
        g_gles_caps.GL_EXT_multi_draw_indirect && GLES.glMultiDrawArraysIndirectEXT != nullptr;
    md_caps.multibasevertex = multibasevertex;
    md_caps.multiarrays = mg_multi_draw_arrays_ext_available();
    md_caps.compute = compute;

    // Filter each entry's requested order down to what this device can run. The
    // result is the runtime fallback chain; its first item is the resolved
    // backend. Unroll survives for the three list-taking entry points because it
    // is always available; for the two *Indirect entry points an empty result
    // means the context has no indirect draw at all, and their single exported
    // function already warns and draws nothing in that case -- keep the best
    // rung anyway so the order is never empty.
    for (int i = 0; i < MD_ENTRY_COUNT; ++i) {
        const md_entry_t e = static_cast<md_entry_t>(i);
        int n = 0;
        for (int k = 0; k < s_md_requested_len[i]; ++k) {
            const B cand = s_md_requested[i][k];
            if (!md_backend_available(e, cand, md_caps)) continue;
            global_settings.multidraw_order[i][n++] = cand;
        }
        if (n == 0) {
            LOG_W_FORCE("%s: no backend in the order is available on this device; keeping %s", k_md_entries[i].label,
                        md_backend_name(s_md_requested[i][0]))
            global_settings.multidraw_order[i][n++] = s_md_requested[i][0];
        }
        global_settings.multidraw_order_len[i] = n;
        global_settings.multidraw_backend[i] = global_settings.multidraw_order[i][0];

        std::string order_str;
        for (int k = 0; k < n; ++k) {
            if (!order_str.empty()) order_str += " > ";
            order_str += md_backend_name(global_settings.multidraw_order[i][k]);
        }
        LOG_V("[MobileGlues] %-34s = %s", k_md_entries[i].order_key, order_str.c_str());
    }
}

std::string dump_settings_string(std::string prefix) {
    std::stringstream ss;

    ss << prefix << "Angle: " << (global_settings.angle == AngleMode::Enabled ? "Enabled" : "Disabled") << "\n";
    ss << prefix << "IgnoreError: ";
    switch (global_settings.ignore_error) {
    case IgnoreErrorLevel::None:
        ss << "None";
        break;
    case IgnoreErrorLevel::Partial:
        ss << "Partial";
        break;
    case IgnoreErrorLevel::Full:
        ss << "Full";
        break;
    }
    ss << "\n";

    ss << prefix << "ExtComputeShader: " << (global_settings.ext_compute_shader ? "True" : "False") << "\n";
    ss << prefix << "ExtTimerQuery: " << (global_settings.ext_timer_query ? "True" : "False") << "\n";
    ss << prefix << "ExtDirectStateAccess: " << (global_settings.ext_direct_state_access ? "True" : "False") << "\n";
    ss << prefix << "MaxGlslCacheSize: " << (global_settings.max_glsl_cache_size / 1024 / 1024) << "MB\n";

    for (int i = 0; i < MD_ENTRY_COUNT; ++i) {
        ss << prefix << k_md_entries[i].order_key << ": ";
        for (int k = 0; k < global_settings.multidraw_order_len[i]; ++k) {
            if (k > 0) ss << " > ";
            ss << md_backend_name(global_settings.multidraw_order[i][k]);
        }
        ss << "\n";
    }

    ss << prefix << "AngleDepthClearFixMode: "
       << (global_settings.angle_depth_clear_fix_mode == AngleDepthClearFixMode::Disabled ? "Disabled" : "Enabled");
    ss << "\n";

    ss << prefix << "BufferCoherentAsFlush: " << (global_settings.buffer_coherent_as_flush ? "True" : "False") << "\n";

    ss << prefix << "CustomGLVersion: "
       << ((GLVersion.toInt(2) == DEFAULT_GL_VERSION) ? "(Default)" : std::to_string(GLVersion.toInt(2))) << "\n";

    ss << prefix << "Fsr1Setting: ";

    switch (global_settings.fsr1_setting) {
    case FSR1_Quality_Preset::Disabled:
        ss << "Disabled";
        break;
    case FSR1_Quality_Preset::UltraQuality:
        ss << "UltraQuality";
        break;
    case FSR1_Quality_Preset::Quality:
        ss << "Quality";
        break;
    case FSR1_Quality_Preset::Balanced:
        ss << "Balanced";
        break;
    case FSR1_Quality_Preset::Performance:
        ss << "Performance";
        break;
    default:
        ss << "Unknown";
        break;
    }
    ss << "\n";

    ss << prefix << "HideMGEnvLevel: "
       << ((global_settings.hide_mg_env_level == HideMGEnvLevel::Disabled)
               ? "Disabled"
               : std::to_string(static_cast<int>(global_settings.hide_mg_env_level)))
       << "\n";

    ss << prefix << "EnableVMDI: " << (global_settings.enable_vmdi ? "True" : "False") << "\n";
    ss << prefix << "EnableIMDBI: " << (global_settings.enable_imdbi ? "True" : "False") << "\n";
    ss << prefix << "MultiDrawEngine: " << mg_get_multidraw_engine_name() << "\n";

    return ss.str();
}

// ═══════════════════════════════════════════════════════════════════════
//  mg_log_all_settings_full — loga TODAS as configurações aplicadas.
//  Chamada no fim de mg_v3_apply_settings(), depois que todos os campos
//  já foram resolvidos e as exclusões mútuas já foram aplicadas.
//  Usa LOG_I para aparecer sempre (independente de DEBUG).
// ═══════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════
//  mg_log_all_settings_full — loga TODAS as configurações aplicadas (Strict v3 format).
// ═══════════════════════════════════════════════════════════════════════
static void mg_log_all_settings_full() {
    const auto& S = global_settings;
    const char* PFX = "[MobileGlues]";

    // Get GPU info strings safely
    const char* gpu_cstr = reinterpret_cast<const char*>(GLES.glGetString ? GLES.glGetString(GL_RENDERER) : nullptr);
    if (!gpu_cstr) gpu_cstr = "Unknown GPU";
    
    // Check if extensions available for log
    const char* exts = reinterpret_cast<const char*>(GLES.glGetString ? GLES.glGetString(GL_EXTENSIONS) : nullptr);
    bool hasAngle = (exts && strstr(exts, "GL_ANGLE_"));

    LOG_I("╔════════════════════════════════════════════════════════════════════════════════╗");
    LOG_I("║                 MOBILEGLUES v2.0.0 — INITIALIZATION LOG                       ║");
    LOG_I("║                 ════════════════════════════════════════════                   ║");
    LOG_I("║  License: GNU LGPL-2.1  |  Device: %-38s |  GL: %-8s   ║", gpu_cstr, "ES 3.2");
    LOG_I("╚════════════════════════════════════════════════════════════════════════════════╝");
    LOG_I("%s ┌──────────────────────────────────────────────────────────────────┐", PFX);
    LOG_I("%s │ [SYSTEM]  Initializing MobileGlues configuration...              │", PFX);
    LOG_I("%s └──────────────────────────────────────────────────────────────────┘", PFX);
    LOG_I("%s ", PFX);

    LOG_I("%s 📦 CONFIG SOURCE: %s", PFX, "/storage/emulated/0/MG/config.json");
    LOG_I("%s    Schema version: 3.0", PFX);
    LOG_I("%s ", PFX);

    // LAYER 1
    LOG_I("%s ╔════════════════════════════════════════════════════════════════╗", PFX);
    LOG_I("%s ║  [LAYER 1] OPENGL ES / EGL SETUP                              ║", PFX);
    LOG_I("%s ╚════════════════════════════════════════════════════════════════╝", PFX);
    
    LOG_I("%s  📌 enableANGLE (config)", PFX);
    LOG_I("%s     Value: %d", PFX, static_cast<int>(S.angle_config));
    LOG_I("%s     Probe result: Device %s ANGLE", PFX, hasAngle ? "SUPPORTS" : "DOES NOT SUPPORT");
    LOG_I("%s     Status: %s", PFX, S.angle == AngleMode::Enabled ? "ENABLED ✓" : "DISABLED");
    LOG_I("%s     → Using %s", PFX, S.angle == AngleMode::Enabled ? "ANGLE" : "native OpenGL ES");
    LOG_I("%s ", PFX);

    LOG_I("%s  📌 customGLVersion (override)", PFX);
    LOG_I("%s     Value: \"%s\"", PFX, S.custom_gl_version.isEmpty() ? "0" : S.custom_gl_version.toString().c_str());
    LOG_I("%s     Status: %s", PFX, S.custom_gl_version.isEmpty() ? "AUTO" : "ACTIVE ✓");
    LOG_I("%s ", PFX);

    LOG_I("%s  📌 hideMGEnvLevel", PFX);
    LOG_I("%s     Value: %d", PFX, static_cast<int>(S.hide_mg_env_level));
    LOG_I("%s     Status: %s", PFX, S.hide_mg_env_level > HideMGEnvLevel::Disabled ? "ACTIVE ✓" : "DISABLED");
    LOG_I("%s ", PFX);

    LOG_I("%s  📌 enableExtGL43 (GL 4.3 emulation)", PFX);
    LOG_I("%s     Value: %d", PFX, S.enable_ext_gl43 ? 1 : 0);
    LOG_I("%s     Status: %s", PFX, S.enable_ext_gl43 ? "ACTIVE ✓" : "DISABLED");
    LOG_I("%s ", PFX);

    // LAYER 2
    LOG_I("%s ╔════════════════════════════════════════════════════════════════╗", PFX);
    LOG_I("%s ║  [LAYER 2] ERROR HANDLING & PRECISION                         ║", PFX);
    LOG_I("%s ╚════════════════════════════════════════════════════════════════╝", PFX);
    
    LOG_I("%s  📌 enableNoError (glGetError behavior)", PFX);
    LOG_I("%s     Value: %d", PFX, static_cast<int>(S.ignore_error));
    LOG_I("%s     Status: %s", PFX, S.ignore_error != IgnoreErrorLevel::None ? "ACTIVE ✓" : "DISABLED");
    LOG_I("%s ", PFX);

    LOG_I("%s  📌 forceGlGetErrorSkip", PFX);
    LOG_I("%s     Value: %d", PFX, S.force_gl_get_error_skip ? 1 : 0);
    LOG_I("%s     Status: %s", PFX, S.force_gl_get_error_skip ? "ACTIVE ✓" : "DISABLED");
    LOG_I("%s ", PFX);

    LOG_I("%s  📌 forceDepthPrecisionFix", PFX);
    LOG_I("%s     Value: %d", PFX, S.force_depth_precision_fix ? 1 : 0);
    LOG_I("%s     Status: %s", PFX, S.force_depth_precision_fix ? "ACTIVE ✓" : "DISABLED");
    LOG_I("%s ", PFX);

    LOG_I("%s  📌 angleDepthClearFixMode", PFX);
    LOG_I("%s     Value: %d", PFX, static_cast<int>(S.angle_depth_clear_fix_mode));
    LOG_I("%s     Status: %s", PFX, S.angle_depth_clear_fix_mode > AngleDepthClearFixMode::Disabled ? "ACTIVE ✓" : "DISABLED");
    LOG_I("%s ", PFX);

    LOG_I("%s  📌 disableComputeOnWeakGpu", PFX);
    LOG_I("%s     Value: %d", PFX, S.disable_compute_on_weak_gpu ? 1 : 0);
    LOG_I("%s     Status: %s", PFX, S.disable_compute_on_weak_gpu ? "ACTIVE ✓" : "DISABLED");
    LOG_I("%s ", PFX);

    // LAYER 3
    LOG_I("%s ╔════════════════════════════════════════════════════════════════╗", PFX);
    LOG_I("%s ║  [LAYER 3] SHADER COMPILATION & CACHING                       ║", PFX);
    LOG_I("%s ╚════════════════════════════════════════════════════════════════╝", PFX);

    LOG_I("%s  📌 maxGlslCacheSize", PFX);
    LOG_I("%s     Value: %d MB", PFX, static_cast<int>(S.max_glsl_cache_size / 1024 / 1024));
    LOG_I("%s     Status: %s", PFX, S.max_glsl_cache_size > 0 ? "ACTIVE ✓" : "DISABLED");
    LOG_I("%s ", PFX);

    LOG_I("%s  📌 useProgramBinaryCache", PFX);
    LOG_I("%s     Value: %d", PFX, S.use_program_binary_cache ? 1 : 0);
    LOG_I("%s     Status: %s", PFX, S.use_program_binary_cache ? "ACTIVE ✓" : "DISABLED");
    LOG_I("%s ", PFX);

    // LAYER 4
    LOG_I("%s ╔════════════════════════════════════════════════════════════════╗", PFX);
    LOG_I("%s ║  [LAYER 4] TEXTURE & BUFFER UPLOAD                            ║", PFX);
    LOG_I("%s ╚════════════════════════════════════════════════════════════════╝", PFX);

    LOG_I("%s  📌 bufferUploadMode", PFX);
    LOG_I("%s     Value: %d", PFX, S.buffer_upload_mode);
    LOG_I("%s ", PFX);

    LOG_I("%s  📌 textureSwizzleMode", PFX);
    LOG_I("%s     Value: %d", PFX, S.texture_swizzle_mode);
    LOG_I("%s ", PFX);

    LOG_I("%s  📌 maxAnisotropyOverride", PFX);
    LOG_I("%s     Value: %d", PFX, S.max_anisotropy_override);
    LOG_I("%s ", PFX);

    // LAYER 5
    LOG_I("%s ╔════════════════════════════════════════════════════════════════╗", PFX);
    LOG_I("%s ║  [LAYER 5] MULTIDRAW ENGINE — ★ EXCLUSIVE SELECTION ★         ║", PFX);
    LOG_I("%s ║            Only ONE mode active. Others ignored.               ║", PFX);
    LOG_I("%s ╚════════════════════════════════════════════════════════════════╝", PFX);

    bool is_legacy = (S.multidraw_mode == MG_MultiDrawMode::LEGACY_MOBILEGLUES);
    bool is_vmdi   = (S.multidraw_mode == MG_MultiDrawMode::MG_VMDI_OPTIMIZED);
    bool is_imdbi  = (S.multidraw_mode == MG_MultiDrawMode::MG_IMDBI_OPTIMIZED);

    LOG_I("%s  🎯 ENGINE SELECTION: %s", PFX, mg_get_multidraw_engine_name());
    LOG_I("%s ", PFX);

    LOG_I("%s  ⚪ LEGACY_MOBILEGLUES (Original)", PFX);
    LOG_I("%s     Status: %s", PFX, is_legacy ? "[ATIVO ✓]" : "[INATIVO]");
    LOG_I("%s ", PFX);

    LOG_I("%s  ⚪ VMDI_OPTIMIZED (Virtual MultiDraw Indirect)", PFX);
    LOG_I("%s     Status: %s", PFX, is_vmdi ? "[ATIVO ✓]" : "[INATIVO]");
    LOG_I("%s ", PFX);

    LOG_I("%s  🟢 IMDBI_OPTIMIZED (Infinity MultiDraw Bi-Indirect)", PFX);
    LOG_I("%s     Status: %s", PFX, is_imdbi ? "[ATIVO ✓]" : "[INATIVO]");
    
    if (is_imdbi) {
        LOG_I("%s     ┌─── IMDBI SUBMODOS ─────────────────────────────────────┐", PFX);
        LOG_I("%s     │  📌 imdbiBackend                                        │", PFX);
        LOG_I("%s     │     Value: %d                                           │", PFX, S.imdbi_backend_mode);
        LOG_I("%s     │  📌 imdbiUnrollFactor                                   │", PFX);
        LOG_I("%s     │     Value: %d                                           │", PFX, S.imdbi_unroll_factor);
        LOG_I("%s     │  📌 imdbiPersistentMapping                              │", PFX);
        LOG_I("%s     │     Value: %d                                           │", PFX, S.imdbi_persistent_mapping ? 1 : 0);
        LOG_I("%s     │  📌 imdbiRegisterPinning                                │", PFX);
        LOG_I("%s     │     Value: %d                                           │", PFX, S.imdbi_register_pinning ? 1 : 0);
        LOG_I("%s     │  📌 imdbiPrimitiveRestart                               │", PFX);
        LOG_I("%s     │     Value: %d                                           │", PFX, S.imdbi_primitive_restart ? 1 : 0);
        LOG_I("%s     │  📌 imdbiRingSize                                       │", PFX);
        LOG_I("%s     │     Value: %d bytes                                     │", PFX, S.imdbi_ring_size);
        LOG_I("%s     └─────────────────────────────────────────────────────────┘", PFX);
    }
    LOG_I("%s ", PFX);
    
    if (is_vmdi && is_imdbi) {
        LOG_I("%s  ⚠️  CONFLICT CHECK:", PFX);
        LOG_I("%s     enableVMDI: 1", PFX);
        LOG_I("%s     enableIMDBI: 1", PFX);
        LOG_I("%s     → CONFLICT DETECTED (Using IMDBI)", PFX);
    }

    // LAYER 6
    LOG_I("%s ╔════════════════════════════════════════════════════════════════╗", PFX);
    LOG_I("%s ║  [LAYER 6] MULTIDRAW ORDER — Backend Fallback Priority        ║", PFX);
    LOG_I("%s ╚════════════════════════════════════════════════════════════════╝", PFX);
    
    for (int i = 0; i < MD_ENTRY_COUNT; ++i) {
        std::string order_str;
        for (int k = 0; k < S.multidraw_order_len[i]; ++k) {
            if (!order_str.empty()) order_str += ", ";
            order_str += md_backend_name(S.multidraw_order[i][k]);
        }
        LOG_I("%s  📌 %s", PFX, k_md_entries[i].order_key);
        LOG_I("%s     Config: \"%s\"", PFX, order_str.c_str());
        LOG_I("%s ", PFX);
    }

    // LAYER 7
    LOG_I("%s ╔════════════════════════════════════════════════════════════════╗", PFX);
    LOG_I("%s ║  [LAYER 7] EXTENSIONS SUPPORT                                 ║", PFX);
    LOG_I("%s ╚════════════════════════════════════════════════════════════════╝", PFX);

    LOG_I("%s  📌 enableExtComputeShader", PFX);
    LOG_I("%s     Value: %d", PFX, S.ext_compute_shader ? 1 : 0);
    LOG_I("%s     Status: %s", PFX, S.ext_compute_shader ? "[ATIVO ✓]" : "[DESATIVADO]");
    LOG_I("%s ", PFX);

    LOG_I("%s  📌 enableExtTimerQuery", PFX);
    LOG_I("%s     Value: %d", PFX, S.ext_timer_query ? 1 : 0);
    LOG_I("%s     Status: %s", PFX, S.ext_timer_query ? "[ATIVO ✓]" : "[DESATIVADO]");
    LOG_I("%s ", PFX);

    LOG_I("%s  📌 enableExtDirectStateAccess (DSA)", PFX);
    LOG_I("%s     Value: %d", PFX, S.ext_direct_state_access ? 1 : 0);
    LOG_I("%s     Status: %s", PFX, S.ext_direct_state_access ? "[ATIVO ✓]" : "[DESATIVADO]");
    LOG_I("%s ", PFX);

    // LAYER 8
    LOG_I("%s ╔════════════════════════════════════════════════════════════════╗", PFX);
    LOG_I("%s ║  [LAYER 8] UPSCALING / SUPER RESOLUTION                       ║", PFX);
    LOG_I("%s ╚════════════════════════════════════════════════════════════════╝", PFX);
    
    LOG_I("%s  📌 fsrEnableSharpening (Master switch)", PFX);
    LOG_I("%s     Value: %d", PFX, S.fsr_enable_sharpening ? 1 : 0);
    LOG_I("%s     Status: %s", PFX, S.fsr_enable_sharpening ? "[ATIVO ✓]" : "[DESATIVADO]");
    LOG_I("%s ", PFX);

    LOG_I("%s  📌 fsr1Version", PFX);
    LOG_I("%s     Value: %d", PFX, S.fsr1_version);
    LOG_I("%s ", PFX);

    LOG_I("%s  📌 fsr1Sharpness", PFX);
    LOG_I("%s     Value: %.2f", PFX, S.fsr1_sharpness);
    LOG_I("%s     Status: %s", PFX, (S.fsr_enable_sharpening && S.fsr1_version == 1) ? "[ATIVO ✓]" : "[INATIVO]");
    LOG_I("%s ", PFX);

    LOG_I("%s  📌 fsr2Sharpness", PFX);
    LOG_I("%s     Value: %.2f", PFX, S.fsr2_sharpness);
    LOG_I("%s     Status: %s", PFX, (S.fsr_enable_sharpening && S.fsr1_version == 2) ? "[ATIVO ✓]" : "[INATIVO]");
    LOG_I("%s ", PFX);

    // LAYER 9
    LOG_I("%s ╔════════════════════════════════════════════════════════════════╗", PFX);
    LOG_I("%s ║  [LAYER 9] DIAGNOSTICS & PROFILING                            ║", PFX);
    LOG_I("%s ╚════════════════════════════════════════════════════════════════╝", PFX);
    
    LOG_I("%s  📌 Diagnostics (Master switch)", PFX);
    LOG_I("%s     Value: %d", PFX, S.diag_enabled ? 1 : 0);
    LOG_I("%s     Status: %s", PFX, S.diag_enabled ? "[ATIVO ✓]" : "[DESATIVADO]");
    LOG_I("%s ", PFX);
    
    LOG_I("%s ╔════════════════════════════════════════════════════════════════╗", PFX);
    LOG_I("%s ║  READY! MobileGlues is active and optimized for your device   ║", PFX);
    LOG_I("%s ║  Version: 2.0.0 | License: GNU LGPL-2.1                       ║", PFX);
    LOG_I("%s ╚════════════════════════════════════════════════════════════════╝", PFX);
    LOG_I("%s ", PFX);
}
// ═══════════════════════════════════════════════════════════════════════
//  Schema v3 — aplica campos adicionais lidos pelo plugin.
//  Chamada por main.cpp depois de init_settings_post().
//  Não substitui nada do init_settings original.
//
//  A API de config (config.h) é:
//      int   config_get_int(char* name);    // -1 se ausente
//      char* config_get_string(char* name); // nullptr se ausente
//  Não há variante com default — o helper abaixo emula isso.
// ═══════════════════════════════════════════════════════════════════════

void mg_v3_apply_settings() {
    // Extensões avançadas
    global_settings.enable_ext_gl43           = mg_cfg_bool_compat("gpuOptimization.enableExtGL43", "enableExtGL43", 0) > 0;
    global_settings.force_gl_get_error_skip   = mg_cfg_bool_compat("errorHandling.forceGlGetErrorSkip", "forceGlGetErrorSkip", 1) > 0;
    // disableComputeOnWeakGpu: when true (default), auto-disable compute shaders
    // on GPUs detected to be too slow for them (e.g. PowerVR GE8320).
    global_settings.disable_compute_on_weak_gpu = mg_cfg_bool_compat("gpuOptimization.disableComputeOnWeakGpu", "disableComputeOnWeakGpu", 1) > 0;
    global_settings.buffer_upload_mode        = mg_cfg_int_compat("textureBuffer.bufferUploadMode", "bufferUploadMode", 0);
    global_settings.texture_swizzle_mode      = mg_cfg_int_compat("textureBuffer.textureSwizzleMode", "textureSwizzleMode", 0);
    global_settings.max_anisotropy_override   = mg_cfg_int_compat("textureBuffer.maxAnisotropyOverride", "maxAnisotropyOverride", 0);
    global_settings.force_depth_precision_fix = mg_cfg_bool_compat("errorHandling.forceDepthPrecisionFix", "forceDepthPrecisionFix", 0) > 0;

    // ─── FSR version & sharpness (v3.1) ─────────────────────────────
    //
    // fsr1Version: 1 = 5-tap (FSR1), 2 = 3-tap (FSR2)
    // Default 2 (3-tap; casa com performance de blit hardware)
    {
        int ver = mg_cfg_int_compat("upscaling.fsr1Version", "fsr1Version", 2);
        if (ver < 1) ver = 1;
        if (ver > 2) ver = 2;
        global_settings.fsr1_version = ver;
    }

    // fsr1Sharpness: aplica quando fsr1Version=1 (5-tap)
    {
        float sharp = mg_cfg_float_compat("upscaling.fsr1Sharpness", "fsr1Sharpness");
        if (!std::isnan(sharp)) {
            if (sharp < 0.0f) sharp = 0.0f;
            if (sharp > 1.0f) sharp = 1.0f;
            global_settings.fsr1_sharpness = sharp;
        }
    }

    // fsr2Sharpness: aplica quando fsr1Version=2 (3-tap)
    {
        float sharp = mg_cfg_float_compat("upscaling.fsr2Sharpness", "fsr2Sharpness");
        if (!std::isnan(sharp)) {
            if (sharp < 0.0f) sharp = 0.0f;
            if (sharp > 1.0f) sharp = 1.0f;
            global_settings.fsr2_sharpness = sharp;
        }
    }

    // fsrEnableSharpening: master switch (default true)
    {
        int enable = mg_cfg_bool_compat("upscaling.fsrEnableSharpening", "fsrEnableSharpening", 1);
        global_settings.fsr_enable_sharpening = enable > 0;
    }

    LOG_I("[MobileGlues] FSR: version=%d sharp1=%.2f sharp2=%.2f enable=%d",
          global_settings.fsr1_version,
          global_settings.fsr1_sharpness,
          global_settings.fsr2_sharpness,
          (int)global_settings.fsr_enable_sharpening);

    int bin_cache_cfg = mg_cfg_int_compat("shaderCache.useProgramBinaryCache", "useProgramBinaryCache");
    if (bin_cache_cfg < 0) {
        bin_cache_cfg = mg_cfg_int_compat("shaderCache.use_program_binary_cache", "use_program_binary_cache");
    }
    global_settings.use_program_binary_cache = bin_cache_cfg > 0;

    extern char* mg_directory_path;
    if (mg_directory_path) {
        MG::ProgramBinaryCache::get_instance().set_cache_dir(std::string(mg_directory_path) + "/program_bin");
    }
    MG::ProgramBinaryCache::get_instance().set_enabled(global_settings.use_program_binary_cache);

    // Layer 3 — Debug
    global_settings.diag_enabled               = mg_cfg_bool_compat("diagnostics.enabled", "diag.enabled", 0);
    global_settings.diag_frame_profiler        = mg_cfg_bool_compat("diagnostics.overlay.frameProfiler", "diag.overlay.frameProfiler", 0);
    global_settings.diag_draw_call_count       = mg_cfg_bool_compat("diagnostics.overlay.drawCallCount", "diag.overlay.drawCallCount", 0);
    global_settings.diag_shader_recompiles     = mg_cfg_bool_compat("diagnostics.overlay.shaderRecompiles", "diag.overlay.shaderRecompiles", 0);
    global_settings.diag_backend_tier          = mg_cfg_bool_compat("diagnostics.overlay.backendTier", "diag.overlay.backendTier", 0);
    global_settings.diag_cpu_gpu_load          = mg_cfg_bool_compat("diagnostics.overlay.cpuGpuLoad", "diag.overlay.cpuGpuLoad", 0);
    global_settings.diag_log_backend_selection = mg_cfg_bool_compat("diagnostics.logging.backendSelection", "diag.logging.backendSelection", 0);
    global_settings.diag_log_shader_recompiles = mg_cfg_bool_compat("diagnostics.logging.shaderRecompiles", "diag.logging.shaderRecompiles", 0);
    global_settings.diag_log_draw_call_count   = mg_cfg_bool_compat("diagnostics.logging.drawCallCount", "diag.logging.drawCallCount", 0);
    global_settings.diag_log_gl_trace          = mg_cfg_bool_compat("diagnostics.logging.glTrace", "diag.logging.glTrace", 0);
    const char* log_lvl = mg_cfg_str_compat("diagnostics.logging.level", "diag.logging.level");
    global_settings.diag_log_level             = (log_lvl && log_lvl[0]) ? log_lvl : "info";
    global_settings.diag_capability_report     = mg_cfg_bool_compat("diagnostics.capabilityReport", "diag.capabilityReport", 0);
    global_settings.diag_perfetto_enabled      = mg_cfg_bool_compat("diagnostics.perfetto.enabled", "diag.perfetto.enabled", 0);
    int p_dur = mg_cfg_int_compat("diagnostics.perfetto.maxDurationSec", "diag.perfetto.maxDurationSec", -1);
    global_settings.diag_perfetto_max_duration = p_dur > 0 ? p_dur : 30;

    // ── IMDBI submodes (só aplicam se engine == IMDBI) ──
    if (global_settings.multidraw_mode == MG_MultiDrawMode::MG_IMDBI_OPTIMIZED || global_settings.enable_imdbi) {
        const char* imdbi_mode_str = mg_cfg_str_compat("multidrawEngine.imdbi.imdbiBackend", "imdbiBackend");
        int mode = 1;
        if (imdbi_mode_str && *imdbi_mode_str) {
            if (strcasecmp(imdbi_mode_str, "stitching") == 0) mode = 0;
            else if (strcasecmp(imdbi_mode_str, "fast_indirect_ring") == 0) mode = 1;
            else if (strcasecmp(imdbi_mode_str, "unrolled_loop") == 0) mode = 2;
            else if (strcasecmp(imdbi_mode_str, "compute_dispatch") == 0) mode = 3;
        }
        global_settings.imdbi_backend_mode       = mode;
        global_settings.imdbi_unroll_factor      = mg_cfg_int_compat("multidrawEngine.imdbi.imdbiUnrollFactor", "imdbiUnrollFactor") > 0
                                                    ? mg_cfg_int_compat("multidrawEngine.imdbi.imdbiUnrollFactor", "imdbiUnrollFactor") : 4;
        global_settings.imdbi_persistent_mapping = mg_cfg_bool_compat("multidrawEngine.imdbi.imdbiPersistentMapping", "imdbiPersistentMapping", 1);
        global_settings.imdbi_register_pinning   = mg_cfg_bool_compat("multidrawEngine.imdbi.imdbiRegisterPinning", "imdbiRegisterPinning", 1);
        global_settings.imdbi_primitive_restart  = mg_cfg_bool_compat("multidrawEngine.imdbi.imdbiPrimitiveRestart", "imdbiPrimitiveRestart", 1);
        int ring = mg_cfg_int_compat("multidrawEngine.imdbi.imdbiRingSize", "imdbiRingSize");
        global_settings.imdbi_ring_size          = ring > 0 ? ring : (4 * 1024 * 1024);

        // aplica na dispatcher
        IMDBI_Config cfg = g_imdbiDispatcher.get_config();
        cfg.primary_mode = static_cast<IMDBI_BackendMode>(mode);
        cfg.unroll_factor = static_cast<uint32_t>(global_settings.imdbi_unroll_factor);
        cfg.use_persistent_mapping = global_settings.imdbi_persistent_mapping;
        cfg.enable_register_pinning = global_settings.imdbi_register_pinning;
        cfg.enable_primitive_restart = global_settings.imdbi_primitive_restart;
        cfg.ring_buffer_size = static_cast<size_t>(global_settings.imdbi_ring_size);
        g_imdbiDispatcher.set_config(cfg);

        LOG_I("[MobileGlues] IMDBI submode: backend=%d unroll=%d persistent=%d",
              mode, global_settings.imdbi_unroll_factor,
              (int)global_settings.imdbi_persistent_mapping);
    }

    // ── VMDI submode tier (só se engine == VMDI) ──
    if (global_settings.multidraw_mode == MG_MultiDrawMode::MG_VMDI_OPTIMIZED || global_settings.enable_vmdi) {
        const char* tier_str = mg_cfg_str_compat("multidrawEngine.vmdi.vmdiBackendTier", "vmdiBackendTier");
        int tier = -1;
        if (tier_str && *tier_str) {
            if (strcasecmp(tier_str, "native_mdi") == 0) tier = 0;
            else if (strcasecmp(tier_str, "multi_base_vertex") == 0) tier = 1;
            else if (strcasecmp(tier_str, "indirect_unrolled") == 0) tier = 2;
            else if (strcasecmp(tier_str, "direct_fallback") == 0) tier = 3;
            // "auto" ou vazio → -1
        }
        global_settings.vmdi_backend_tier    = tier;
        global_settings.vmdi_enable_autotune = mg_cfg_bool_compat("multidrawEngine.vmdi.vmdiEnableAutotune", "vmdiEnableAutotune", 1);

        mg_vmdi_set_tier(global_settings.vmdi_enable_autotune ? -1 : tier);
        LOG_I("[MobileGlues] VMDI submode: tier=%d autotune=%d",
              tier, (int)global_settings.vmdi_enable_autotune);
    }

    // Loga o estado final de TODAS as configurações numa única passagem.
    // Deve ser a última instrução desta função, após tudo ter sido aplicado.
    mg_log_all_settings_full();
}
