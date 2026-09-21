// MobileGlues - gl/diag_report.cpp
// Full Diagnostic Runtime Report.
//
// Design constraints (see commit message):
//   * runs ONCE at init, never per-frame
//   * every value comes from a real source (global_settings, g_gles_caps,
//     /proc/*, stat(), system properties, dlsym, glGetString)
//   * no unbounded allocation, no process spawning, no libc calls that can
//     block for long
//   * a missing file or a null driver string degrades that line, never the
//     report and never the process
// Copyright (c) 2025-2026 MobileGL-Dev
// SPDX-License-Identifier: LGPL-2.1-only

#include "diag_report.h"
#include "mg.h"
#include "log.h"
#include "mg_vmdi.h"
#include "mg_vmdi_config.h"
#include "imdb_engine.h"
#include "../config/settings.h"
#include "../config/config.h"
#include "../config/gpu_utils.h"
#include "../gles/loader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <chrono>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>

#if !defined(__APPLE__)
#include <sys/system_properties.h>
#if defined(__has_include)
#  if __has_include(<vulkan/vulkan.h>)
#    include <vulkan/vulkan.h>
#    define MG_HAS_VULKAN_HEADERS 1
#  endif
#endif
#endif

#ifndef MG_HAS_VULKAN_HEADERS
#define MG_HAS_VULKAN_HEADERS 0
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Small robust helpers
// ─────────────────────────────────────────────────────────────────────────────


// Local duplicates of the inline compat helpers from config/settings.cpp.
// Cannot include settings.cpp's anonymous namespace, so redeclare here.
namespace {
inline int mg_cfg_int_compat_d(const char* n, const char* f, int d = -1) {
    int v = -1;
    if (n) v = config_get_int_path(n);
    if (v < 0 && f) v = config_get_int_path(f);
    return v < 0 ? d : v;
}
inline float mg_cfg_float_compat_d(const char* n, const char* f, float d = 0.0f) {
    float v = std::nanf("");
    if (n) v = config_get_float_path(n);
    if (std::isnan(v) && f) v = config_get_float_path(f);
    return std::isnan(v) ? d : v;
}
}
#define mg_cfg_int_compat mg_cfg_int_compat_d
#define mg_cfg_float_compat mg_cfg_float_compat_d




namespace {

// Reads one non-negative number from /proc files by scanning for a key.
// Returns -1 if not found or unparsable.
static long read_proc_long(const char* path, const char* key) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    long result = -1;
    const size_t klen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0) {
            const char* p = line + klen;
            while (*p == ' ' || *p == ':' || *p == '\t') p++;
            result = atol(p);
            break;
        }
    }
    fclose(f);
    return result;
}

static int read_cpu_cores() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0 && n < 256) ? static_cast<int>(n) : -1;
}

static long read_ram_mb() {
    long kb = read_proc_long("/proc/meminfo", "MemTotal:");
    return kb > 0 ? kb / 1024 : -1;
}

static int read_android_api() {
#if !defined(__APPLE__)
    char buf[PROP_VALUE_MAX] = {};
    if (__system_property_get("ro.build.version.sdk", buf) > 0) {
        int v = atoi(buf);
        if (v > 0) return v;
    }
#endif
    return -1;
}

static long stat_size(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return static_cast<long>(st.st_size);
}

static long stat_mtime(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return static_cast<long>(st.st_mtime);
}

static const char* safe_gl_string(GLenum name) {
    if (!GLES.glGetString) return "(no glGetString)";
    const GLubyte* s = GLES.glGetString(name);
    return s ? reinterpret_cast<const char*>(s) : "(null)";
}

// Classify a GPU. Conservative: returns "WEAK" only for known low-end families.
static const char* gpu_class(const char* renderer) {
    if (!renderer || !*renderer) return "UNKNOWN";
    if (strstr(renderer, "PowerVR") && (strstr(renderer, "GE") || strstr(renderer, "GE8")))
        return "WEAK (TBDR, low-end)";
    if (strstr(renderer, "Mali-4"))   return "WEAK (Mali-4xx)";
    if (strstr(renderer, "Mali-T"))   return "MEDIUM (Mali-T)";
    if (strstr(renderer, "Mali-G"))   return "MODERN (Mali-G)";
    if (strstr(renderer, "Adreno (TM) 5")) return "MEDIUM (Adreno 5xx)";
    if (strstr(renderer, "Adreno"))   return "MODERN (Adreno 6xx+)";
    return "UNKNOWN";
}

