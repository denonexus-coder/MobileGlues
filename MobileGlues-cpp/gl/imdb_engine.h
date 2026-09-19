// =============================================================================
// FILE: imdb_engine.h
// MobileGlues - IMDBI (Infinity MultiDraw Bi-Indirect) Engine Header
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// =============================================================================

#ifndef IMDB_ENGINE_H
#define IMDB_ENGINE_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <vector>
#include <array>
#include <atomic>
#include <chrono>

#include <GLES3/gl32.h>
#include "../gles/loader.h"
#include "mg_vmdi_config.h"
#include "log.h"

// =============================================================================
// MACROS: Alignment, Cache Lines, and SIMD Helper
// =============================================================================

#define IMDB_CACHE_LINE_SIZE 128
#define IMDB_ALIGN_CACHE_LINE __attribute__((aligned(IMDB_CACHE_LINE_SIZE)))
#define IMDB_ALIGN(N) __attribute__((aligned(N)))
#define IMDB_PACKED __attribute__((packed))
#define IMDB_ALWAYS_INLINE __attribute__((always_inline)) inline
#define IMDB_NOINLINE __attribute__((noinline))
#define IMDB_RESTRICT __restrict

#define IMDB_LIKELY(x) __builtin_expect(!!(x), 1)
#define IMDB_UNLIKELY(x) __builtin_expect(!!(x), 0)

#define IMDB_ROUND_UP(value, N) (((value) + (N) - 1) / (N) * (N))
#define IMDB_ROUND_DOWN(value, N) ((value) / (N) * (N))
#define IMDB_IS_POWER_OF_2(x) ((x) != 0 && ((x) & ((x) - 1)) == 0)
#define IMDB_ALIGN_PTR(ptr, N) (reinterpret_cast<uintptr_t>(ptr + (N) - 1) & ~((N) - 1))

// =============================================================================
// ENUMS: IMDBI Backend Modes & Draw Types
// =============================================================================

enum class IMDBI_BackendMode : uint8_t {
    STITCHING = 0,
    FAST_INDIRECT_RING = 1,
    UNROLLED_LOOP = 2,
    COMPUTE_DISPATCH = 3,
    INVALID = 255
};

enum class IMDBI_DrawType : uint8_t {
    DRAW_ARRAYS = 0,
    DRAW_ELEMENTS = 1,
    DRAW_ARRAYS_INDIRECT = 2,
    DRAW_ELEMENTS_INDIRECT = 3,
    MULTI_DRAW_ARRAYS = 4,
    MULTI_DRAW_ELEMENTS = 5,
    MULTI_DRAW_ARRAYS_INDIRECT = 6,
    MULTI_DRAW_ELEMENTS_INDIRECT = 7,
};

// =============================================================================
// STRUCTS: CPU Shadow State Cache
// =============================================================================

struct IMDB_ALIGN_CACHE_LINE IMDBI_StateCache {
    GLuint bound_vao = 0;
    GLuint bound_array_buffer = 0;
    GLuint bound_element_buffer = 0;
    GLuint bound_indirect_buffer = 0;
    
    bool tf_active = false;
    bool tf_paused = false;
    
    bool primitive_restart_enabled = false;
    GLuint primitive_restart_index = 0;
    
    void clear() {
        bound_vao = 0;
        bound_array_buffer = 0;
        bound_element_buffer = 0;
        bound_indirect_buffer = 0;
        tf_active = false;
        tf_paused = false;
        primitive_restart_enabled = false;
        primitive_restart_index = 0;
    }
    
    bool is_vao_bound(GLuint vao) const { return bound_vao == vao; }
    bool is_indirect_buffer_bound(GLuint buffer) const { return bound_indirect_buffer == buffer; }
    bool is_tf_active() const { return tf_active && !tf_paused; }
};

static thread_local IMDBI_StateCache g_imdbi_state_cache;

// =============================================================================
// STRUCTS: Draw Commands
// =============================================================================

struct IMDBI_DrawElementsIndirectCommand {
    GLuint count;
    GLuint instanceCount;
    GLuint firstIndex;
    GLuint baseVertex;
    GLuint baseInstance;
};

