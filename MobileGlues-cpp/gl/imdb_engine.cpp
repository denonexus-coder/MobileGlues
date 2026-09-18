// =============================================================================
// FILE: imdb_engine.cpp
// MobileGlues - IMDBI (Infinity MultiDraw Bi-Indirect) Engine Implementation
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// =============================================================================

#include "imdb_engine.h"
#include "buffer.h"
#include "../gles/loader.h"
#include "log.h"
#include <chrono>

#ifndef GL_MAP_PERSISTENT_BIT_EXT
#define GL_MAP_PERSISTENT_BIT_EXT 0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT_EXT
#define GL_MAP_COHERENT_BIT_EXT 0x0080
#endif

IMDBI_Dispatcher g_imdbiDispatcher;

IMDBI_Dispatcher::IMDBI_Dispatcher(const IMDBI_Config& config)
    : m_config(config), m_ring_buffer(config.ring_buffer_size) {
    m_glDrawElementsIndirect = GLES.glDrawElementsIndirect;
    m_glDrawArraysIndirect = GLES.glDrawArraysIndirect;
    m_glMultiDrawElementsIndirect = GLES.glMultiDrawElementsIndirectEXT;
}

IMDBI_Dispatcher::~IMDBI_Dispatcher() {
    shutdown();
}

bool IMDBI_Dispatcher::initialize() {
    if (m_initialized) return true;

    if (m_glDrawElementsIndirect == nullptr) m_glDrawElementsIndirect = GLES.glDrawElementsIndirect;
    if (m_glDrawArraysIndirect == nullptr) m_glDrawArraysIndirect = GLES.glDrawArraysIndirect;
    if (m_glMultiDrawElementsIndirect == nullptr) m_glMultiDrawElementsIndirect = GLES.glMultiDrawElementsIndirectEXT;

    if (m_ring_buffer.buffer == nullptr) {
        try {
            m_ring_buffer.buffer = new uint8_t[m_ring_buffer.capacity];
        } catch (...) {
            return false;
        }
    }
    
    if (m_config.use_persistent_mapping && GLES.glBufferStorageEXT && GLES.glMapBufferRange) {
        GLES.glGenBuffers(1, &m_ring_buffer.gl_buffer);
        GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_ring_buffer.gl_buffer);
        
        GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
        GLES.glBufferStorageEXT(GL_DRAW_INDIRECT_BUFFER, m_ring_buffer.capacity, nullptr, flags);
        
        void* ptr = GLES.glMapBufferRange(GL_DRAW_INDIRECT_BUFFER, 0, m_ring_buffer.capacity, flags);
        if (ptr == nullptr) {
            if (GLES.glBufferData) {
                GLES.glBufferData(GL_DRAW_INDIRECT_BUFFER, m_ring_buffer.capacity, nullptr, GL_STREAM_DRAW);
            }
        } else {
            delete[] m_ring_buffer.buffer;
            m_ring_buffer.buffer = static_cast<uint8_t*>(ptr);
        }
        GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    } else {
        if (GLES.glGenBuffers) {
            GLES.glGenBuffers(1, &m_ring_buffer.gl_buffer);
            GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_ring_buffer.gl_buffer);
            if (GLES.glBufferData) {
                GLES.glBufferData(GL_DRAW_INDIRECT_BUFFER, m_ring_buffer.capacity, nullptr, GL_STREAM_DRAW);
            }
            GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        }
    }
    
    m_initialized = true;
    return true;
}

void IMDBI_Dispatcher::shutdown() {
    if (!m_initialized) return;
    
    if (m_ring_buffer.gl_buffer != 0) {
        if (m_config.use_persistent_mapping && m_ring_buffer.buffer && GLES.glUnmapBuffer) {
            GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_ring_buffer.gl_buffer);
            GLES.glUnmapBuffer(GL_DRAW_INDIRECT_BUFFER);
        }
        if (GLES.glDeleteBuffers) {
            GLES.glDeleteBuffers(1, &m_ring_buffer.gl_buffer);
        }
        m_ring_buffer.gl_buffer = 0;
    }
    
    if (m_ring_buffer.buffer != nullptr && !m_config.use_persistent_mapping) {
        delete[] m_ring_buffer.buffer;
        m_ring_buffer.buffer = nullptr;
    }
    
    m_initialized = false;
    m_ring_buffer.reset();
}