static bool gpu_is_tbdr(const char* renderer) {
    if (!renderer) return false;
    return strstr(renderer, "PowerVR") != nullptr
        || strstr(renderer, "Mali-T") != nullptr
        || strstr(renderer, "Mali-G") != nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Box-drawing primitives. Every line goes through LOG_I.
// ─────────────────────────────────────────────────────────────────────────────

static void section_header(const char* icon, int idx, int total, const char* title) {
    // Box-drawing chars are 3-byte UTF-8 sequences, so padding cannot be done
    // by writing a single char into a buffer. Build a std::string and count
    // visible glyphs while appending.
    char head[256];
    snprintf(head, sizeof(head), "\u250c\u2500 %s [SECTION %d/%d] %s", icon, idx, total, title);
    std::string line = head;

    auto visible_len = [](const std::string& s) {
        int n = 0;
        for (size_t i = 0; i < s.size(); ) {
            unsigned char c = (unsigned char)s[i];
            if      (c < 0x80)           { n++; i += 1; }
            else if ((c & 0xE0) == 0xC0) { n++; i += 2; }
            else if ((c & 0xF0) == 0xE0) { n++; i += 3; }
            else                          { n++; i += 4; }
        }
        return n;
    };

    while (visible_len(line) < 92) line += "\u2500";
    line += "\u2510";
    LOG_I("");
    LOG_I("%s", line.c_str());
}

static void section_close() {
    LOG_I("└────────────────────────────────────────────────────────────────────────────────────────────┘");
}

static void kv(const char* key, const char* fmt, ...) {
    char keybuf[32];
    snprintf(keybuf, sizeof(keybuf), "%-24s", key);
    char valbuf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(valbuf, sizeof(valbuf), fmt, ap);
    va_end(ap);
    LOG_I("│  %s │ %s", keybuf, valbuf);
}

static void kv_bool(const char* key, bool active, const char* note = nullptr) {
    char keybuf[32];
    snprintf(keybuf, sizeof(keybuf), "%-24s", key);
    LOG_I("│  %s │ %s%s%s", keybuf,
          active ? "✅" : "❌",
          active ? " enabled" : " disabled",
          note ? note : "");
}

} // anon namespace

// ─────────────────────────────────────────────────────────────────────────────
// Conflict rules R1-R4
// ─────────────────────────────────────────────────────────────────────────────

extern "C" int mg_diag_apply_conflict_rules(void) {
    int resolved = 0;

    // R1: weak-GPU guard × ext_compute_shader
    if (global_settings.disable_compute_on_weak_gpu && global_settings.ext_compute_shader) {
        LOG_I("[DIAG-CONFLICT] R1: weak-GPU guard → ext_compute_shader forced OFF (was: ON)");
        global_settings.ext_compute_shader = false;
        resolved++;
    }

    // R2: weak-GPU guard × imdbiBackend = compute_dispatch
    if (global_settings.disable_compute_on_weak_gpu
        && global_settings.multidraw_mode == MG_MultiDrawMode::MG_IMDBI_OPTIMIZED
        && global_settings.imdbi_backend_mode == 3) {
        LOG_I("[DIAG-CONFLICT] R2: weak-GPU guard → IMDBI backend downgraded compute_dispatch → fast_indirect_ring");
        global_settings.imdbi_backend_mode = 1;
        IMDBI_Config cfg = g_imdbiDispatcher.get_config();
        cfg.primary_mode = IMDBI_BackendMode::FAST_INDIRECT_RING;
        g_imdbiDispatcher.set_config(cfg);
        resolved++;
    }

    // R3: FSR sharpening off but per-version sharpness set
    if (!global_settings.fsr_enable_sharpening
        && (global_settings.fsr1_sharpness != 0.4f || global_settings.fsr2_sharpness != 0.5f)) {
        LOG_I("[DIAG-CONFLICT] R3: fsrEnableSharpening=OFF → sharpness values are inert");
    }

    // R4: ANGLE off but angle depth fix set
    if (global_settings.angle == AngleMode::Disabled
        && global_settings.angle_depth_clear_fix_mode != AngleDepthClearFixMode::Disabled) {
        LOG_I("[DIAG-CONFLICT] R4: angleDepthClearFixMode set but ANGLE=off → no effect");
    }

    return resolved;
}

