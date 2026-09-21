// MobileGlues - gl/frame_profiler.h
// Frame timing profiler — FPS, frame time, CPU/GPU split, session persistence.
//
// Design constraints:
//   * zero overhead when not initialized (a single atomic bool check)
//   * frame_mark runs once per eglSwapBuffers (~50ns total)
//   * CPU marks are cheap (thread-local depth counter + clock_gettime)
//   * no unbounded allocation, no locks on the hot path
//   * session files are written periodically (10s) and at exit
//   * everything degrades gracefully if the filesystem is unavailable
// Copyright (c) 2025-2026 MobileGL-Dev
// SPDX-License-Identifier: LGPL-2.1-only
#ifndef MOBILEGLUES_FRAME_PROFILER_H
#define MOBILEGLUES_FRAME_PROFILER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MG_FrameStats {
    // FPS (over the whole session)
    float fps_current;
    float fps_avg;
    float fps_min;   // worst frame = 1000 / max_frame_ms
    float fps_max;   // best frame  = 1000 / min_frame_ms
    float fps_p95;

    // Frame time (ms)
    float frame_ms_current;
    float frame_ms_avg;
    float frame_ms_min;
    float frame_ms_max;
    float frame_ms_p95;

    // CPU / GPU split
    float cpu_load_pct;   // % of frame time spent in GL calls (measured)
    float gpu_load_pct;   // 100 - cpu_load_pct (inferred, no timer query on PowerVR)
    int   has_gpu_timer;  // 0 for PowerVR GE8320, 1 if GL_EXT_disjoint_timer_query available

    // Counters
    uint64_t frame_count;
    uint64_t dropped_frames;  // frames > 33.34ms (< 30fps)
} MG_FrameStats;

// ─── Lifecycle ──────────────────────────────────────────────────────────────
// Called once from proc_init(). Registers atexit for final session flush.
void mg_profiler_init(void);

// Called once after mg_apply_all_settings_from_json(). Reads latest.json from
// the previous session for delta comparison.
void mg_profiler_begin_session(void);

// Called from atexit() and optionally from eglTerminate(). Writes final
// session files.
void mg_profiler_end_session(void);

// ─── Per-frame / per-draw ───────────────────────────────────────────────────
// Called at the top of every eglSwapBuffers (or damage swap). Measures the
// time since the previous call = one frame period.
void mg_profiler_frame_mark(void);

// Bracket a CPU-heavy GL call. Thread-local depth counter handles nesting.
void mg_profiler_cpu_begin(void);
void mg_profiler_cpu_end(void);

// ─── Read ───────────────────────────────────────────────────────────────────
// Thread-safe snapshot of the current stats.
void mg_profiler_snapshot(MG_FrameStats* out);

// JSON export for FFM (Java 21). Returns a malloc'd string.
// Caller must free() via mg_profiler_free_string().
char* mg_profiler_stats_json(void);
void  mg_profiler_free_string(char* p);

// Deltas vs the previous session (0 if none / first run).
// Positive = current session is better.
float mg_profiler_delta_fps_min(void);
float mg_profiler_delta_fps_avg(void);
float mg_profiler_delta_fps_max(void);

#ifdef __cplusplus
}
#endif

#endif // MOBILEGLUES_FRAME_PROFILER_H