void IMDBI_Dispatcher::reset() {
    m_ring_buffer.reset();
    g_imdbi_state_cache.clear();
}

bool IMDBI_Dispatcher::dispatch_multi_draw(
    IMDBI_DrawType draw_type,
    GLenum mode,
    GLenum type,
    const void* indirect_commands,
    GLsizei draw_count,
    GLsizei stride) {
    if (!m_initialized) {
        if (!initialize()) return false;
    }
    if (draw_count <= 0) return true;
    if (indirect_commands == nullptr) return false;

    auto t_start = std::chrono::high_resolution_clock::now();
    bool status = false;

    switch (m_config.primary_mode) {
        case IMDBI_BackendMode::STITCHING:
            status = dispatch_stitching(draw_type, mode, indirect_commands, draw_count, stride);
            break;
        case IMDBI_BackendMode::FAST_INDIRECT_RING:
            status = dispatch_fast_indirect_ring(draw_type, mode, indirect_commands, draw_count, stride);
            break;
        case IMDBI_BackendMode::UNROLLED_LOOP:
            status = dispatch_unrolled_loop(draw_type, mode, type, indirect_commands, draw_count, stride);
            break;
        case IMDBI_BackendMode::COMPUTE_DISPATCH:
            status = dispatch_compute(draw_type, mode, type, indirect_commands, draw_count, stride);
            break;
        default:
            status = false;
            break;
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    uint64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
    m_profiler.record(static_cast<uint64_t>(draw_count), elapsed_ns);

    return status;
}

bool IMDBI_Dispatcher::dispatch_stitched_draw(const IMDBI_DrawBatch& batch) {
    if (!m_initialized) {
        if (!initialize()) return false;
    }
    if (batch.empty()) return true;

    auto t_start = std::chrono::high_resolution_clock::now();

    if (!g_imdbi_state_cache.primitive_restart_enabled) {
        if (GLES.glEnable) GLES.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
        g_imdbi_state_cache.primitive_restart_enabled = true;
    }
    
    if (GLES.glBindBuffer) {
        GLES.glBindBuffer(GL_ARRAY_BUFFER, batch.vertex_buffer);
        GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, batch.index_buffer);
    }
    
    if (m_glDrawElementsIndirect) {
        for (GLsizei i = 0; i < static_cast<GLsizei>(batch.indirect_commands.size()); ++i) {
            m_glDrawElementsIndirect(batch.mode, batch.index_type, &batch.indirect_commands[i]);
        }
    } else if (GLES.glDrawElements) {
        for (GLsizei i = 0; i < static_cast<GLsizei>(batch.counts.size()); ++i) {
            GLES.glDrawElements(
                batch.mode,
                batch.counts[i],
                batch.index_type,
                reinterpret_cast<const void*>(static_cast<uintptr_t>(batch.first_indices[i]))
            );
        }
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    uint64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
    m_profiler.record(static_cast<uint64_t>(batch.counts.size()), elapsed_ns);

    return true;
}

bool IMDBI_Dispatcher::dispatch_unrolled_loop(
    IMDBI_DrawType draw_type,
    GLenum mode,
    GLenum type,
    const void* indirect_commands,
    GLsizei draw_count,
    GLsizei stride) {
    if (draw_count <= 0) return true;
    if (indirect_commands == nullptr) return false;
    
    if (m_glDrawElementsIndirect == nullptr) m_glDrawElementsIndirect = GLES.glDrawElementsIndirect;
    if (m_glDrawArraysIndirect == nullptr) m_glDrawArraysIndirect = GLES.glDrawArraysIndirect;
    if (m_glMultiDrawElementsIndirect == nullptr) m_glMultiDrawElementsIndirect = GLES.glMultiDrawElementsIndirectEXT;

    switch (draw_type) {
        case IMDBI_DrawType::DRAW_ELEMENTS_INDIRECT:
        case IMDBI_DrawType::MULTI_DRAW_ELEMENTS:
        case IMDBI_DrawType::MULTI_DRAW_ELEMENTS_INDIRECT: {
            if (m_glMultiDrawElementsIndirect != nullptr && draw_type == IMDBI_DrawType::MULTI_DRAW_ELEMENTS_INDIRECT) {
                m_glMultiDrawElementsIndirect(
                    mode,
                    GL_UNSIGNED_INT,
                    indirect_commands,
                    draw_count,
                    stride > 0 ? stride : sizeof(IMDBI_DrawElementsIndirectCommand)
                );
                return true;
            }
            if (m_glDrawElementsIndirect == nullptr) return false;
            
            if (m_config.enable_register_pinning) {
                switch (m_config.unroll_factor) {
                    case 8:
                        imdbi_unroll_draw_elements_indirect_8x(
                            m_glDrawElementsIndirect,
                            mode,
                            type,
                            indirect_commands,
                            draw_count,
                            stride > 0 ? stride : sizeof(IMDBI_DrawElementsIndirectCommand)
                        );
                        break;
                    case 4:
                    default:
                        imdbi_unroll_draw_elements_indirect_4x(
                            m_glDrawElementsIndirect,
                            mode,
                            type,
                            indirect_commands,
                            draw_count,
                            stride > 0 ? stride : sizeof(IMDBI_DrawElementsIndirectCommand)
                        );
                        break;
                }
            } else {
                const uint8_t* cmd_ptr = static_cast<const uint8_t*>(indirect_commands);
                size_t actual_stride = stride > 0 ? stride : sizeof(IMDBI_DrawElementsIndirectCommand);
                for (GLsizei i = 0; i < draw_count; ++i) {
                    m_glDrawElementsIndirect(mode, type, cmd_ptr);
                    cmd_ptr += actual_stride;
                }
            }
            return true;
        }
        
        case IMDBI_DrawType::DRAW_ARRAYS_INDIRECT:
        case IMDBI_DrawType::MULTI_DRAW_ARRAYS:
        case IMDBI_DrawType::MULTI_DRAW_ARRAYS_INDIRECT: {
            if (m_glDrawArraysIndirect == nullptr) return false;
            const uint8_t* cmd_ptr = static_cast<const uint8_t*>(indirect_commands);
            size_t actual_stride = stride > 0 ? stride : sizeof(IMDBI_DrawArraysIndirectCommand);
            for (GLsizei i = 0; i < draw_count; ++i) {
                m_glDrawArraysIndirect(mode, cmd_ptr);
                cmd_ptr += actual_stride;
            }
            return true;
        }
        
        default:
            return false;
    }
}

bool IMDBI_Dispatcher::dispatch_fast_indirect_ring(
    IMDBI_DrawType draw_type,
    GLenum mode,
    const void* indirect_commands,
    GLsizei draw_count,
    GLsizei stride
) {
    if (draw_count <= 0) return true;
    if (indirect_commands == nullptr) return false;
    
    size_t actual_stride = stride > 0 ? stride : sizeof(IMDBI_DrawElementsIndirectCommand);
    size_t required_size = draw_count * actual_stride;
    
    if (required_size > m_ring_buffer.get_free()) {
        m_ring_buffer.reset();
    }
    
    void* ring_memory = m_ring_buffer.allocate(required_size);
    if (ring_memory == nullptr) {
        return dispatch_unrolled_loop(draw_type, mode, type, indirect_commands, draw_count, stride);
    }
    
    const uint8_t* src_ptr = static_cast<const uint8_t*>(indirect_commands);
    uint8_t* dst_ptr = static_cast<uint8_t*>(ring_memory);
    
    imdbi_memcpy(dst_ptr, src_ptr, required_size);
    
    if (GLES.glBindBuffer) {
        GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_ring_buffer.gl_buffer);
    }
    
    switch (draw_type) {
        case IMDBI_DrawType::DRAW_ELEMENTS_INDIRECT:
        case IMDBI_DrawType::MULTI_DRAW_ELEMENTS:
        case IMDBI_DrawType::MULTI_DRAW_ELEMENTS_INDIRECT: {
            if (m_glMultiDrawElementsIndirect != nullptr && draw_type == IMDBI_DrawType::MULTI_DRAW_ELEMENTS_INDIRECT) {
                size_t offset = reinterpret_cast<uintptr_t>(ring_memory) - reinterpret_cast<uintptr_t>(m_ring_buffer.buffer);
                const void* offset_ptr = static_cast<const void*>(static_cast<const uint8_t*>(m_ring_buffer.buffer) + offset);
                
                m_glMultiDrawElementsIndirect(
                    mode,
                    GL_UNSIGNED_INT,
                    offset_ptr,
                    draw_count,
                    static_cast<GLsizei>(actual_stride)
                );
                return true;
            }
            if (m_glDrawElementsIndirect == nullptr) return false;
            
            size_t offset = reinterpret_cast<uintptr_t>(ring_memory) - reinterpret_cast<uintptr_t>(m_ring_buffer.buffer);
            const uint8_t* cmd_ptr = static_cast<const uint8_t*>(m_ring_buffer.buffer) + offset;
            for (GLsizei i = 0; i < draw_count; ++i) {
                m_glDrawElementsIndirect(mode, type, cmd_ptr);
                cmd_ptr += actual_stride;
            }
            return true;
        }
        
        default:
            return dispatch_unrolled_loop(draw_type, mode, type, indirect_commands, draw_count, stride);
    }
}