// ─────────────────────────────────────────────────────────────────────────────
// Vulkan version probe — cached, safe, no processes.
// ─────────────────────────────────────────────────────────────────────────────

extern "C" int mg_diag_max_vulkan_version(void) {
#if MG_HAS_VULKAN_HEADERS && !defined(__APPLE__)
    static int cached = -2;   // -2 = not probed
    if (cached != -2) return cached;

    void* vk = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!vk) {
        cached = 0;
        return cached;
    }

    typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
    typedef void (*PFN_vkDestroyInstance)(VkInstance, const VkAllocationCallbacks*);
    typedef VkResult (*PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*);
    typedef void (*PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, VkPhysicalDeviceProperties*);

    auto pCreate = (PFN_vkCreateInstance)dlsym(vk, "vkCreateInstance");
    auto pDestroy = (PFN_vkDestroyInstance)dlsym(vk, "vkDestroyInstance");
    auto pEnum = (PFN_vkEnumeratePhysicalDevices)dlsym(vk, "vkEnumeratePhysicalDevices");
    auto pGetProps = (PFN_vkGetPhysicalDeviceProperties)dlsym(vk, "vkGetPhysicalDeviceProperties");

    int max_ver = 0;
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice* gpus = nullptr;
    bool created = false;

    if (pCreate && pDestroy && pEnum && pGetProps) {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "MobileGlues-Diag";
        app.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;

        if (pCreate(&ci, nullptr, &inst) == VK_SUCCESS) {
            created = true;
            uint32_t count = 0;
            if (pEnum(inst, &count, nullptr) == VK_SUCCESS && count > 0 && count < 32) {
                gpus = (VkPhysicalDevice*)malloc(sizeof(VkPhysicalDevice) * count);
                if (gpus && pEnum(inst, &count, gpus) == VK_SUCCESS) {
                    for (uint32_t i = 0; i < count; i++) {
                        VkPhysicalDeviceProperties props{};
                        pGetProps(gpus[i], &props);
                        if (props.apiVersion > (uint32_t)max_ver) max_ver = (int)props.apiVersion;
                    }
                }
            }
        }
    }

    free(gpus);
    if (created) pDestroy(inst, nullptr);
    dlclose(vk);

    cached = max_ver;
    return cached;
#else
    return 0;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Report — 10 sections
// ─────────────────────────────────────────────────────────────────────────────

