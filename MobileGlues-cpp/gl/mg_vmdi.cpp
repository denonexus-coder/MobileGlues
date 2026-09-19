// MobileGlues - gl/mg_vmdi.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "mg_vmdi.h"
#include "imdb_engine.h"
#include "buffer.h"
#include "../gles/loader.h"
#include "log.h"
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <vector>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define USE_ARM_NEON 1
#endif

#ifndef GL_MAP_PERSISTENT_BIT_EXT
#define GL_MAP_PERSISTENT_BIT_EXT 0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT_EXT
#define GL_MAP_COHERENT_BIT_EXT 0x0080
#endif

MG_VMDI_Engine g_vmdiEngine;
static MG_MultiDrawMode g_CurrentMode = MG_MultiDrawMode::LEGACY_MOBILEGLUES;

extern "C" {
    void mg_vmdi_set_mode(MG_MultiDrawMode mode) {
        g_CurrentMode = mode;
        if (g_CurrentMode == MG_MultiDrawMode::MG_IMDBI_OPTIMIZED) {
            g_imdbiDispatcher.initialize();
        }
    // =========================================================================
    // LOG DETALHADO DE CAPACIDADES MULTIDRAW
    // =========================================================================
    LOG_I("");
    LOG_I("=====================================================================");
    LOG_I("MobileGlues MultiDraw Subsystem — Capability Report");
    LOG_I("=====================================================================");

    const char* md_mode_name = mg_get_multidraw_engine_name();
    LOG_I("  Active Mode: %s", md_mode_name);

    const bool has_mdi = g_gles_caps.GL_EXT_multi_draw_indirect &&
                         GLES.glMultiDrawElementsIndirectEXT != nullptr;
    const bool has_mda = mg_multi_draw_arrays_ext_available();
    const bool has_bv  = mg_multi_draw_elements_basevertex_ext_available();
    const bool has_ind = GLES.glDrawElementsIndirect != nullptr;
    const bool has_bst = g_gles_caps.GL_EXT_buffer_storage;
    const bool has_cmp = GLES.glDispatchCompute != nullptr;

    LOG_I("  GL_EXT_multi_draw_indirect : %s", has_mdi ? "YES" : "NO ");
    LOG_I("  GL_EXT_multi_draw_arrays   : %s", has_mda ? "YES" : "NO ");
    LOG_I("  draw_elements_base_vertex  : %s", has_bv  ? "YES" : "NO ");
    LOG_I("  glDrawElementsIndirect     : %s", has_ind ? "YES" : "NO ");
    LOG_I("  GL_EXT_buffer_storage      : %s", has_bst ? "YES" : "NO ");
    LOG_I("  glDispatchCompute          : %s", has_cmp ? "YES" : "NO ");

    const char* backend_rank = "N/A";
    if (has_mdi)      backend_rank = "MDI native > Indirect > Compute > Unroll";
    else if (has_bv)  backend_rank = "MultiBaseVertex > Indirect > Compute > Unroll";
    else if (has_ind) backend_rank = "Indirect > Compute > Unroll";
    else              backend_rank = "Compute > Unroll (fallback chain)";

    LOG_I("  Effective Backend Rank: %s", backend_rank);

    const char* engine_status = "Legacy (no active engine)";
    if (g_CurrentMode == MG_MultiDrawMode::MG_VMDI_OPTIMIZED)
        engine_status = "VMDI active (ring buffer + compact/fuse + autotuner)";
    else if (g_CurrentMode == MG_MultiDrawMode::MG_IMDBI_OPTIMIZED)
        engine_status = "IMDBI active (persistent ring + state cache + 4x/8x unroll)";

    LOG_I("  Engine Status: %s", engine_status);
    LOG_I("=====================================================================");
    LOG_I("");
    // =========================================================================

    }

    MG_MultiDrawMode mg_vmdi_get_mode() {
        return g_CurrentMode;
    }

    void mg_vmdi_toggle_mode() {
        if (g_CurrentMode == MG_MultiDrawMode::LEGACY_MOBILEGLUES) {
            mg_vmdi_set_mode(MG_MultiDrawMode::MG_VMDI_OPTIMIZED);
        } else if (g_CurrentMode == MG_MultiDrawMode::MG_VMDI_OPTIMIZED) {
            mg_vmdi_set_mode(MG_MultiDrawMode::MG_IMDBI_OPTIMIZED);
        } else {
            mg_vmdi_set_mode(MG_MultiDrawMode::LEGACY_MOBILEGLUES);
        }
    }

    const char* mg_get_multidraw_engine_name() {
        switch (g_CurrentMode) {
        case MG_MultiDrawMode::LEGACY_MOBILEGLUES:
            return "Original (Legacy)";
        case MG_MultiDrawMode::MG_VMDI_OPTIMIZED:
            return "VMDI (Virtual MDI)";
        case MG_MultiDrawMode::MG_IMDBI_OPTIMIZED:
            return "IMDBI (Bi-Indirect)";
        default:
            return "Unknown";
        }
    }

    static int g_ForcedTier = -1;  // -1 = auto

    void mg_vmdi_set_tier(int tier) {
        if (tier < -1 || tier > 3) tier = -1;
        g_ForcedTier = tier;
        if (tier >= 0) {
            g_vmdiEngine.SetTierForced(static_cast<BackendTier>(tier));
            LOG_I("[MobileGlues] VMDI tier forced to %d", tier);
        } else {
            LOG_I("[MobileGlues] VMDI tier = auto (autotuner active)");
        }
    }

    int mg_vmdi_get_tier() {
        return g_ForcedTier;
    }

    static char s_profiler_str_buf[256];
    const char* mg_get_multidraw_profiler_string() {
        switch (g_CurrentMode) {
        case MG_MultiDrawMode::MG_IMDBI_OPTIMIZED: {
            double avg_us = 0.0;
            uint64_t last_us = 0;
            uint64_t dispatches = 0;
            uint64_t commands = 0;
            g_imdbiDispatcher.get_profiler_stats().get_metrics(avg_us, last_us, dispatches, commands);
            snprintf(s_profiler_str_buf, sizeof(s_profiler_str_buf),
                     "MultiDraw: IMDBI | Calls: %llu | Cmds: %llu | Avg: %.2fus",
                     (unsigned long long)dispatches, (unsigned long long)commands, avg_us);
            return s_profiler_str_buf;
        }
        case MG_MultiDrawMode::MG_VMDI_OPTIMIZED: {
            snprintf(s_profiler_str_buf, sizeof(s_profiler_str_buf),
                     "MultiDraw: VMDI (Virtual MDI) | Active");
            return s_profiler_str_buf;
        }
        case MG_MultiDrawMode::LEGACY_MOBILEGLUES:
        default: {
            snprintf(s_profiler_str_buf, sizeof(s_profiler_str_buf),
                     "MultiDraw: Original MobileGlues (Legacy) | Active");
            return s_profiler_str_buf;
        }
        }
    }

    void mg_init_multidraw_subsystem(const char* glExtensions) {
        g_vmdiEngine.Init(glExtensions);
        if (g_CurrentMode == MG_MultiDrawMode::MG_IMDBI_OPTIMIZED) {
            g_imdbiDispatcher.initialize();
        }
    }
}

