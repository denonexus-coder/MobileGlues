// MobileGlues - config/settings.cpp
// Parser completo do config.json. Autocontido — leitor JSON embutido.
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "settings.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// ── Logging (usa android/log.h; fallback p/ nada) ──────────────────────
#if defined(__ANDROID__)
#include <android/log.h>
#define MG_LOG_TAG "MobileGlues"
#define LOG_V(...) __android_log_print(ANDROID_LOG_VERBOSE, MG_LOG_TAG, __VA_ARGS__)
#else
#define LOG_V(...) ((void)0)
#endif

global_settings_t global_settings;

// ═══════════════════════════════════════════════════════════════════════
//  Leitor JSON mínimo (subset — objetos, strings, números, bools)
//  Suficiente para o config.json do MobileGlues. Sem dependências.
// ═══════════════════════════════════════════════════════════════════════
namespace mgjson {

struct Value {
    enum Type { Null, Bool, Number, String } type = Null;
    bool        b = false;
    double      n = 0.0;
    std::string s;

    bool isBool()   const { return type == Bool; }
    bool isNumber() const { return type == Number; }
    bool isString() const { return type == String; }
};

struct Object {
    std::vector<std::pair<std::string, Value>>        scalars;
    std::vector<std::pair<std::string, Object>>       children;

    const Value*  getScalar(const std::string& k) const {
        for (auto& p : scalars) if (p.first == k) return &p.second;
        return nullptr;
    }
    const Object* getObject(const std::string& k) const {
        for (auto& p : children) if (p.first == k) return &p.second;
        return nullptr;
    }
};

class Parser {
public:
    Parser(const std::string& src) : s(src), i(0) {}

    bool parse(Object& out) { skip(); return parseObject(out); }

private:
    const std::string& s;
    std::size_t i;

    void skip() {
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
            // ignora // e /* */
            if (c == '/' && i + 1 < s.size() && s[i+1] == '/') {
                i += 2; while (i < s.size() && s[i] != '\n') ++i; continue;
            }
            if (c == '/' && i + 1 < s.size() && s[i+1] == '*') {
                i += 2; while (i + 1 < s.size() && !(s[i] == '*' && s[i+1] == '/')) ++i;
                if (i + 1 < s.size()) i += 2; continue;
            }
            break;
        }
    }

    bool parseObject(Object& out) {
        skip();
        if (i >= s.size() || s[i] != '{') return false;
        ++i;
        while (true) {
            skip();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            if (i >= s.size() || s[i] != '"') return false;
            std::string key = parseString();
            skip();
            if (i >= s.size() || s[i] != ':') return false;
            ++i;
            skip();
            if (i < s.size() && s[i] == '{') {
                Object child;
                if (!parseObject(child)) return false;
                out.children.emplace_back(key, std::move(child));
            } else {
                Value v;
                if (!parseValue(v)) return false;
                out.scalars.emplace_back(key, std::move(v));
            }
            skip();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            return false;
        }
    }

