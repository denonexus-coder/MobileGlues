// MobileGlues - gl/mg_vmdi.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#pragma once

#include <GLES3/gl32.h>
#include <cstdint>
#include <cstddef>
#include "mg_vmdi_config.h"

#define MG_VMDI_MAX_DRAWS 4096
#define MG_VMDI_FRAMES_IN_FLIGHT 3

#pragma pack(push, 1)
struct DrawElementsIndirectCommand {
    uint32_t count;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t  baseVertex;
    uint32_t baseInstance;
};
#pragma pack(pop)

enum class BackendTier : uint32_t {
    NATIVE_MDI = 0,
    MULTI_BASE_VERTEX = 1,
    INDIRECT_UNROLLED = 2,
    DIRECT_FALLBACK = 3
};

struct MG_RingBuffer {
    GLuint bufferId;
    uint8_t* mappedPtr;
    GLsync fences[MG_VMDI_FRAMES_IN_FLIGHT];
    size_t slotSize;
    uint32_t currentFrameIndex;
};

class MG_VMDI_Engine {
private:
    BackendTier currentTier;
    MG_RingBuffer ringBuffer;
    bool hasBufferStorage;
    bool hasBaseVertex;
    bool initialized;

    struct alignas(64) TuningMetrics {
        double timePerBackend[4];
        uint32_t sampleCount[4];
        uint32_t callsSinceLastEvaluation;
    } tuner;

    alignas(64) DrawElementsIndirectCommand scratchCommands[MG_VMDI_MAX_DRAWS];

    void InitRingBuffer();
    void SynchronizeSlot(uint32_t slot);
    void ExecuteBackend(GLenum mode, GLenum type, size_t slotOffset, uint32_t count);
    void UpdateAutotuner(double frameTimeUs, uint32_t drawCount);

public:
    MG_VMDI_Engine();
    ~MG_VMDI_Engine();

    void Init(const char* glExtensions);
    void Destroy();
    
    // Pipeline de Compactação e Fusão Contígua com SIMD
    uint32_t CompactAndFuseCommands(const DrawElementsIndirectCommand* __restrict src, 
                                    DrawElementsIndirectCommand* __restrict dst, 
                                    uint32_t count);

    void Dispatch(GLenum mode, GLenum type, const void* indirect, 
                  uint32_t drawCount, uint32_t stride);

    void SetTierForced(BackendTier t);
};

// Singleton Engine
extern MG_VMDI_Engine g_vmdiEngine;

#ifdef __cplusplus
extern "C" {
#endif

void mg_init_multidraw_subsystem(const char* glExtensions);

// Fase 3A: força um BackendTier específico e desliga o autotuner.
// tier < 0 restaura autotuner (comportamento original).
void mg_vmdi_set_tier(int tier);
int  mg_vmdi_get_tier();

#ifdef __cplusplus
}
#endif