MG_VMDI_Engine::MG_VMDI_Engine() 
    : currentTier(BackendTier::DIRECT_FALLBACK),
      hasBufferStorage(false),
      hasBaseVertex(false),
      initialized(false) {
    memset(&ringBuffer, 0, sizeof(ringBuffer));
    memset(&tuner, 0, sizeof(tuner));
    ringBuffer.slotSize = MG_VMDI_MAX_DRAWS * sizeof(DrawElementsIndirectCommand);
}

MG_VMDI_Engine::~MG_VMDI_Engine() {
    Destroy();
}

void MG_VMDI_Engine::Init(const char* glExtensions) {
    if (initialized) return;

    // Checagem de leitura da Configuração inicial via Env Var (Opcional)
    const char* envMode = getenv("MG_VMDI_ENABLE");
    if (envMode) {
        g_CurrentMode = (strcmp(envMode, "1") == 0 || strcasecmp(envMode, "true") == 0)
                        ? MG_MultiDrawMode::MG_VMDI_OPTIMIZED 
                        : MG_MultiDrawMode::LEGACY_MOBILEGLUES;
    }

    if (!glExtensions && GLES.glGetString) {
        glExtensions = reinterpret_cast<const char*>(GLES.glGetString(GL_EXTENSIONS));
    }

    hasBufferStorage = g_gles_caps.GL_EXT_buffer_storage || 
                       (glExtensions && strstr(glExtensions, "GL_EXT_buffer_storage") != nullptr);
    hasBaseVertex = g_gles_caps.GL_OES_draw_elements_base_vertex || 
                    g_gles_caps.GL_EXT_draw_elements_base_vertex ||
                    (GLES.glDrawElementsBaseVertex != nullptr) ||
                    (glExtensions && (strstr(glExtensions, "GL_OES_draw_elements_base_vertex") != nullptr ||
                                      strstr(glExtensions, "GL_EXT_draw_elements_base_vertex") != nullptr));

    if (g_gles_caps.GL_EXT_multi_draw_indirect && (GLES.glMultiDrawElementsIndirectEXT != nullptr)) {
        currentTier = BackendTier::NATIVE_MDI;
    } else if (hasBaseVertex) {
        currentTier = BackendTier::MULTI_BASE_VERTEX;
    } else if (GLES.glDrawElementsIndirect != nullptr) {
        currentTier = BackendTier::INDIRECT_UNROLLED;
    } else {
        currentTier = BackendTier::DIRECT_FALLBACK;
    }

    InitRingBuffer();
    initialized = true;
}

