// MobileGlues - gl/frame_profiler.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// SPDX-License-Identifier: LGPL-2.1-only

#include "frame_profiler.h"
#include "log.h"
#include "mg.h"
#include "../config/config.h"
#include "../config/settings.h"
#include "../config/cJSON.h"

#include <atomic>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cfloat>
#include <ctime>
#include <sys/stat.h>
#include <algorithm>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
namespace {

constexpr int kRecentRingSize = 240;      // 4s at 60fps for current-window avg
constexpr int kHistBins       = 32;       // 0..31 ms, plus overflow (31 = 31ms+)
constexpr uint64_t kFlushMs   = 10000;    // periodic session flush every 10s

struct FrameRing {
    double frame_ms[kRecentRingSize];
    double cpu_ms[kRecentRingSize];
    int    total_count;
};

struct SessionRunning {
    uint64_t frame_count;
    uint64_t dropped_count;
    double   frame_ms_sum;
    double   cpu_ms_sum;
    double   frame_ms_min;   // session absolute min
    double   frame_ms_max;   // session absolute max
    uint64_t hist[kHistBins];
};

struct PrevStats {
    int   valid;
    float fps_min;
    float fps_avg;
    float fps_max;
    float frame_ms_min;
    float frame_ms_max;
    uint64_t duration_ms;
};

struct TLS {
    uint64_t last_frame_ns;
    uint64_t cpu_begin_ns;
    uint64_t cpu_accum_ns;
    int      cpu_depth;
};

// ── Global state ────────────────────────────────────────────────────────────
FrameRing         g_ring;
std::mutex        g_ring_mutex;

SessionRunning    g_session;
std::mutex        g_session_mutex;

std::atomic<bool> g_profiler_initialized{false};
std::atomic<bool> g_session_active{false};
std::atomic<float> g_fps_cur{0.0f};
std::atomic<float> g_frame_ms_cur{0.0f};

std::mutex        g_lifecycle_mutex;
uint64_t          g_session_id       = 0;
uint64_t          g_session_start_ms = 0;
uint64_t          g_last_flush_ms    = 0;

PrevStats         g_prev = {};

thread_local TLS  g_tls = {};

// ── Time helpers ────────────────────────────────────────────────────────────
uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t epoch_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

void iso8601(uint64_t epoch_sec, char* out, size_t n) {
    time_t t = (time_t)epoch_sec;
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

// ── Recording ───────────────────────────────────────────────────────────────
void record_frame(double frame_ms, double cpu_ms) {
    {
        std::lock_guard<std::mutex> lk(g_ring_mutex);
        int idx = g_ring.total_count % kRecentRingSize;
        g_ring.frame_ms[idx] = frame_ms;
        g_ring.cpu_ms[idx]   = cpu_ms;
        g_ring.total_count++;
    }
    {
        std::lock_guard<std::mutex> lk(g_session_mutex);
        g_session.frame_count++;
        g_session.frame_ms_sum += frame_ms;
        g_session.cpu_ms_sum   += cpu_ms;
        if (frame_ms < g_session.frame_ms_min) g_session.frame_ms_min = frame_ms;
        if (frame_ms > g_session.frame_ms_max) g_session.frame_ms_max = frame_ms;
        if (frame_ms > 33.34) g_session.dropped_count++;
        int bin = (int)frame_ms;
        if (bin < 0) bin = 0;
        if (bin >= kHistBins) bin = kHistBins - 1;
        g_session.hist[bin]++;
    }
    g_frame_ms_cur.store((float)frame_ms, std::memory_order_relaxed);
    g_fps_cur.store(frame_ms > 0.01 ? (float)(1000.0 / frame_ms) : 0.0f,
                    std::memory_order_relaxed);
}

// ── Snapshot ────────────────────────────────────────────────────────────────
void compute_snapshot(MG_FrameStats* out) {
    memset(out, 0, sizeof(*out));
    if (!g_profiler_initialized.load()) return;

    double fm_buf[kRecentRingSize];
    double cm_buf[kRecentRingSize];
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(g_ring_mutex);
        n = g_ring.total_count > kRecentRingSize ? kRecentRingSize : g_ring.total_count;
        for (int i = 0; i < n; i++) {
            fm_buf[i] = g_ring.frame_ms[i];
            cm_buf[i] = g_ring.cpu_ms[i];
        }
    }

    SessionRunning sess;
    memset(&sess, 0, sizeof(sess));
    sess.frame_ms_min = DBL_MAX;
    {
        std::lock_guard<std::mutex> lk(g_session_mutex);
        sess = g_session;
    }

    // Recent-window p95 (used only for the "current" summary line)
    (void)fm_buf; (void)cm_buf; (void)n;

    double s_avg = sess.frame_count > 0 ? sess.frame_ms_sum / sess.frame_count : 0.0;
    double s_min = sess.frame_count > 0 ? sess.frame_ms_min : 0.0;
    double s_max = sess.frame_count > 0 ? sess.frame_ms_max : 0.0;

    // p95 from histogram (upper edge of the bin at which 95% is reached)
    double s_p95 = 0.0;
    if (sess.frame_count > 0) {
        uint64_t target = (uint64_t)(sess.frame_count * 0.95);
        uint64_t cum = 0;
        for (int i = 0; i < kHistBins; i++) {
            cum += sess.hist[i];
            if (cum >= target) { s_p95 = i + 0.5; break; }
        }
        if (s_p95 <= 0.0) s_p95 = (double)kHistBins - 0.5;
    }

    out->fps_current     = g_fps_cur.load();
    out->fps_avg         = (float)(s_avg > 0.01 ? 1000.0 / s_avg : 0.0);
    out->fps_min         = (float)(s_max > 0.01 ? 1000.0 / s_max : 0.0);
    out->fps_max         = (float)(s_min > 0.01 ? 1000.0 / s_min : 0.0);
    out->fps_p95         = (float)(s_p95 > 0.01 ? 1000.0 / s_p95 : 0.0);

    out->frame_ms_current = g_frame_ms_cur.load();
    out->frame_ms_avg    = (float)s_avg;
    out->frame_ms_min    = (float)s_min;
    out->frame_ms_max    = (float)s_max;
    out->frame_ms_p95    = (float)s_p95;

    double cpu_pct = (s_avg > 0.01 && sess.frame_count > 0)
                     ? (sess.cpu_ms_sum / sess.frame_count) / s_avg * 100.0
                     : 0.0;
    if (cpu_pct < 0.0)   cpu_pct = 0.0;
    if (cpu_pct > 100.0) cpu_pct = 100.0;
    out->cpu_load_pct = (float)cpu_pct;
    out->gpu_load_pct = (float)(100.0 - cpu_pct);
    out->has_gpu_timer = 0;

    out->frame_count    = sess.frame_count;
    out->dropped_frames = sess.dropped_count;
}

// ── Session persistence ─────────────────────────────────────────────────────
std::string sessions_dir() {
    if (!mg_directory_path) return std::string();
    return std::string(mg_directory_path) + "/sessions";
}

void ensure_sessions_dir() {
    std::string d = sessions_dir();
    if (d.empty()) return;
    mkdir(d.c_str(), 0755);
}

void read_prev_session() {
    memset(&g_prev, 0, sizeof(g_prev));
    std::string path = sessions_dir();
    if (path.empty()) return;
    path += "/latest.json";

    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 65536) { fclose(f); return; }

    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';

    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON* stats = cJSON_GetObjectItem(root, "stats");
    if (stats) {
        cJSON* fm = cJSON_GetObjectItem(stats, "fps_min");   if (fm && cJSON_IsNumber(fm)) g_prev.fps_min = (float)fm->valuedouble;
        cJSON* fa = cJSON_GetObjectItem(stats, "fps_avg");   if (fa && cJSON_IsNumber(fa)) g_prev.fps_avg = (float)fa->valuedouble;
        cJSON* fx = cJSON_GetObjectItem(stats, "fps_max");   if (fx && cJSON_IsNumber(fx)) g_prev.fps_max = (float)fx->valuedouble;
        cJSON* mn = cJSON_GetObjectItem(stats, "frame_ms_min"); if (mn && cJSON_IsNumber(mn)) g_prev.frame_ms_min = (float)mn->valuedouble;
        cJSON* mx = cJSON_GetObjectItem(stats, "frame_ms_max"); if (mx && cJSON_IsNumber(mx)) g_prev.frame_ms_max = (float)mx->valuedouble;
        g_prev.valid = 1;
    }
    cJSON* dur = cJSON_GetObjectItem(root, "duration_ms");
    if (dur && cJSON_IsNumber(dur)) g_prev.duration_ms = (uint64_t)dur->valuedouble;