    std::string parseString() {
        std::string r;
        ++i; // abre "
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                ++i;
                switch (s[i]) {
                    case 'n': r += '\n'; break;
                    case 't': r += '\t'; break;
                    case 'r': r += '\r'; break;
                    case '"': r += '"';  break;
                    case '\\': r += '\\'; break;
                    default:  r += s[i]; break;
                }
            } else {
                r += s[i];
            }
            ++i;
        }
        if (i < s.size()) ++i;
        return r;
    }

    bool parseValue(Value& v) {
        if (i >= s.size()) return false;
        char c = s[i];
        if (c == '"') { v.type = Value::String; v.s = parseString(); return true; }
        if (c == 't' && s.compare(i, 4, "true") == 0)  { v.type = Value::Bool; v.b = true;  i += 4; return true; }
        if (c == 'f' && s.compare(i, 5, "false") == 0) { v.type = Value::Bool; v.b = false; i += 5; return true; }
        if (c == 'n' && s.compare(i, 4, "null") == 0)  { v.type = Value::Null; i += 4; return true; }
        // número
        std::size_t start = i;
        while (i < s.size() && (std::isdigit((unsigned char)s[i]) ||
               s[i] == '-' || s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E'))
            ++i;
        if (i == start) return false;
        v.type = Value::Number;
        v.n = std::atof(s.substr(start, i - start).c_str());
        return true;
    }
};

inline const Object* navigate(const Object& root, const std::string& dotted) {
    const Object* cur = &root;
    std::size_t start = 0;
    while (start <= dotted.size()) {
        std::size_t dot = dotted.find('.', start);
        std::string part = (dot == std::string::npos)
            ? dotted.substr(start)
            : dotted.substr(start, dot - start);
        const Object* next = cur->getObject(part);
        if (!next) return nullptr;
        cur = next;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return cur;
}

} // namespace mgjson

// ═══════════════════════════════════════════════════════════════════════
//  Estado global do parser
// ═══════════════════════════════════════════════════════════════════════
namespace {

mgjson::Object g_root;
bool           g_loaded  = false;
bool           g_success = false;

const char* kCandidates[] = {
    "/sdcard/MG/config.json",
    "/storage/emulated/0/MG/config.json",
    "/sdcard/Android/data/com.fcl.plugin.mobileglues/files/MG/config.json",
    "/data/local/tmp/MG/config.json",
    "./MG/config.json",
    "./config.json",
    "config.json",
    nullptr
};

void load_once() {
    if (g_loaded) return;
    g_loaded = true;
    for (int k = 0; kCandidates[k]; ++k) {
        std::ifstream f(kCandidates[k], std::ios::binary);
        if (!f.good()) continue;
        std::stringstream ss; ss << f.rdbuf();
        const std::string content = ss.str();
        if (content.empty()) continue;
        mgjson::Parser p(content);
        mgjson::Object obj;
        if (p.parse(obj)) {
            g_root    = std::move(obj);
            g_success = true;
            LOG_V("[MobileGlues] config carregado de %s", kCandidates[k]);
            return;
        }
    }
    LOG_V("[MobileGlues] config.json não encontrado — usando defaults.");
}

// ── Acesso por caminho pontuado ────────────────────────────────────────
int get_int(const char* path, int fallback) {
    load_once();
    if (!g_success) return fallback;
    // pode ser escalar direto ou aninhado
    const mgjson::Object* cur = &g_root;
    std::string dotted(path);
    std::size_t start = 0;
    while (true) {
        std::size_t dot = dotted.find('.', start);
        std::string part = (dot == std::string::npos)
            ? dotted.substr(start) : dotted.substr(start, dot - start);
        if (dot == std::string::npos) {
            const mgjson::Value* v = cur->getScalar(part);
            if (!v) return fallback;
            if (v->isNumber()) return (int)v->n;
            if (v->isBool())   return v->b ? 1 : 0;
            if (v->isString()) return std::atoi(v->s.c_str());
            return fallback;
        }
        const mgjson::Object* next = cur->getObject(part);
        if (!next) return fallback;
        cur = next;
        start = dot + 1;
    }
}

bool get_bool(const char* path, bool fallback) {
    load_once();
    if (!g_success) return fallback;
    const mgjson::Object* cur = &g_root;
    std::string dotted(path);
    std::size_t start = 0;
    while (true) {
        std::size_t dot = dotted.find('.', start);
        std::string part = (dot == std::string::npos)
            ? dotted.substr(start) : dotted.substr(start, dot - start);
        if (dot == std::string::npos) {
            const mgjson::Value* v = cur->getScalar(part);
            if (!v) return fallback;
            if (v->isBool())   return v->b;
            if (v->isNumber()) return v->n != 0.0;
            if (v->isString()) {
                std::string t = v->s;
                for (auto& c : t) c = (char)std::tolower((unsigned char)c);
                return t == "true" || t == "1" || t == "yes" || t == "on";
            }
            return fallback;
        }
        const mgjson::Object* next = cur->getObject(part);
        if (!next) return fallback;
        cur = next;
        start = dot + 1;
    }
}

float get_float(const char* path, float fallback) {
    load_once();
    if (!g_success) return fallback;
    const mgjson::Object* cur = &g_root;
    std::string dotted(path);
    std::size_t start = 0;
    while (true) {
        std::size_t dot = dotted.find('.', start);
        std::string part = (dot == std::string::npos)
            ? dotted.substr(start) : dotted.substr(start, dot - start);
        if (dot == std::string::npos) {
            const mgjson::Value* v = cur->getScalar(part);
            if (!v) return fallback;
            if (v->isNumber()) return (float)v->n;
            if (v->isString()) return (float)std::atof(v->s.c_str());
            return fallback;
        }
        const mgjson::Object* next = cur->getObject(part);
        if (!next) return fallback;
        cur = next;
        start = dot + 1;
    }
}

std::string get_string(const char* path, const char* fallback) {
    load_once();
    if (!g_success) return fallback ? std::string(fallback) : std::string();
    const mgjson::Object* cur = &g_root;
    std::string dotted(path);
    std::size_t start = 0;
    while (true) {
        std::size_t dot = dotted.find('.', start);
        std::string part = (dot == std::string::npos)
            ? dotted.substr(start) : dotted.substr(start, dot - start);
        if (dot == std::string::npos) {
            const mgjson::Value* v = cur->getScalar(part);
            if (!v || !v->isString()) return fallback ? std::string(fallback) : std::string();
            return v->s;
        }
        const mgjson::Object* next = cur->getObject(part);
        if (!next) return fallback ? std::string(fallback) : std::string();
        cur = next;
        start = dot + 1;
    }
}

MG_MultiDrawMode parse_engine(const std::string& s) {
    if (s == "imdbi" || s == "IMDBI" || s == "Imdbi" || s == "2")
        return MG_MultiDrawMode::MG_IMDBI_OPTIMIZED;
    if (s == "vmdi"  || s == "VMDI"  || s == "Vmdi"  || s == "1")
        return MG_MultiDrawMode::MG_VMDI_OPTIMIZED;
    return MG_MultiDrawMode::LEGACY_MOBILEGLUES;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
//  Version(std::string)
// ═══════════════════════════════════════════════════════════════════════
Version::Version(const std::string& s) {
    int ma = 0, mi = 0, pa = 0;
    std::sscanf(s.c_str(), "%d.%d.%d", &ma, &mi, &pa);
    Major = ma; Minor = mi; Patch = pa;
}

// ═══════════════════════════════════════════════════════════════════════
//  init_settings — chamado a cada boot da lib
// ═══════════════════════════════════════════════════════════════════════
void init_settings() {
    // Força recarregar do disco (plugin pode ter reescrito o JSON)
    g_loaded  = false;
    g_success = false;
    g_root    = mgjson::Object{};

    // ── 2.1 GL Error Policy ───────────────────────────────────────────
    switch (get_int("enableNoError", 0)) {
        case 1:  global_settings.ignore_error = IgnoreErrorLevel::None;    break;
        case 2:  global_settings.ignore_error = IgnoreErrorLevel::Partial; break;
        case 3:  global_settings.ignore_error = IgnoreErrorLevel::Full;    break;
        default: global_settings.ignore_error = IgnoreErrorLevel::Partial; break;
    }
    global_settings.force_gl_get_error_skip = get_bool("forceGlGetErrorSkip", true);

    // ── 2.2 ANGLE ─────────────────────────────────────────────────────
    const int angleWire = get_int("enableANGLE", 1);
    global_settings.angle_config = static_cast<AngleConfig>(angleWire);
    global_settings.angle = (angleWire == 1 || angleWire == 3)
        ? AngleMode::Enabled : AngleMode::Disabled;
    global_settings.angle_depth_clear_fix_mode =
        static_cast<AngleDepthClearFixMode>(get_int("angleDepthClearFixMode", 0));

    // ── 2.3 MultiDraw Engine ──────────────────────────────────────────
    std::string engine = get_string("multidrawEngine", "legacy");
    MG_MultiDrawMode md = parse_engine(engine);
    if (const char* env = std::getenv("MG_MULTIDRAW_ENGINE")) md = parse_engine(env);
    global_settings.multidraw_mode = md;
    global_settings.enable_vmdi  = (md == MG_MultiDrawMode::MG_VMDI_OPTIMIZED);
    global_settings.enable_imdbi = (md == MG_MultiDrawMode::MG_IMDBI_OPTIMIZED);

    // ── 2.4 FSR1 ──────────────────────────────────────────────────────
    global_settings.fsr1_setting   = static_cast<FSR1_Quality_Preset>(get_int("fsr1Setting", 0));
    global_settings.fsr1_sharpness = get_float("fsr1Sharpness", 0.75f);

    // ── 2.5 GL Version ────────────────────────────────────────────────
    const int glv = get_int("customGLVersion", 0);
    if (glv > 0) global_settings.custom_gl_version = Version(glv / 10, glv % 10, 0);
    else         global_settings.custom_gl_version = Version(0, 0, 0);

    // ── 2.6 Extensions ────────────────────────────────────────────────
    global_settings.ext_compute_shader      = get_bool("enableExtComputeShader", false);
    global_settings.enable_ext_timer_query  = get_bool("enableExtTimerQuery", false);
    global_settings.ext_direct_state_access = get_bool("enableExtDirectStateAccess", false);
    global_settings.enable_ext_gl43         = get_bool("enableExtGL43", false);

    // ── 2.7 Shader Pipeline ───────────────────────────────────────────
    const int cacheMb = get_int("maxGlslCacheSize", 32);
    global_settings.max_glsl_cache_size = (cacheMb > 0)
        ? static_cast<std::size_t>(cacheMb) * 1024u * 1024u : 0u;
    global_settings.force_depth_precision_fix = get_bool("forceDepthPrecisionFix", false);

    // ── 2.8 Buffer / Texture ──────────────────────────────────────────
    global_settings.buffer_upload_mode      = get_int("bufferUploadMode", 0);
    global_settings.texture_swizzle_mode    = get_int("textureSwizzleMode", 0);
    global_settings.max_anisotropy_override = get_int("maxAnisotropyOverride", 0);

    // ── 2.9 Hide MG Env ───────────────────────────────────────────────
    global_settings.hide_mg_env_level = static_cast<HideMGEnvLevel>(get_int("hideMGEnvLevel", 0));

    // ── Layer 3 — Debug ───────────────────────────────────────────────
    global_settings.diag_enabled               = get_bool("diag.enabled", false);
    global_settings.diag_frame_profiler        = get_bool("diag.overlay.frameProfiler", false);
    global_settings.diag_draw_call_count       = get_bool("diag.overlay.drawCallCount", false);
    global_settings.diag_shader_recompiles     = get_bool("diag.overlay.shaderRecompiles", false);
    global_settings.diag_backend_tier          = get_bool("diag.overlay.backendTier", false);
    global_settings.diag_cpu_gpu_load          = get_bool("diag.overlay.cpuGpuLoad", false);
    global_settings.diag_log_backend_selection = get_bool("diag.logging.backendSelection", false);
    global_settings.diag_log_shader_recompiles = get_bool("diag.logging.shaderRecompiles", false);
    global_settings.diag_log_draw_call_count   = get_bool("diag.logging.drawCallCount", false);
    global_settings.diag_log_gl_trace          = get_bool("diag.logging.glTrace", false);
    global_settings.diag_log_level             = get_string("diag.logging.level", "info");
    global_settings.diag_capability_report     = get_bool("diag.capabilityReport", false);
    global_settings.diag_perfetto_enabled      = get_bool("diag.perfetto.enabled", false);
    global_settings.diag_perfetto_max_duration = get_int("diag.perfetto.maxDurationSec", 30);

    LOG_V("[MobileGlues] enableANGLE=%d enableNoError=%d multidraw=%d fsr1=%d",
          (int)global_settings.angle, (int)global_settings.ignore_error,
          (int)global_settings.multidraw_mode, (int)global_settings.fsr1_setting);
}