void MG_VMDI_Engine::InitRingBuffer() {
    if (!GLES.glGenBuffers) return;

    GLES.glGenBuffers(1, &ringBuffer.bufferId);
    GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, ringBuffer.bufferId);

    size_t totalAllocSize = ringBuffer.slotSize * MG_VMDI_FRAMES_IN_FLIGHT;

    if (hasBufferStorage && GLES.glBufferStorageEXT && GLES.glMapBufferRange) {
        GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
        GLES.glBufferStorageEXT(GL_DRAW_INDIRECT_BUFFER, totalAllocSize, nullptr, flags);
        ringBuffer.mappedPtr = static_cast<uint8_t*>(GLES.glMapBufferRange(
            GL_DRAW_INDIRECT_BUFFER, 0, totalAllocSize, flags
        ));
    } else {
        if (GLES.glBufferData) {
            GLES.glBufferData(GL_DRAW_INDIRECT_BUFFER, totalAllocSize, nullptr, GL_DYNAMIC_DRAW);
        }
        ringBuffer.mappedPtr = nullptr;
    }

    for (int i = 0; i < MG_VMDI_FRAMES_IN_FLIGHT; ++i) {
        ringBuffer.fences[i] = nullptr;
    }
    GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
}

void MG_VMDI_Engine::Destroy() {
    if (!initialized) return;

    for (int i = 0; i < MG_VMDI_FRAMES_IN_FLIGHT; ++i) {
        if (ringBuffer.fences[i]) {
            if (GLES.glDeleteSync) {
                GLES.glDeleteSync(ringBuffer.fences[i]);
            }
            ringBuffer.fences[i] = nullptr;
        }
    }

    if (ringBuffer.bufferId != 0) {
        if (hasBufferStorage && ringBuffer.mappedPtr && GLES.glUnmapBuffer) {
            GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, ringBuffer.bufferId);
            GLES.glUnmapBuffer(GL_DRAW_INDIRECT_BUFFER);
            ringBuffer.mappedPtr = nullptr;
        }
        if (GLES.glDeleteBuffers) {
            GLES.glDeleteBuffers(1, &ringBuffer.bufferId);
        }
        ringBuffer.bufferId = 0;
    }
    initialized = false;
}

