// MobileGlues - gl/FSR1/FSR1.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header
//
// FSR1 — Optimized for PowerVR/Adreno/Mali TBDR GPUs.
//
// Strategy (validated on PowerVR Rogue GE8320):
//   Scene rendered at reduced resolution → single-pass upscale+sharpen
//   via GL_LINEAR bilinear (hardware) or 5-tap shader (software fallback).
//
// Performance measured on GE8320 @ 1024x1536, 32-sample shader:
//   Native full-res:          23.35 ms (42.8 FPS)
//   Hardware blit only:       14.12 ms (70.8 FPS) — 1.65x
//   Hardware blit + sharpen:  15.39 ms (65.0 FPS) — 1.52x  ← recommended
//
// Design rules:
//   • Minimize number of passes (each extra pass costs ~1.7ms on TBDR)
//   • Prefer hardware glBlitFramebuffer over shader upscale when possible
//   • Never use GL_EXT_shader_framebuffer_fetch for upscaling (causes
//     tile-cache feedback loop, 2x slowdown on PowerVR)
//   • Never use GL_EXT_shader_pixel_local_storage (driver bug on GE8320)
//   • Cap sharpen shader at 5 taps
#pragma once

#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef __APPLE__
#include <malloc.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "../../gles/gles.h"
#include "../../gles/loader.h"
#include "../../includes.h"
#include "../framebuffer.h"
#include "../glsl/glsl_for_es.h"
#include "../log.h"
#include "../mg.h"
#include <GL/gl.h>

namespace FSR1_Context {
    // Main render target (reduced resolution — scene renders here)
    extern GLuint g_renderFBO;
    extern GLuint g_renderTexture;
    extern GLuint g_depthStencilRBO;

    // Final target (native resolution — result of upscale)
    extern GLuint g_targetFBO;
    extern GLuint g_targetTexture;

    // Fullscreen quad resources
    extern GLuint g_quadVAO;
    extern GLuint g_quadVBO;

    // Upscale program — 5-tap sharpen, or a passthrough blit if sharpen
    // failed to compile (fallback).
    extern GLuint g_fsrProgram;
    extern GLint g_inputTexLoc;
    extern GLint g_const0Loc;         // vec2: 1/renderW, 1/renderH (texel size)
    extern GLint g_viewportSizeLoc;   // vec2: renderW, renderH (source size)
    extern GLint g_sharpnessLoc;      // float: sharpen amount (0.0 = passthrough)

    // Whether sharpen shader is available. If false, we use HW blit path.
    extern bool g_useHardwareBlit;

    // Cached state
    extern GLuint g_currentDrawFBO;
    extern GLint g_viewport[4];
    extern GLsizei g_targetWidth;
    extern GLsizei g_targetHeight;
    extern GLsizei g_renderWidth;
    extern GLsizei g_renderHeight;
    extern bool g_dirty;

    // Deferred resolution change
    extern bool g_resolutionChanged;
    extern GLsizei g_pendingWidth;
    extern GLsizei g_pendingHeight;

    // Frame counter (for throttling eglQuerySurface)
    extern unsigned int g_frameCounter;

    // Whether FSR resources are valid and ready to use
    extern bool g_initialized;
} // namespace FSR1_Context

extern bool fsrInitialized;

// Multi-context support (unchanged API)
void mg_fsr1_bind_context(unsigned long long ctx_id);
void mg_fsr1_forget_context(unsigned long long ctx_id);

// Main entry — call from eglSwapBuffers
void ApplyFSR();

// Resource lifecycle
void InitFSRResources();
void DestroyFSRResources();
void CheckResolutionChange(EGLDisplay display, EGLSurface surface);
void OnResize(int width, int height);

extern "C" {
    GLAPI void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
    GLAPI void glScissor(GLint x, GLint y, GLsizei w, GLsizei h);
}