bool IMDBI_Dispatcher::dispatch_stitching(
    IMDBI_DrawType draw_type,
    GLenum mode,
    const void* indirect_commands,
    GLsizei draw_count,
    GLsizei stride
) {
    if (!g_imdbi_state_cache.primitive_restart_enabled) {
        if (GLES.glEnable) GLES.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
        g_imdbi_state_cache.primitive_restart_enabled = true;
        g_imdbi_state_cache.primitive_restart_index = 0xFFFFFFFF;
    }
    
    return dispatch_unrolled_loop(draw_type, mode, type, indirect_commands, draw_count, stride);
}

bool IMDBI_Dispatcher::dispatch_compute(
    IMDBI_DrawType draw_type,
    GLenum mode,
    GLenum type,
    const void* indirect_commands,
    GLsizei draw_count,
    GLsizei stride) {
    return dispatch_unrolled_loop(draw_type, mode, type, indirect_commands, draw_count, stride);
}

IMDBI_BackendMode IMDBI_Dispatcher::get_backend_mode() const { return m_config.primary_mode; }
void IMDBI_Dispatcher::set_backend_mode(IMDBI_BackendMode mode) { m_config.primary_mode = mode; }
const IMDBI_Config& IMDBI_Dispatcher::get_config() const { return m_config; }
void IMDBI_Dispatcher::set_config(const IMDBI_Config& config) {
    m_config = config;
    if (m_ring_buffer.capacity != config.ring_buffer_size) {
        shutdown();
        m_ring_buffer = IMDBI_CommandRingBuffer(config.ring_buffer_size);
        initialize();
    }
}

IMDBI_CommandRingBuffer& IMDBI_Dispatcher::get_ring_buffer() { return m_ring_buffer; }
const IMDBI_StateCache& IMDBI_Dispatcher::get_state_cache() const { return g_imdbi_state_cache; }
IMDBI_StateCache& IMDBI_Dispatcher::get_state_cache() { return g_imdbi_state_cache; }

void IMDBI_Dispatcher::update_state_cache(
    GLuint vao,
    GLuint array_buffer,
    GLuint element_buffer,
    GLuint indirect_buffer,
    bool tf_active,
    bool tf_paused
) {
    g_imdbi_state_cache.bound_vao = vao;
    g_imdbi_state_cache.bound_array_buffer = array_buffer;
    g_imdbi_state_cache.bound_element_buffer = element_buffer;
    g_imdbi_state_cache.bound_indirect_buffer = indirect_buffer;
    g_imdbi_state_cache.tf_active = tf_active;
    g_imdbi_state_cache.tf_paused = tf_paused;
}