    cJSON_Delete(root);
}

// Snapshot the fields of global_settings that affect performance.
void build_config_snapshot(cJSON* cfg) {
    auto addint = [&](const char* k, int v) { cJSON_AddNumberToObject(cfg, k, v); };
    auto addstr = [&](const char* k, const char* v) { cJSON_AddStringToObject(cfg, k, v ? v : ""); };

    addstr("multidrawEngine", mg_get_multidraw_engine_name());
    addint("enable_vmdi", global_settings.enable_vmdi ? 1 : 0);
    addint("enable_imdbi", global_settings.enable_imdbi ? 1 : 0);
    addint("imdbi_backend_mode", global_settings.imdbi_backend_mode);
    addint("imdbi_unroll_factor", global_settings.imdbi_unroll_factor);
    addint("imdbi_ring_size", global_settings.imdbi_ring_size);
    addint("imdbi_persistent_mapping", global_settings.imdbi_persistent_mapping ? 1 : 0);
    addint("imdbi_register_pinning", global_settings.imdbi_register_pinning ? 1 : 0);

    addint("max_glsl_cache_size_mb", (int)(global_settings.max_glsl_cache_size / 1024 / 1024));
    addint("use_program_binary_cache", global_settings.use_program_binary_cache ? 1 : 0);

    addint("fsr_enable_sharpening", global_settings.fsr_enable_sharpening ? 1 : 0);
    addint("fsr1_version", global_settings.fsr1_version);
    cJSON_AddNumberToObject(cfg, "fsr1_sharpness", global_settings.fsr1_sharpness);
    cJSON_AddNumberToObject(cfg, "fsr2_sharpness", global_settings.fsr2_sharpness);

    addint("hide_mg_env_level", (int)global_settings.hide_mg_env_level);
    addint("disable_compute_on_weak_gpu", global_settings.disable_compute_on_weak_gpu ? 1 : 0);
    addint("force_gl_get_error_skip", global_settings.force_gl_get_error_skip ? 1 : 0);
    addint("force_depth_precision_fix", global_settings.force_depth_precision_fix ? 1 : 0);
    addint("enable_ext_timer_query", global_settings.ext_timer_query ? 1 : 0);
    addint("enable_ext_compute_shader", global_settings.ext_compute_shader ? 1 : 0);
    addint("enable_ext_direct_state_access", global_settings.ext_direct_state_access ? 1 : 0);
    addint("buffer_upload_mode", global_settings.buffer_upload_mode);
    addint("texture_swizzle_mode", global_settings.texture_swizzle_mode);
    addint("max_anisotropy_override", global_settings.max_anisotropy_override);
    addint("custom_gl_version_major", global_settings.custom_gl_version.Major);
    addint("custom_gl_version_minor", global_settings.custom_gl_version.Minor);
}