struct IMDBI_DrawArraysIndirectCommand {
    GLuint count;
    GLuint instanceCount;
    GLuint first;
    GLuint baseInstance;
};

// =============================================================================
// STRUCTS: Command Ring Buffer
// =============================================================================

struct IMDBI_CommandRingBuffer {
    static constexpr size_t DEFAULT_RING_SIZE = 4 * 1024 * 1024;
    static constexpr size_t MIN_RING_SIZE = 256 * 1024;
    static constexpr size_t MAX_RING_SIZE = 16 * 1024 * 1024;
    
    uint8_t* buffer = nullptr;
    size_t capacity = 0;
    size_t head = 0;
    size_t tail = 0;
    size_t used = 0;
    
    GLuint gl_buffer = 0;
    
    explicit IMDBI_CommandRingBuffer(size_t size = DEFAULT_RING_SIZE) {
        if (size < MIN_RING_SIZE) size = MIN_RING_SIZE;
        if (size > MAX_RING_SIZE) size = MAX_RING_SIZE;
        capacity = size;
        buffer = new uint8_t[capacity];
        head = 0;
        tail = 0;
        used = 0;
    }
    
    ~IMDBI_CommandRingBuffer() {
        if (buffer && gl_buffer == 0) {
            delete[] buffer;
        }
        buffer = nullptr;
    }
    
    void* allocate(size_t bytes, size_t alignment = alignof(uint32_t)) {
        if (bytes == 0) return nullptr;
        size_t aligned_head = IMDB_ROUND_UP(head, alignment);
        
        if (aligned_head + bytes > capacity) {
            if (bytes > capacity) return nullptr;
            aligned_head = 0;
        }
        
        if (aligned_head < tail) {
            if (tail + (capacity - aligned_head) < bytes) return nullptr;
        } else {
            if (capacity - aligned_head + tail < bytes) return nullptr;
        }
        
        void* ptr = buffer + aligned_head;
        head = aligned_head + bytes;
        used += bytes;
        if (head >= capacity) head = 0;
        return ptr;
    }
    
    void reset() { head = 0; tail = 0; used = 0; }
    void advance_tail(size_t bytes) {
        tail += bytes;
        used -= bytes;
        if (tail >= capacity) tail = 0;
    }
    bool empty() const { return used == 0; }
    bool full() const { return used >= capacity; }
    size_t get_used() const { return used; }
    size_t get_free() const { return capacity - used; }
};

// =============================================================================
// STRUCTS: Draw Batch
// =============================================================================

struct IMDBI_DrawBatch {
    IMDBI_DrawType type = IMDBI_DrawType::DRAW_ELEMENTS;
    GLenum mode = GL_TRIANGLES;
    GLenum index_type = GL_UNSIGNED_INT;
    GLuint vertex_buffer = 0;
    GLuint index_buffer = 0;
    
    std::vector<GLuint> counts;
    std::vector<GLuint> first_indices;
    std::vector<GLuint> base_vertices;
    std::vector<IMDBI_DrawElementsIndirectCommand> indirect_commands;
    GLsizei total_count = 0;
    
    void clear() {
        counts.clear();
        first_indices.clear();
        base_vertices.clear();
        indirect_commands.clear();
        total_count = 0;
    }
    
    void add_draw(GLsizei count, GLuint first_index = 0, GLuint base_vertex = 0) {
        counts.push_back(static_cast<GLuint>(count));
        first_indices.push_back(first_index);
        base_vertices.push_back(base_vertex);
        total_count += count;
    }
    
    bool empty() const { return counts.empty(); }
};

// =============================================================================
// STRUCTS: Config & Profiler
// =============================================================================

struct IMDBI_Config {
    IMDBI_BackendMode primary_mode = IMDBI_BackendMode::FAST_INDIRECT_RING;
    IMDBI_BackendMode fallback_mode = IMDBI_BackendMode::UNROLLED_LOOP;
    size_t ring_buffer_size = IMDBI_CommandRingBuffer::DEFAULT_RING_SIZE;
    bool enable_primitive_restart = true;
    bool enable_register_pinning = true;
    uint32_t unroll_factor = 4;
    bool use_persistent_mapping = true;
    bool use_explicit_sync = false;
    uint32_t max_batch_size = 1024;
    uint32_t max_indirect_commands = 4096;
};