void MG_VMDI_Engine::SynchronizeSlot(uint32_t slot) {
    if (ringBuffer.fences[slot] != nullptr) {
        if (GLES.glClientWaitSync) {
            GLenum result = GLES.glClientWaitSync(ringBuffer.fences[slot], 0, 0);
            if (result == GL_TIMEOUT_EXPIRED || result == GL_WAIT_FAILED) {
                GLES.glClientWaitSync(ringBuffer.fences[slot], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000);
            }
        }
        if (GLES.glDeleteSync) {
            GLES.glDeleteSync(ringBuffer.fences[slot]);
        }
        ringBuffer.fences[slot] = nullptr;
    }
}

// Compactação e Fusão com Otimização de Cache e Branch Reduction
uint32_t MG_VMDI_Engine::CompactAndFuseCommands(const DrawElementsIndirectCommand* __restrict src, 
                                                DrawElementsIndirectCommand* __restrict dst, 
                                                uint32_t count) {
    if (__builtin_expect(count == 0, 0)) return 0;
    if (count > MG_VMDI_MAX_DRAWS) count = MG_VMDI_MAX_DRAWS;

    uint32_t writeIdx = 0;
    uint32_t readIdx = 0;

    for (; readIdx < count; ++readIdx) {
#if USE_ARM_NEON
        while (readIdx + 4 <= count) {
            uint32x4_t counts = {
                src[readIdx + 0].count,
                src[readIdx + 1].count,
                src[readIdx + 2].count,
                src[readIdx + 3].count
            };
            uint32x4_t zeroVec = vdupq_n_u32(0);
            uint32x4_t cmp = vceqq_u32(counts, zeroVec);
            uint64x2_t cmp64 = vreinterpretq_u64_u32(cmp);
            
            // Se todos os 4 comandos são count == 0, salta direto
            if (vgetq_lane_u64(cmp64, 0) == 0xFFFFFFFFFFFFFFFFULL && 
                vgetq_lane_u64(cmp64, 1) == 0xFFFFFFFFFFFFFFFFULL) {
                readIdx += 4;
                continue;
            }
            break;
        }
        if (readIdx >= count) break;
#endif
        const DrawElementsIndirectCommand& current = src[readIdx];
        if (__builtin_expect(current.count == 0, 0)) continue;

        if (writeIdx > 0) {
            DrawElementsIndirectCommand& prev = dst[writeIdx - 1];
            // Fusão de Chunks do Sodium contíguos na mesma Slice de VBO
            if (prev.baseVertex == current.baseVertex &&
                (prev.firstIndex + prev.count) == current.firstIndex) {
                prev.count += current.count;
                continue;
            }
        }

        dst[writeIdx++] = current;
    }

    return writeIdx;
}