void write_session_files(int final) {
    if (!g_session_active.load() && !final) return;
    ensure_sessions_dir();
    std::string dir = sessions_dir();
    if (dir.empty()) return;

    MG_FrameStats stats;
    compute_snapshot(&stats);

    uint64_t now_ms  = epoch_ms();
    uint64_t dur_ms  = now_ms - g_session_start_ms;
    char started_iso[32], ended_iso[32];
    iso8601(g_session_start_ms / 1000ULL, started_iso, sizeof(started_iso));
    iso8601(now_ms / 1000ULL, ended_iso, sizeof(ended_iso));

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "session_id", (double)g_session_id);
    cJSON_AddStringToObject(root, "started_at", started_iso);
    cJSON_AddStringToObject(root, "ended_at", ended_iso);
    cJSON_AddNumberToObject(root, "duration_ms", (double)dur_ms);

    cJSON* device = cJSON_CreateObject();
    cJSON_AddStringToObject(device, "gpu_renderer",
        glGetString ? (const char*)glGetString(GL_RENDERER) : "");
    cJSON_AddStringToObject(device, "gpu_vendor",
        glGetString ? (const char*)glGetString(GL_VENDOR) : "");
    cJSON_AddNumberToObject(device, "es_major", 0);
    cJSON_AddNumberToObject(device, "es_minor", 0);
    cJSON_AddItemToObject(root, "device", device);

    cJSON* cfg = cJSON_CreateObject();
    build_config_snapshot(cfg);
    cJSON_AddItemToObject(root, "config", cfg);

    cJSON* st = cJSON_CreateObject();
    cJSON_AddNumberToObject(st, "fps_current", stats.fps_current);
    cJSON_AddNumberToObject(st, "fps_avg",     stats.fps_avg);
    cJSON_AddNumberToObject(st, "fps_min",     stats.fps_min);
    cJSON_AddNumberToObject(st, "fps_max",     stats.fps_max);
    cJSON_AddNumberToObject(st, "fps_p95",     stats.fps_p95);
    cJSON_AddNumberToObject(st, "frame_ms_current", stats.frame_ms_current);
    cJSON_AddNumberToObject(st, "frame_ms_avg",     stats.frame_ms_avg);
    cJSON_AddNumberToObject(st, "frame_ms_min",     stats.frame_ms_min);
    cJSON_AddNumberToObject(st, "frame_ms_max",     stats.frame_ms_max);
    cJSON_AddNumberToObject(st, "frame_ms_p95",     stats.frame_ms_p95);
    cJSON_AddNumberToObject(st, "cpu_load_pct",     stats.cpu_load_pct);
    cJSON_AddNumberToObject(st, "gpu_load_pct",     stats.gpu_load_pct);
    cJSON_AddNumberToObject(st, "has_gpu_timer",    stats.has_gpu_timer);
    cJSON_AddNumberToObject(st, "frame_count",      (double)stats.frame_count);
    cJSON_AddNumberToObject(st, "dropped_frames",   (double)stats.dropped_frames);
    cJSON_AddItemToObject(root, "stats", st);

    if (g_prev.valid) {
        cJSON* d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "fps_min", stats.fps_min - g_prev.fps_min);
        cJSON_AddNumberToObject(d, "fps_avg", stats.fps_avg - g_prev.fps_avg);
        cJSON_AddNumberToObject(d, "fps_max", stats.fps_max - g_prev.fps_max);
        cJSON_AddItemToObject(root, "delta_prev", d);
    }

    char* txt = cJSON_Print(root);
    cJSON_Delete(root);
    if (!txt) return;

    if (final && g_session_id != 0) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%llu.json", dir.c_str(), (unsigned long long)g_session_id);
        FILE* f = fopen(path, "w");
        if (f) { fputs(txt, f); fclose(f); }
    }

    {
        char path[512];
        snprintf(path, sizeof(path), "%s/latest.json", dir.c_str());
        FILE* f = fopen(path, "w");
        if (f) { fputs(txt, f); fclose(f); }
    }

    if (final) {
        char path[512];
        snprintf(path, sizeof(path), "%s/summary.log", dir.c_str());
        FILE* f = fopen(path, "a");
        if (f) {
            fprintf(f,
                "%s  sess=%llu  dur=%llus  fps(min/avg/max)=%.1f/%.1f/%.1f  drop=%llu/%llu  cpu=%.1f%%  delta(min/avg/max)=%+.1f/%+.1f/%+.1f\n",
                ended_iso,
                (unsigned long long)g_session_id,
                (unsigned long long)(dur_ms / 1000),
                stats.fps_min, stats.fps_avg, stats.fps_max,
                (unsigned long long)stats.dropped_frames,
                (unsigned long long)stats.frame_count,
                stats.cpu_load_pct,
                g_prev.valid ? (stats.fps_min - g_prev.fps_min) : 0.0f,
                g_prev.valid ? (stats.fps_avg - g_prev.fps_avg) : 0.0f,
                g_prev.valid ? (stats.fps_max - g_prev.fps_max) : 0.0f);
            fclose(f);
        }
    }

    free(txt);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────