struct IMDBI_ProfilerStats {
    std::atomic<uint64_t> total_dispatches{0};
    std::atomic<uint64_t> total_commands{0};
    std::atomic<uint64_t> last_dispatch_ns{0};
    std::atomic<uint64_t> accum_dispatch_ns{0};
    std::atomic<uint64_t> sample_count{0};

    void record(uint64_t commands, uint64_t elapsed_ns) {
        total_dispatches.fetch_add(1, std::memory_order_relaxed);
        total_commands.fetch_add(commands, std::memory_order_relaxed);
        last_dispatch_ns.store(elapsed_ns, std::memory_order_relaxed);
        accum_dispatch_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
        sample_count.fetch_add(1, std::memory_order_relaxed);
    }

    void get_metrics(double& avg_us, uint64_t& last_us, uint64_t& dispatches, uint64_t& commands) const {
        dispatches = total_dispatches.load(std::memory_order_relaxed);
        commands = total_commands.load(std::memory_order_relaxed);
        uint64_t sc = sample_count.load(std::memory_order_relaxed);
        uint64_t accum = accum_dispatch_ns.load(std::memory_order_relaxed);
        uint64_t last = last_dispatch_ns.load(std::memory_order_relaxed);
        avg_us = (sc > 0) ? ((double)accum / (double)sc / 1000.0) : 0.0;
        last_us = last / 1000;
    }
};

// =============================================================================
// CLASS: IMDBI Dispatcher
// =============================================================================

class IMDBI_Dispatcher {
public:
    explicit IMDBI_Dispatcher(const IMDBI_Config& config = IMDBI_Config());
    ~IMDBI_Dispatcher();

    bool initialize();
    void shutdown();
    void reset();

    bool dispatch_multi_draw(
        IMDBI_DrawType draw_type,
        GLenum mode,
        GLenum type,
        const void* indirect_commands,
        GLsizei draw_count,
        GLsizei stride = 0
    );

    bool dispatch_stitched_draw(const IMDBI_DrawBatch& batch);

    IMDBI_BackendMode get_backend_mode() const;
    void set_backend_mode(IMDBI_BackendMode mode);
    const IMDBI_Config& get_config() const;
    void set_config(const IMDBI_Config& config);
    IMDBI_CommandRingBuffer& get_ring_buffer();
    const IMDBI_StateCache& get_state_cache() const;
    IMDBI_StateCache& get_state_cache();
    const IMDBI_ProfilerStats& get_profiler_stats() const { return m_profiler; }

    void update_state_cache(
        GLuint vao = 0,
        GLuint array_buffer = 0,
        GLuint element_buffer = 0,
        GLuint indirect_buffer = 0,
        bool tf_active = false,
        bool tf_paused = false
    );

private:
    IMDBI_Config m_config;
    IMDBI_CommandRingBuffer m_ring_buffer;
    IMDBI_ProfilerStats m_profiler;
    bool m_initialized = false;

    using glDrawElementsIndirectFunc = void (*)(GLenum, GLenum, const void*);
    using glDrawArraysIndirectFunc = void (*)(GLenum, const void*);
    using glMultiDrawElementsIndirectFunc = void (*)(GLenum, GLenum, const void*, GLsizei, GLsizei);

    glDrawElementsIndirectFunc m_glDrawElementsIndirect = nullptr;
    glDrawArraysIndirectFunc m_glDrawArraysIndirect = nullptr;
    glMultiDrawElementsIndirectFunc m_glMultiDrawElementsIndirect = nullptr;

    bool dispatch_unrolled_loop(
        IMDBI_DrawType draw_type,
        GLenum mode,
        GLenum type,
        const void* indirect_commands,
        GLsizei draw_count,
        GLsizei stride
    );

    bool dispatch_fast_indirect_ring(
        IMDBI_DrawType draw_type,
        GLenum mode,
        GLenum type,
        const void* indirect_commands,
        GLsizei draw_count,
        GLsizei stride
    );