void MG_VMDI_Engine::Dispatch(GLenum mode, GLenum type, const void* indirect, 
                              uint32_t drawCount, uint32_t stride) {
    if (__builtin_expect(drawCount == 0, 0)) return;
    if (!initialized) Init(nullptr);

    auto startTime = std::chrono::high_resolution_clock::now();

    if (stride == 0) stride = sizeof(DrawElementsIndirectCommand);

    const GLuint prevIndirect = mg_driver_bound_buffer(GL_DRAW_INDIRECT_BUFFER);
    const DrawElementsIndirectCommand* rawCommands = nullptr;
    std::vector<DrawElementsIndirectCommand> tempCommands;
    bool mappedInput = false;

    if (prevIndirect != 0) {
        size_t totalBytes = static_cast<size_t>(drawCount) * stride;
        GLintptr offset = reinterpret_cast<GLintptr>(indirect);
        const uint8_t* mapped = nullptr;
        if (GLES.glMapBufferRange) {
            mapped = static_cast<const uint8_t*>(GLES.glMapBufferRange(GL_DRAW_INDIRECT_BUFFER, offset, totalBytes, GL_MAP_READ_BIT));
        }
        if (mapped) {
            mappedInput = true;
            if (stride == sizeof(DrawElementsIndirectCommand)) {
                rawCommands = reinterpret_cast<const DrawElementsIndirectCommand*>(mapped);
            } else {
                tempCommands.resize(drawCount);
                for (uint32_t i = 0; i < drawCount; ++i) {
                    tempCommands[i] = *reinterpret_cast<const DrawElementsIndirectCommand*>(mapped + i * stride);
                }
                rawCommands = tempCommands.data();
            }
        }
    } else if (indirect != nullptr) {
        if (stride == sizeof(DrawElementsIndirectCommand)) {
            rawCommands = reinterpret_cast<const DrawElementsIndirectCommand*>(indirect);
        } else {
            const uint8_t* srcBytes = reinterpret_cast<const uint8_t*>(indirect);
            tempCommands.resize(drawCount);
            for (uint32_t i = 0; i < drawCount; ++i) {
                tempCommands[i] = *reinterpret_cast<const DrawElementsIndirectCommand*>(srcBytes + i * stride);
            }
            rawCommands = tempCommands.data();
        }
    }

    if (!rawCommands) {
        if (g_gles_caps.GL_EXT_multi_draw_indirect && GLES.glMultiDrawElementsIndirectEXT) {
            GLES.glMultiDrawElementsIndirectEXT(mode, type, indirect, drawCount, stride);
        } else if (GLES.glDrawElementsIndirect) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(indirect);
            for (uint32_t i = 0; i < drawCount; ++i) {
                GLES.glDrawElementsIndirect(
                    mode, type,
                    reinterpret_cast<const void*>(base + static_cast<uintptr_t>(i) * static_cast<uintptr_t>(stride)));
            }
        }
        return;
    }

    uint32_t optimizedCount = CompactAndFuseCommands(rawCommands, scratchCommands, drawCount);

    if (mappedInput && GLES.glUnmapBuffer) {
        GLES.glUnmapBuffer(GL_DRAW_INDIRECT_BUFFER);
    }

    if (__builtin_expect(optimizedCount == 0, 0)) {
        if (prevIndirect != 0) {
            GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prevIndirect);
        }
        return;
    }

    // Sincronização do Ring Buffer
    uint32_t slot = ringBuffer.currentFrameIndex;
    SynchronizeSlot(slot);
    size_t slotOffset = slot * ringBuffer.slotSize;
    size_t payloadBytes = optimizedCount * sizeof(DrawElementsIndirectCommand);

    // Upload Zero-Stall
    if (hasBufferStorage && ringBuffer.mappedPtr) {
        memcpy(ringBuffer.mappedPtr + slotOffset, scratchCommands, payloadBytes);
    } else {
        GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, ringBuffer.bufferId);
        GLES.glBufferSubData(GL_DRAW_INDIRECT_BUFFER, slotOffset, payloadBytes, scratchCommands);
    }

    // Execução no Backend
    ExecuteBackend(mode, type, slotOffset, optimizedCount);

    // Inserção da Fence para o Frame N
    if (GLES.glFenceSync) {
        ringBuffer.fences[slot] = GLES.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
    ringBuffer.currentFrameIndex = (ringBuffer.currentFrameIndex + 1) % MG_VMDI_FRAMES_IN_FLIGHT;

    // Restaura buffer de indirect anterior
    GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prevIndirect);

    auto endTime = std::chrono::high_resolution_clock::now();
    double elapsedUs = std::chrono::duration<double, std::micro>(endTime - startTime).count();
    UpdateAutotuner(elapsedUs, optimizedCount);
}