extern "C" {

void mg_profiler_init(void) {
    if (g_profiler_initialized.exchange(true)) return;
    memset(&g_ring, 0, sizeof(g_ring));
    memset(&g_session, 0, sizeof(g_session));
    g_session.frame_ms_min = DBL_MAX;
    g_tls = {};
    LOG_I("[PROFILER] initialized");
    atexit(mg_profiler_end_session);
}

void mg_profiler_begin_session(void) {
    std::lock_guard<std::mutex> lk(g_lifecycle_mutex);
    if (g_session_active.exchange(true)) return;
    g_session_id       = (uint64_t)time(NULL);
    g_session_start_ms = epoch_ms();
    g_last_flush_ms    = g_session_start_ms;

    {
        std::lock_guard<std::mutex> lk2(g_ring_mutex);
        memset(&g_ring, 0, sizeof(g_ring));
    }
    {
        std::lock_guard<std::mutex> lk2(g_session_mutex);
        memset(&g_session, 0, sizeof(g_session));
        g_session.frame_ms_min = DBL_MAX;
    }

    read_prev_session();
    LOG_I("[PROFILER] session started (id=%llu, prev_valid=%d, prev_fps_avg=%.1f)",
          (unsigned long long)g_session_id, g_prev.valid, g_prev.valid ? g_prev.fps_avg : 0.0f);
}

void mg_profiler_end_session(void) {
    std::lock_guard<std::mutex> lk(g_lifecycle_mutex);
    if (!g_session_active.exchange(false)) return;
    write_session_files(1);
    LOG_I("[PROFILER] session ended (id=%llu)", (unsigned long long)g_session_id);
}

void mg_profiler_frame_mark(void) {
    if (!g_profiler_initialized.load(std::memory_order_relaxed)) return;
    uint64_t now = now_ns();
    TLS& t = g_tls;
    if (t.last_frame_ns != 0) {
        double frame_ms = (double)(now - t.last_frame_ns) / 1e6;
        if (frame_ms > 0.1 && frame_ms < 10000.0) {
            double cpu_ms = (double)t.cpu_accum_ns / 1e6;
            if (cpu_ms > frame_ms) cpu_ms = frame_ms;
            record_frame(frame_ms, cpu_ms);
        }
    }
    t.last_frame_ns = now;
    t.cpu_accum_ns  = 0;

    uint64_t em = epoch_ms();
    if (em > g_last_flush_ms + kFlushMs) {
        g_last_flush_ms = em;
        write_session_files(0);
    }
}

void mg_profiler_cpu_begin(void) {
    if (!g_profiler_initialized.load(std::memory_order_relaxed)) return;
    TLS& t = g_tls;
    if (t.cpu_depth++ == 0) {
        t.cpu_begin_ns = now_ns();
    }
}

void mg_profiler_cpu_end(void) {
    if (!g_profiler_initialized.load(std::memory_order_relaxed)) return;
    TLS& t = g_tls;
    if (t.cpu_depth > 0 && --t.cpu_depth == 0) {
        uint64_t now = now_ns();
        if (now >= t.cpu_begin_ns) t.cpu_accum_ns += now - t.cpu_begin_ns;
    }
}

void mg_profiler_snapshot(MG_FrameStats* out) {
    if (!out) return;
    compute_snapshot(out);
}

char* mg_profiler_stats_json(void) {
    MG_FrameStats s;
    compute_snapshot(&s);

    uint64_t dur_ms = g_session_start_ms ? (epoch_ms() - g_session_start_ms) : 0;

    // Static-buffer format; kept under 2KB.
    char buf[2048];
    snprintf(buf, sizeof(buf),
        "{"
        "\"fps\":{\"current\":%.2f,\"avg\":%.2f,\"min\":%.2f,\"max\":%.2f,\"p95\":%.2f},"
        "\"frame_ms\":{\"current\":%.2f,\"avg\":%.2f,\"min\":%.2f,\"max\":%.2f,\"p95\":%.2f},"
        "\"load\":{\"cpu_pct\":%.1f,\"gpu_pct\":%.1f,\"has_gpu_timer\":%d},"
        "\"counters\":{\"frames\":%llu,\"dropped\":%llu},"
        "\"delta_prev\":{\"fps_min\":%+.2f,\"fps_avg\":%+.2f,\"fps_max\":%+.2f,\"valid\":%d},"
        "\"session\":{\"id\":%llu,\"active\":%d,\"duration_ms\":%llu}"
        "}",
        s.fps_current, s.fps_avg, s.fps_min, s.fps_max, s.fps_p95,
        s.frame_ms_current, s.frame_ms_avg, s.frame_ms_min, s.frame_ms_max, s.frame_ms_p95,
        s.cpu_load_pct, s.gpu_load_pct, s.has_gpu_timer,
        (unsigned long long)s.frame_count, (unsigned long long)s.dropped_frames,
        g_prev.valid ? (s.fps_min - g_prev.fps_min) : 0.0f,
        g_prev.valid ? (s.fps_avg - g_prev.fps_avg) : 0.0f,
        g_prev.valid ? (s.fps_max - g_prev.fps_max) : 0.0f,
        g_prev.valid ? 1 : 0,
        (unsigned long long)g_session_id,
        g_session_active.load() ? 1 : 0,
        (unsigned long long)dur_ms);

    size_t n = strlen(buf);
    char* out = (char*)malloc(n + 1);
    if (!out) return nullptr;
    memcpy(out, buf, n + 1);
    return out;
}

void mg_profiler_free_string(char* p) {
    if (p) free(p);
}

float mg_profiler_delta_fps_min(void) {
    if (!g_prev.valid) return 0.0f;
    MG_FrameStats s; compute_snapshot(&s);
    return s.fps_min - g_prev.fps_min;
}
float mg_profiler_delta_fps_avg(void) {
    if (!g_prev.valid) return 0.0f;
    MG_FrameStats s; compute_snapshot(&s);
    return s.fps_avg - g_prev.fps_avg;
}
float mg_profiler_delta_fps_max(void) {
    if (!g_prev.valid) return 0.0f;
    MG_FrameStats s; compute_snapshot(&s);
    return s.fps_max - g_prev.fps_max;
}

} // extern "C"