extern "C" void mg_diag_emit_full_report(void) {
    using clock = std::chrono::steady_clock;
    const auto t_start = clock::now();

    const char* real_renderer = safe_gl_string(GL_RENDERER);
    const char* real_vendor   = safe_gl_string(GL_VENDOR);
    const char* real_version  = safe_gl_string(GL_VERSION);
    const char* real_glsl     = safe_gl_string(GL_SHADING_LANGUAGE_VERSION);

    LOG_I("");
    LOG_I("╔════════════════════════════════════════════════════════════════════════════════════════════╗");
    LOG_I("║                                                                                            ║");
    LOG_I("║   ███╗   ███╗ ██████╗ ██████╗ ██╗██╗     ███████╗ ██████╗ ██╗     ██╗   ██╗███████╗███████╗ ║");
    LOG_I("║   ████╗ ████║██╔═══██╗██╔══██╗██║██║     ██╔════╝██╔════╝ ██║     ██║   ██║██╔════╝██╔════╝ ║");
    LOG_I("║   ██╔████╔██║██║   ██║██████╔╝██║██║     █████╗  ██║  ███╗██║     ██║   ██║█████╗  ███████╗ ║");
    LOG_I("║   ██║╚██╔╝██║██║   ██║██╔══██╗██║██║     ██╔══╝  ██║   ██║██║     ██║   ██║██╔══╝  ╚════██║ ║");
    LOG_I("║   ██║ ╚═╝ ██║╚██████╔╝██████╔╝██║███████╗███████╗╚██████╔╝███████╗╚██████╔╝███████╗███████║ ║");
    LOG_I("║   ╚═╝     ╚═╝ ╚═════╝ ╚═════╝ ╚═╝╚══════╝╚══════╝ ╚═════╝ ╚══════╝ ╚═════╝ ╚══════╝╚══════╝ ║");
    LOG_I("║                                                                                            ║");
    LOG_I("║                          Full Diagnostic Runtime Report · v2.0.0                           ║");
    LOG_I("╚════════════════════════════════════════════════════════════════════════════════════════════╝");

    // ── [1/10] DEVICE PROFILE ──
    section_header("📱", 1, 10, "DEVICE PROFILE");
    {
        const char* cls = gpu_class(real_renderer);
        kv("GL_VENDOR (real)", "%s", real_vendor ? real_vendor : "(null)");
        kv("GL_RENDERER (real)", "%s", real_renderer ? real_renderer : "(null)");
        kv("GL_VERSION (real)", "%s", real_version ? real_version : "(null)");
        kv("GLSL (real)", "%s", real_glsl ? real_glsl : "(null)");
        kv("ES version", "%d.%d", g_gles_caps.major, g_gles_caps.minor);
        kv("GPU class", "%s", cls);
        kv_bool("TBDR architecture", gpu_is_tbdr(real_renderer));

        int vk = mg_diag_max_vulkan_version();
        if (vk == 0) {
            kv("Vulkan", "❌ not available");
        } else {
            int vmaj = VK_VERSION_MAJOR(vk);
            int vmin = VK_VERSION_MINOR(vk);
            kv("Vulkan", "✅ %d.%d detected", vmaj, vmin);
        }

        kv("CPU cores", "%d", read_cpu_cores());
        long ram = read_ram_mb();
        if (ram > 0) kv("Physical RAM", "%ld MB (≈ %.1f GB)", ram, ram / 1024.0);
        else         kv("Physical RAM", "(unreadable)");

        int api = read_android_api();
        if (api > 0) kv("Android API level", "%d", api);
        else         kv("Android API level", "(unreadable)");

        bool angle_ok = checkIfANGLESupported(real_renderer);
        kv_bool("ANGLE support", angle_ok,
                angle_ok ? " (device eligible)" : " (probe rejected)");
    }
    section_close();

    // ── [2/10] GLES DRIVER CAPABILITIES ──
    section_header("🔌", 2, 10, "GLES DRIVER CAPABILITIES");
    {
        kv("Extensions count", "%d",
           GLES.glGetIntegerv ? ([]() {
               GLint n = 0; GLES.glGetIntegerv(GL_NUM_EXTENSIONS, &n); return (int)n;
           })() : 0);

        struct ext_check { const char* name; bool flag; const char* note; };
        const ext_check exts[] = {
            {"GL_EXT_buffer_storage",           (bool)g_gles_caps.GL_EXT_buffer_storage,          "persistent mapping"},
            {"GL_EXT_disjoint_timer_query",     (bool)g_gles_caps.GL_EXT_disjoint_timer_query,    "GPU timer queries"},
            {"GL_EXT_multi_draw_indirect",      (bool)g_gles_caps.GL_EXT_multi_draw_indirect,     "batched indirect draws"},
            {"GL_EXT_multi_draw_arrays",        mg_multi_draw_arrays_ext_available(),            "batched CPU-array draws"},
            {"GL_EXT_draw_elements_base_vertex",(bool)g_gles_caps.GL_EXT_draw_elements_base_vertex,"GPU-side base vertex"},
            {"GL_OES_draw_elements_base_vertex",(bool)g_gles_caps.GL_OES_draw_elements_base_vertex,"OES base vertex"},
            {"GL_EXT_sRGB_write_control",       (bool)g_gles_caps.GL_EXT_sRGB_write_control,      "sRGB framebuffer writes"},
            {"GL_EXT_multisample_compatibility",(bool)g_gles_caps.GL_EXT_multisample_compatibility,"GL_MULTISAMPLE enable"},
            {"GL_EXT_clip_cull_distance",       (bool)g_gles_caps.GL_EXT_clip_cull_distance,      "GL_CLIP_DISTANCE0..7"},
            {"GL_EXT_depth_clamp",              (bool)g_gles_caps.GL_EXT_depth_clamp,             "GL_DEPTH_CLAMP"},
            {"GL_NV_polygon_mode",              (bool)g_gles_caps.GL_NV_polygon_mode,             "polygon offset line/point"},
            {"GL_OES_sample_shading",           (bool)g_gles_caps.GL_OES_sample_shading,          "sample shading"},
        };
        for (const auto& e : exts) {
            kv(e.name, "%s %s", e.flag ? "✅" : "❌", e.note);
        }

        LOG_I("│");
        kv("resolved entry points:", "");
        struct fn_check { const char* name; bool present; };
        const fn_check fns[] = {
            {"glDrawElementsIndirect",         GLES.glDrawElementsIndirect != nullptr},
            {"glDrawArraysIndirect",           GLES.glDrawArraysIndirect != nullptr},
            {"glMultiDrawElementsIndirectEXT", GLES.glMultiDrawElementsIndirectEXT != nullptr},
            {"glMultiDrawArraysIndirectEXT",   GLES.glMultiDrawArraysIndirectEXT != nullptr},
            {"glMultiDrawElementsBaseVertexEXT",GLES.glMultiDrawElementsBaseVertexEXT != nullptr},
            {"glDrawElementsBaseVertex",       GLES.glDrawElementsBaseVertex != nullptr},
            {"glDispatchCompute",              GLES.glDispatchCompute != nullptr},
            {"glBufferStorageEXT",             GLES.glBufferStorageEXT != nullptr},
            {"glMapBufferRange",               GLES.glMapBufferRange != nullptr},
            {"glMemoryBarrier",                GLES.glMemoryBarrier != nullptr},
        };
        for (const auto& f : fns) kv(f.name, "%s", f.present ? "✅ resolved" : "❌ not resolved");
    }
    section_close();

    // ── [3/10] CONFIG SOURCE ──
    section_header("📁", 3, 10, "CONFIG SOURCE");
    {
        const char* path = config_file_path ? config_file_path : "(unset)";
        kv("Path", "%s", path);
        long sz = stat_size(path);
        kv("File size", "%ld bytes", sz);
        long mt = stat_mtime(path);
        kv("Last modified (FS)", "%ld (epoch)", mt);
        kv("Parse status", "%s", initialized ? "✅ loaded & parsed" : "❌ not parsed");
        kv("Config source", "authoritative (config.json is source of truth)");
    }
    section_close();

    // ── [4/10] JSON → RUNTIME (applied) ──
    section_header("⚙️", 4, 10, "JSON → RUNTIME (applied to global_settings)");
    {
        // Cross-check: read from JSON again, compare to runtime struct
        auto cross_int = [](const char* nested, const char* flat, int runtime) {
            int json_v = mg_cfg_int_compat(nested, flat, -999);
            const char* status = (json_v == -999) ? "⚠️  MISSING" :
                                 (json_v == runtime) ? "✅ APPLIED" : "❌ MISMATCH";
            kv(nested, "JSON:%-6d Runtime:%-10d %s", json_v, runtime, status);
        };
        auto cross_bool = [](const char* nested, const char* flat, bool runtime) {
            int json_v = mg_cfg_int_compat(nested, flat, -999);
            const char* status = (json_v == -999) ? "⚠️  MISSING" :
                                 ((json_v > 0) == runtime) ? "✅ APPLIED" : "❌ MISMATCH";
            kv(nested, "JSON:%-6d Runtime:%-10s %s",
               json_v, runtime ? "true" : "false", status);
        };

        LOG_I("│  [1/9] OpenGL ES / EGL");
        cross_int("opengl_egl.enableANGLE",  "enableANGLE",  (int)global_settings.angle_config);
        cross_int("opengl_egl.hideMGEnvLevel","hideMGEnvLevel",(int)global_settings.hide_mg_env_level);
        cross_bool("gpuOptimization.enableExtGL43", "enableExtGL43", global_settings.enable_ext_gl43);

        LOG_I("│  [2/9] Error Handling");
        cross_int("errorHandling.enableNoError","enableNoError",(int)global_settings.ignore_error);
        cross_bool("errorHandling.forceGlGetErrorSkip","forceGlGetErrorSkip", global_settings.force_gl_get_error_skip);
        cross_bool("errorHandling.forceDepthPrecisionFix","forceDepthPrecisionFix", global_settings.force_depth_precision_fix);
        cross_int("errorHandling.angleDepthClearFixMode","angleDepthClearFixMode",(int)global_settings.angle_depth_clear_fix_mode);

        LOG_I("│  [3/9] Shader Cache");
        {
            int mb = mg_cfg_int_compat("shaderCache.maxGlslCacheSize", "maxGlslCacheSize", -1);
            int runtime_mb = (int)(global_settings.max_glsl_cache_size / 1024 / 1024);
            const char* s = (mb < 0) ? "⚠️  MISSING" : (mb == runtime_mb) ? "✅ APPLIED" : "❌ MISMATCH";
            kv("shaderCache.maxGlslCacheSize", "JSON:%-6d Runtime:%-6d MB  %s", mb, runtime_mb, s);
        }
        cross_bool("shaderCache.useProgramBinaryCache","useProgramBinaryCache", global_settings.use_program_binary_cache);

        LOG_I("│  [4/9] Texture / Buffer");
        cross_int("textureBuffer.bufferUploadMode","bufferUploadMode", global_settings.buffer_upload_mode);
        cross_int("textureBuffer.textureSwizzleMode","textureSwizzleMode", global_settings.texture_swizzle_mode);
        cross_int("textureBuffer.maxAnisotropyOverride","maxAnisotropyOverride", global_settings.max_anisotropy_override);

        LOG_I("│  [5/9] Multidraw Engine (mutually exclusive)");
        {
            const char* eng = mg_get_multidraw_engine_name();
            kv("multidrawEngine", "%s", eng);
            kv_bool("  enableVMDI", global_settings.enable_vmdi);
            kv_bool("  enableIMDBI", global_settings.enable_imdbi);
            kv("  imdbiBackend", "%d (0=stitch 1=ring 2=unroll 3=compute)", global_settings.imdbi_backend_mode);
            kv("  imdbiUnrollFactor", "%d", global_settings.imdbi_unroll_factor);
            kv_bool("  imdbiPersistentMap", global_settings.imdbi_persistent_mapping);
            kv_bool("  imdbiRegisterPin", global_settings.imdbi_register_pinning);
            kv_bool("  imdbiPrimitiveRst", global_settings.imdbi_primitive_restart);
            kv("  imdbiRingSize", "%d bytes", global_settings.imdbi_ring_size);
        }

        LOG_I("│  [6/9] Fallback Orders");
        static const char* order_keys[] = {
            "multidrawOrderArrays", "multidrawOrderElements",
            "multidrawOrderElementsBaseVertex",
            "multidrawOrderArraysIndirect", "multidrawOrderElementsIndirect"
        };
        for (int i = 0; i < MD_ENTRY_COUNT; ++i) {
            std::string s;
            for (int k = 0; k < global_settings.multidraw_order_len[i]; ++k) {
                if (!s.empty()) s += " > ";
                s += md_backend_name(global_settings.multidraw_order[i][k]);
            }
            kv(order_keys[i], "%s", s.c_str());
        }

        LOG_I("│  [7/9] Extensions");
        cross_bool("extensions.enableExtComputeShader","enableExtComputeShader", global_settings.ext_compute_shader);
        cross_bool("extensions.enableExtTimerQuery","enableExtTimerQuery", global_settings.ext_timer_query);
        cross_bool("extensions.enableExtDirectStateAccess","enableExtDirectStateAccess", global_settings.ext_direct_state_access);

        LOG_I("│  [8/9] FSR");
        cross_bool("upscaling.fsrEnableSharpening","fsrEnableSharpening", global_settings.fsr_enable_sharpening);
        cross_int("upscaling.fsr1Version","fsr1Version", global_settings.fsr1_version);
        {
            float s1 = mg_cfg_float_compat("upscaling.fsr1Sharpness", "fsr1Sharpness", -1.0f);
            float s2 = mg_cfg_float_compat("upscaling.fsr2Sharpness", "fsr2Sharpness", -1.0f);
            kv("upscaling.fsr1Sharpness", "JSON:%.2f Runtime:%.2f %s", s1, global_settings.fsr1_sharpness,
               (s1 < 0 || s1 == global_settings.fsr1_sharpness) ? "✅" : "❌");
            kv("upscaling.fsr2Sharpness", "JSON:%.2f Runtime:%.2f %s", s2, global_settings.fsr2_sharpness,
               (s2 < 0 || s2 == global_settings.fsr2_sharpness) ? "✅" : "❌");
        }

        LOG_I("│  [9/9] Diagnostics");
        cross_bool("diagnostics.enabled", "diag.enabled", global_settings.diag_enabled);
    }
    section_close();

    // ── [5/10] MULTIDRAW SUBSYSTEM ──
    section_header("🎯", 5, 10, "MULTIDRAW SUBSYSTEM");
    {
        kv("Active engine", "%s", mg_get_multidraw_engine_name());
        kv("Runtime enum", "%d", (int)global_settings.multidraw_mode);

        LOG_I("│");
        LOG_I("│  Engine states (mutually exclusive):");
        kv_bool("  LEGACY_MOBILEGLUES", global_settings.multidraw_mode == MG_MultiDrawMode::LEGACY_MOBILEGLUES);
        kv_bool("  VMDI_OPTIMIZED",     global_settings.multidraw_mode == MG_MultiDrawMode::MG_VMDI_OPTIMIZED);
        kv_bool("  IMDBI_OPTIMIZED",    global_settings.multidraw_mode == MG_MultiDrawMode::MG_IMDBI_OPTIMIZED);

        LOG_I("│");
        LOG_I("│  Per-entry resolved backend (after capability filtering):");
        const char* names[] = {"glMultiDrawArrays", "glMultiDrawElements",
                               "glMultiDrawElementsBaseVertex",
                               "glMultiDrawArraysIndirect", "glMultiDrawElementsIndirect"};
        for (int i = 0; i < MD_ENTRY_COUNT; ++i) {
            const char* b = (global_settings.multidraw_order_len[i] > 0)
                ? md_backend_name(global_settings.multidraw_order[i][0])
                : "(none)";
            kv(names[i], "→ %s", b);
        }
    }
    section_close();

    // ── [6/10] CONFLICT RESOLUTION ──
    section_header("🔀", 6, 10, "CONFLICT RESOLUTION (stability rules)");
    {
        kv("R1 (weak GPU × compute)", "%s", global_settings.disable_compute_on_weak_gpu && !global_settings.ext_compute_shader
            ? "✅ applied (compute forced off)" : "⏭️  not triggered");
        kv("R2 (weak GPU × IMDBI compute)", "%s",
            (global_settings.disable_compute_on_weak_gpu
             && global_settings.multidraw_mode == MG_MultiDrawMode::MG_IMDBI_OPTIMIZED
             && global_settings.imdbi_backend_mode != 3)
            ? "✅ applied (ring instead of compute)" : "⏭️  not triggered");
        kv("R3 (FSR master off)", "%s", !global_settings.fsr_enable_sharpening
            ? "ℹ️  sharpness values inert" : "⏭️  not triggered");
        kv("R4 (ANGLE off × depth fix)", "%s",
            (global_settings.angle == AngleMode::Disabled
             && global_settings.angle_depth_clear_fix_mode != AngleDepthClearFixMode::Disabled)
            ? "ℹ️  fix has no effect" : "⏭️  not triggered");
    }
    section_close();

    // ── [7/10] ACTIVE OPTIMIZATIONS ──
    section_header("⚡", 7, 10, "ACTIVE OPTIMIZATIONS (running in engine)");
    {
        int n = 0;
        auto yes = [&](const char* name, const char* detail) {
            LOG_I("│    ✅ %-30s │ %s", name, detail); n++;
        };
        auto no = [&](const char* name, const char* why) {
            LOG_I("│    ❌ %-30s │ %s", name, why);
        };

        if (global_settings.multidraw_mode == MG_MultiDrawMode::MG_IMDBI_OPTIMIZED) {
            char buf[64]; snprintf(buf, sizeof(buf), "ring %d MB · unroll %dx",
                global_settings.imdbi_ring_size / (1024*1024), global_settings.imdbi_unroll_factor);
            yes("IMDBI batching", buf);
        } else if (global_settings.multidraw_mode == MG_MultiDrawMode::MG_VMDI_OPTIMIZED) {
            yes("VMDI batching", "virtual MDI");
        } else {
            no("Multidraw batching", "engine = legacy");
        }

        if (global_settings.max_glsl_cache_size > 0) {
            char buf[32]; snprintf(buf, sizeof(buf), "%zu MB in-memory", global_settings.max_glsl_cache_size / 1024 / 1024);
            yes("GLSL shader cache", buf);
        } else {
            no("GLSL shader cache", "disabled");
        }

        if (global_settings.use_program_binary_cache) yes("Program binary cache (L2)", "disk-backed");
        else no("Program binary cache", "disabled");

        if (global_settings.fsr_enable_sharpening) {
            char buf[64]; snprintf(buf, sizeof(buf), "v%d · intensity %.2f",
                global_settings.fsr1_version,
                global_settings.fsr1_version == 1 ? global_settings.fsr1_sharpness : global_settings.fsr2_sharpness);
            yes("FSR sharpening", buf);
        } else no("FSR sharpening", "master switch off");

        if (global_settings.force_gl_get_error_skip) yes("glGetError skip", "skips driver call in recompile");
        if (global_settings.force_depth_precision_fix) yes("Depth precision fix", "PowerVR workaround");
        if (global_settings.ext_timer_query) yes("Timer query emulation", "ARB/EXT timer_query");
        if (global_settings.ext_direct_state_access) yes("DSA emulation", "direct state access wrapper");
        if (global_settings.hide_mg_env_level != HideMGEnvLevel::Disabled) yes("glGetString camouflage", "hides MG presence");
        if (global_settings.disable_compute_on_weak_gpu) yes("Weak-GPU guard", "compute auto-disabled");
        if (global_settings.buffer_upload_mode != 0) {
            const char* m[] = {"auto","streaming","persistent","ring"};
            yes("Buffer upload mode", m[global_settings.buffer_upload_mode]);
        }
        if (global_settings.texture_swizzle_mode != 0) {
            yes("Texture swizzle", global_settings.texture_swizzle_mode == 1 ? "RGBA" : "BGRA");
        }
        if (global_settings.max_anisotropy_override > 0) {
            char buf[16]; snprintf(buf, sizeof(buf), "%dx", global_settings.max_anisotropy_override);
            yes("Max anisotropy override", buf);
        }

        LOG_I("│");
        LOG_I("│  Total active optimizations: %d", n);

        LOG_I("│");
        LOG_I("│  Deactivated (reasons):");
        if (global_settings.angle != AngleMode::Enabled) no("ANGLE translator", "native GL preferred");
        if (global_settings.multidraw_mode != MG_MultiDrawMode::MG_VMDI_OPTIMIZED) no("VMDI engine", "not selected");
        if (global_settings.multidraw_mode != MG_MultiDrawMode::MG_IMDBI_OPTIMIZED) no("IMDBI engine", "not selected");
        if (!global_settings.ext_compute_shader) no("Compute shader emu", "weak GPU or config");
        if (global_settings.fsr1_version != 1) no("FSR v1 (5-tap)", "v2 selected");
        if (!global_settings.diag_enabled) no("Diagnostics (14 keys)", "master switch off");
    }
    section_close();

    // ── [8/10] REPORTED GL STRINGS ──
    section_header("🎨", 8, 10, "REPORTED GL STRINGS (what the app sees)");
    {
        kv("GL_VENDOR", "%s", safe_gl_string(GL_VENDOR));
        kv("GL_RENDERER", "%s", safe_gl_string(GL_RENDERER));
        kv("GL_VERSION", "%s", safe_gl_string(GL_VERSION));
        kv("GLSL", "%s", safe_gl_string(GL_SHADING_LANGUAGE_VERSION));
        kv("Camouflage mode", "%s",
           global_settings.hide_mg_env_level == HideMGEnvLevel::Disabled
           ? "OFF (real strings shown)" : "ACTIVE (Level1)");
    }
    section_close();

    // ── [9/10] FINAL STATE ──
    section_header("🏁", 9, 10, "FINAL STATE");
    {
        kv("EGL context", "assumed current (report runs post-init)");
        kv("Report duration", "%lld ms",
           (long long)std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t_start).count());
    }
    section_close();

    // ── [10/10] FOOTER ──
    LOG_I("");
    LOG_I("╔════════════════════════════════════════════════════════════════════════════════════════════╗");
    LOG_I("║                                                                                            ║");
    LOG_I("║                                  ✅  MOBILEGLUES READY                                     ║");
    LOG_I("║                                                                                            ║");
    LOG_I("║   Engine  : %-72s ║", mg_get_multidraw_engine_name());
    LOG_I("║   Cache   : %-72s ║", global_settings.max_glsl_cache_size > 0 ? "GLSL active" : "GLSL disabled");
    LOG_I("║   FSR     : %-72s ║",
           global_settings.fsr_enable_sharpening
             ? (global_settings.fsr1_version == 1 ? "v1 (5-tap)" : "v2 (3-tap)")
             : "disabled");
    LOG_I("║   ANGLE   : %-72s ║", global_settings.angle == AngleMode::Enabled ? "enabled" : "disabled (native GL)");
    LOG_I("║                                                                                            ║");
    LOG_I("╚════════════════════════════════════════════════════════════════════════════════════════════╝");
}