void MG_VMDI_Engine::ExecuteBackend(GLenum mode, GLenum type, size_t slotOffset, uint32_t count) {
    switch (currentTier) {
        case BackendTier::NATIVE_MDI: {
            if (GLES.glMultiDrawElementsIndirectEXT) {
                GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, ringBuffer.bufferId);
                GLES.glMultiDrawElementsIndirectEXT(
                    mode, type, 
                    reinterpret_cast<const void*>(slotOffset), 
                    count, sizeof(DrawElementsIndirectCommand)
                );
                GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
                break;
            }
            currentTier = hasBaseVertex ? BackendTier::MULTI_BASE_VERTEX : BackendTier::INDIRECT_UNROLLED;
            [[fallthrough]];
        }

        case BackendTier::MULTI_BASE_VERTEX: {
            if (GLES.glDrawElementsBaseVertex) {
                GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
                const uintptr_t indexStride = (type == GL_UNSIGNED_INT) ? 4 : (type == GL_UNSIGNED_SHORT ? 2 : 1);

                for (uint32_t i = 0; i < count; ++i) {
                    const auto& cmd = scratchCommands[i];
                    GLES.glDrawElementsBaseVertex(
                        mode,
                        cmd.count,
                        type,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(cmd.firstIndex) * indexStride),
                        cmd.baseVertex
                    );
                }
                break;
            }
            currentTier = BackendTier::INDIRECT_UNROLLED;
            [[fallthrough]];
        }

        case BackendTier::INDIRECT_UNROLLED: {
            if (GLES.glDrawElementsIndirect) {
                GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, ringBuffer.bufferId);
                for (uint32_t i = 0; i < count; ++i) {
                    size_t cmdOffset = slotOffset + (i * sizeof(DrawElementsIndirectCommand));
                    GLES.glDrawElementsIndirect(mode, type, reinterpret_cast<const void*>(cmdOffset));
                }
                GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
                break;
            }
            currentTier = BackendTier::DIRECT_FALLBACK;
            [[fallthrough]];
        }

        case BackendTier::DIRECT_FALLBACK: {
            GLES.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
            const uintptr_t indexStride = (type == GL_UNSIGNED_INT) ? 4 : (type == GL_UNSIGNED_SHORT ? 2 : 1);

            for (uint32_t i = 0; i < count; ++i) {
                const auto& cmd = scratchCommands[i];
                GLES.glDrawElements(
                    mode,
                    cmd.count,
                    type,
                    reinterpret_cast<void*>(static_cast<uintptr_t>(cmd.firstIndex) * indexStride)
                );
            }
            break;
        }
    }
}

void MG_VMDI_Engine::SetTierForced(BackendTier t) {
    currentTier = t;
}

void MG_VMDI_Engine::UpdateAutotuner(double frameTimeUs, uint32_t drawCount) {
    if (g_ForcedTier >= 0) return;

    uint32_t tierIdx = static_cast<uint32_t>(currentTier);
    if (tierIdx < 4) {
        tuner.timePerBackend[tierIdx] += frameTimeUs;
        tuner.sampleCount[tierIdx]++;
    }
    tuner.callsSinceLastEvaluation++;

    if (tuner.callsSinceLastEvaluation >= 200) {
        tuner.callsSinceLastEvaluation = 0;

        if (currentTier != BackendTier::NATIVE_MDI) {
            double avgBaseVertex = (tuner.sampleCount[1] > 0) ? 
                (tuner.timePerBackend[1] / tuner.sampleCount[1]) : 999999.0;
            double avgIndirect = (tuner.sampleCount[2] > 0) ? 
                (tuner.timePerBackend[2] / tuner.sampleCount[2]) : 999999.0;

            if (drawCount > 96) {
                currentTier = (avgIndirect <= avgBaseVertex) ? 
                    BackendTier::INDIRECT_UNROLLED : BackendTier::MULTI_BASE_VERTEX;
            } else {
                currentTier = BackendTier::MULTI_BASE_VERTEX;
            }
        }
    }
}