    bool dispatch_stitching(
        IMDBI_DrawType draw_type,
        GLenum mode,
        GLenum type,
        const void* indirect_commands,
        GLsizei draw_count,
        GLsizei stride
    );

    bool dispatch_compute(
        IMDBI_DrawType draw_type,
        GLenum mode,
        GLenum type,
        const void* indirect_commands,
        GLsizei draw_count,
        GLsizei stride
    );
};

extern IMDBI_Dispatcher g_imdbiDispatcher;

// =============================================================================
// INLINE FUNCTIONS
// =============================================================================

static IMDB_ALWAYS_INLINE IMDBI_StateCache& imdbi_get_state_cache() {
    return g_imdbi_state_cache;
}

static IMDB_ALWAYS_INLINE bool imdbi_is_vao_bound(GLuint vao) {
    return g_imdbi_state_cache.is_vao_bound(vao);
}

static IMDB_ALWAYS_INLINE bool imdbi_is_indirect_buffer_bound(GLuint buffer) {
    return g_imdbi_state_cache.is_indirect_buffer_bound(buffer);
}

static IMDB_ALWAYS_INLINE bool imdbi_is_tf_active() {
    return g_imdbi_state_cache.is_tf_active();
}

template <size_t N>
constexpr size_t imdbi_align(size_t value) {
    static_assert(IMDB_IS_POWER_OF_2(N), "Alignment must be a power of 2");
    return IMDB_ROUND_UP(value, N);
}

static IMDB_ALWAYS_INLINE void imdbi_memcpy(void* IMDB_RESTRICT dest, const void* IMDB_RESTRICT src, size_t bytes) {
    std::memcpy(dest, src, bytes);
}

static IMDB_ALWAYS_INLINE void imdbi_unroll_draw_elements_indirect_4x(
    void (*fnDraw)(GLenum, GLenum, const void*),
    GLenum mode,
    GLenum type,
    const void* commands,
    GLsizei count,
    size_t stride = sizeof(IMDBI_DrawElementsIndirectCommand)
) {
    const uint8_t* cmd_ptr = static_cast<const uint8_t*>(commands);
    GLsizei i = 0;
    for (; i + 4 <= count; i += 4) {
        fnDraw(mode, type, cmd_ptr); cmd_ptr += stride;
        fnDraw(mode, type, cmd_ptr); cmd_ptr += stride;
        fnDraw(mode, type, cmd_ptr); cmd_ptr += stride;
        fnDraw(mode, type, cmd_ptr); cmd_ptr += stride;
    }
    for (; i < count; ++i) {
        fnDraw(mode, type, cmd_ptr); cmd_ptr += stride;
    }
}

static IMDB_ALWAYS_INLINE void imdbi_unroll_draw_elements_indirect_8x(
    void (*fnDraw)(GLenum, GLenum, const void*),
    GLenum mode,
    GLenum type,
    const void* commands,
    GLsizei count,
    size_t stride = sizeof(IMDBI_DrawElementsIndirectCommand)
) {
    const uint8_t* cmd_ptr = static_cast<const uint8_t*>(commands);
    GLsizei i = 0;
    for (; i + 8 <= count; i += 8) {
        fnDraw(mode, type, cmd_ptr); cmd_ptr += stride;
        fnDraw(mode, type, cmd_ptr); cmd_ptr += stride;
        fnDraw(mode, type, cmd_ptr); cmd_ptr += stride;
        fnDraw(mode, type, cmd_ptr); cmd_ptr += stride;
        fnDraw(mode, type, cmd_ptr); cmd_ptr += stride;
        fnDraw(mode, type, cmd_ptr); cmd_ptr += stride;
        fnDraw(mode, type, cmd_ptr); cmd_ptr += stride;
        fnDraw(mode, type, cmd_ptr); cmd_ptr += stride;
    }
    for (; i < count; ++i) {
        fnDraw(mode, type, cmd_ptr); cmd_ptr += stride;
    }
}

#endif // IMDB_ENGINE_H
