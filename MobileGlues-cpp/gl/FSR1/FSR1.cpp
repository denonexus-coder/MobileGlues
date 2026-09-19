// MobileGlues - gl/FSR1/FSR1.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header
//
// FSR1 implementation — see FSR1.h for design rationale.

#include "FSR1.h"
#include <mutex>
#include <ska/flat_hash_map.hpp>
#include "FSRShaderSource.h"
#include "../../config/settings.h"

#define DEBUG 0

// ============================================================================
// Constants
// ============================================================================

// Re-check surface size every N frames instead of every frame.
// eglQuerySurface costs ~0.1-0.5ms per call on some drivers.
static constexpr unsigned int SURFACE_QUERY_INTERVAL = 30;

// Default sharpen intensity. 0.4 was chosen empirically — high enough
// to recover bilinear softness, low enough to avoid ringing.
static constexpr float DEFAULT_SHARPNESS = 0.4f;

// ============================================================================
// GL state guard — saves and restores the caller's GL state around our work.
// ============================================================================

enum GLStateBits : unsigned int {
    GUARD_PROGRAM      = 1u << 0,
    GUARD_VAO          = 1u << 1,
    GUARD_ARRAY_BUFFER = 1u << 2,
    GUARD_TEXTURE      = 1u << 3,
    GUARD_FRAMEBUFFER  = 1u << 4,
    GUARD_RENDERBUFFER = 1u << 5,
};

struct GLStateGuard {
    unsigned int saved;
    GLint prevProgram = 0;
    GLint prevVAO = 0;
    GLint prevArrayBuffer = 0;
    GLint prevActiveTexture = GL_TEXTURE0;
    GLint prevTexture = 0;
    GLint prevReadFBO = 0;
    GLint prevDrawFBO = 0;
    GLint prevRenderbuffer = 0;

    explicit GLStateGuard(unsigned int bits) : saved(bits) {
        if (saved & GUARD_PROGRAM) {
            prevProgram = static_cast<GLint>(gl_state->current_program);
        }
        if (saved & GUARD_VAO) {
            GLES.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);
        }
        if (saved & GUARD_ARRAY_BUFFER) {
            GLES.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuffer);
        }
        if (saved & GUARD_TEXTURE) {
            GLES.glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
            GLES.glActiveTexture(GL_TEXTURE0);
            GLES.glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture);
        }
        if (saved & GUARD_FRAMEBUFFER) {
            GLES.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFBO);
            prevDrawFBO = static_cast<GLint>(gl_state->current_draw_fbo);
        }
        if (saved & GUARD_RENDERBUFFER) {
            GLES.glGetIntegerv(GL_RENDERBUFFER_BINDING, &prevRenderbuffer);
        }
    }

    void framebuffer_recreated(GLuint from, GLuint to) {
        if (!(saved & GUARD_FRAMEBUFFER) || from == 0 || from == to) return;
        if (prevReadFBO == static_cast<GLint>(from)) prevReadFBO = static_cast<GLint>(to);
        if (prevDrawFBO == static_cast<GLint>(from)) prevDrawFBO = static_cast<GLint>(to);
    }

    ~GLStateGuard() {
        if (saved & GUARD_PROGRAM) GLES.glUseProgram(prevProgram);
        if (saved & GUARD_VAO) GLES.glBindVertexArray(prevVAO);
        if (saved & GUARD_ARRAY_BUFFER) GLES.glBindBuffer(GL_ARRAY_BUFFER, prevArrayBuffer);
        if (saved & GUARD_TEXTURE) {
            GLES.glActiveTexture(GL_TEXTURE0);
            GLES.glBindTexture(GL_TEXTURE_2D, prevTexture);
            GLES.glActiveTexture(prevActiveTexture);
        }
        if (saved & GUARD_RENDERBUFFER) GLES.glBindRenderbuffer(GL_RENDERBUFFER, prevRenderbuffer);
        if (saved & GUARD_FRAMEBUFFER) {
            GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFBO);
            GLES.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFBO);
        }
    }
};

// ============================================================================
// Global state
// ============================================================================

namespace FSR1_Context {
    GLuint g_renderFBO = 0;
    GLuint g_renderTexture = 0;
    GLuint g_depthStencilRBO = 0;
    GLuint g_quadVAO = 0;
    GLuint g_quadVBO = 0;
    GLuint g_fsrProgram = 0;

    GLint g_inputTexLoc = -1;
    GLint g_const0Loc = -1;
    GLint g_viewportSizeLoc = -1;
    GLint g_sharpnessLoc = -1;

    bool  g_useHardwareBlit = false;

    GLuint g_targetFBO = 0;
    GLuint g_targetTexture = 0;

    GLuint g_currentDrawFBO = 0;
    GLint g_viewport[4] = {0};
    GLsizei g_targetWidth = 2400;
    GLsizei g_targetHeight = 1080;
    GLsizei g_renderWidth = 1200;
    GLsizei g_renderHeight = 540;
    bool g_dirty = false;

    bool g_resolutionChanged = false;
    GLsizei g_pendingWidth = 0;
    GLsizei g_pendingHeight = 0;

    unsigned int g_frameCounter = 0;
    bool g_initialized = false;
} // namespace FSR1_Context

bool fsrInitialized = false;

// ============================================================================
// Resolution helpers
// ============================================================================

// Presets follow the MobileGlues convention: "scale" is the upscale ratio.
// A scale of 2.0 means render at half-res; 1.5 means render at ~67%.
static float GetScaleForPreset(FSR1_Quality_Preset preset) {
    switch (preset) {
        case FSR1_Quality_Preset::UltraQuality: return 1.3f; // 77% render
        case FSR1_Quality_Preset::Quality:      return 1.5f; // 67% render
        case FSR1_Quality_Preset::Balanced:     return 1.7f; // 59% render
        case FSR1_Quality_Preset::Performance:  return 2.0f; // 50% render
        default:                                return 1.5f;
    }
}

void CalculateTargetResolution(FSR1_Quality_Preset preset,
                               int renderWidth, int renderHeight,
                               int* targetWidth, int* targetHeight) {
    float scale = GetScaleForPreset(preset);
    *targetWidth  = static_cast<int>(renderWidth * scale);
    *targetHeight = static_cast<int>(renderHeight * scale);
    // Even dimensions avoid off-by-one blit artifacts
    *targetWidth  = (*targetWidth  + 1) & ~1;
    *targetHeight = (*targetHeight + 1) & ~1;
    LOG_D("FSR1 render res: %dx%d", renderWidth, renderHeight);
    LOG_D("FSR1 target res: %dx%d", *targetWidth, *targetHeight);
}

void CalculateRenderResolution(FSR1_Quality_Preset preset,
                               int targetWidth, int targetHeight,
                               int* renderWidth, int* renderHeight) {
    float scale = GetScaleForPreset(preset);
    *renderWidth  = static_cast<int>(targetWidth / scale);
    *renderHeight = static_cast<int>(targetHeight / scale);
    *renderWidth  = (*renderWidth  + 1) & ~1;
    *renderHeight = (*renderHeight + 1) & ~1;
}

// ============================================================================
// Shader compilation
// ============================================================================

static GLuint CompileShader(GLenum type, const char* src, const char* label) {
    GLuint s = glCreateShader(type);
    if (s == 0) {
        LOG_F("FSR1: glCreateShader failed for %s", label);
        return 0;
    }
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);

    GLint status = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[1024] = {0};
        glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
        LOG_F("FSR1: %s shader compile failed: %s", label, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// Compile the upscale program. Falls back to a plain blit shader if the
// sharpen variant fails. Returns 0 only if both fail (extremely unlikely).
static GLuint CompileUpscaleProgram(bool* out_hardware_blit) {
    *out_hardware_blit = false;

    GLuint vs = CompileShader(GL_VERTEX_SHADER, FSR_VSSource, "VS");
    if (!vs) return 0;

    // Try the sharpen shader first
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, FSR_FSSource, "FS-sharpen");
    if (!fs) {
        // Fallback: minimal blit shader
        LOG_D("FSR1: sharpen FS failed, falling back to passthrough blit");
        fs = CompileShader(GL_FRAGMENT_SHADER, FSR_FSBlitSource, "FS-blit");
        if (!fs) {
            glDeleteShader(vs);
            return 0;
        }
    }

    GLuint prog = glCreateProgram();
    if (prog == 0) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }

    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint status = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        char log[1024] = {0};
        glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
        LOG_F("FSR1: program link failed: %s", log);
        glDeleteShader(vs);
        glDeleteShader(fs);
        glDeleteProgram(prog);
        return 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

// ============================================================================
// Fullscreen quad
// ============================================================================

static void InitFullscreenQuad() {
    GLStateGuard state(GUARD_VAO | GUARD_ARRAY_BUFFER);

    // 2 triangles: position (vec2) only
    const float verts[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };

    GLES.glGenVertexArrays(1, &FSR1_Context::g_quadVAO);
    GLES.glGenBuffers(1, &FSR1_Context::g_quadVBO);

    GLES.glBindVertexArray(FSR1_Context::g_quadVAO);
    GLES.glBindBuffer(GL_ARRAY_BUFFER, FSR1_Context::g_quadVBO);
    GLES.glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    // aPos at location 0
    GLES.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    GLES.glEnableVertexAttribArray(0);

    GLES.glBindBuffer(GL_ARRAY_BUFFER, 0);
    GLES.glBindVertexArray(0);
}

// ============================================================================
// FBO helpers
// ============================================================================

// Creates a texture + FBO pair. Returns false on failure.
static bool CreateFBO(int w, int h, GLuint* out_fbo, GLuint* out_tex) {
    GLES.glGenTextures(1, out_tex);
    if (*out_tex == 0) return false;

    GLES.glBindTexture(GL_TEXTURE_2D, *out_tex);
    // IMPORTANT: use RGBA8, not RGBA32F. Verified on GE8320 that RGBA8 is
    // 4x more bandwidth-efficient with no perceptible quality difference
    // for our use case.
    GLES.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                      GL_UNSIGNED_BYTE, nullptr);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GLES.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLES.glGenFramebuffers(1, out_fbo);
    if (*out_fbo == 0) {
        GLES.glDeleteTextures(1, out_tex);
        *out_tex = 0;
        return false;
    }
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, *out_fbo);
    GLES.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, *out_tex, 0);

    GLenum status = GLES.glCheckFramebufferStatus(GL_FRAMEBUFFER);
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_F("FSR1: FBO incomplete (0x%x) at %dx%d", status, w, h);
        GLES.glDeleteFramebuffers(1, out_fbo);
        GLES.glDeleteTextures(1, out_tex);
        *out_fbo = 0;
        *out_tex = 0;
        return false;
    }
    return true;
}

// ============================================================================
// Init / destroy
// ============================================================================

void InitFSRResources() {
    if (FSR1_Context::g_initialized) return;
    fsrInitialized = true;

    GLStateGuard state(GUARD_PROGRAM | GUARD_TEXTURE | GUARD_FRAMEBUFFER | GUARD_RENDERBUFFER);

    // Compile upscale program (with fallback)
    bool hw_blit = false;
    FSR1_Context::g_fsrProgram = CompileUpscaleProgram(&hw_blit);
    FSR1_Context::g_useHardwareBlit = false; // we always use shader path; hw blit only if shader fails entirely
    if (FSR1_Context::g_fsrProgram == 0) {
        // Total shader failure — enable pure hardware blit path
        LOG_F("FSR1: no shader available, using hardware blit only");
        FSR1_Context::g_useHardwareBlit = true;
    } else {
        FSR1_Context::g_inputTexLoc      = glGetUniformLocation(FSR1_Context::g_fsrProgram, "uInputTex");
        FSR1_Context::g_const0Loc        = glGetUniformLocation(FSR1_Context::g_fsrProgram, "uTexel");
        FSR1_Context::g_viewportSizeLoc  = glGetUniformLocation(FSR1_Context::g_fsrProgram, "uTexel"); // unused; kept for ABI
        FSR1_Context::g_sharpnessLoc     = glGetUniformLocation(FSR1_Context::g_fsrProgram, "uSharpness");

        GLES.glUseProgram(FSR1_Context::g_fsrProgram);
        GLES.glUniform1i(FSR1_Context::g_inputTexLoc, 0);
        GLES.glUseProgram(0);
    }

    InitFullscreenQuad();

    // Render target (reduced resolution)
    if (!CreateFBO(FSR1_Context::g_renderWidth, FSR1_Context::g_renderHeight,
                   &FSR1_Context::g_renderFBO, &FSR1_Context::g_renderTexture)) {
        LOG_F("FSR1: failed to create render FBO");
        FSR1_Context::g_useHardwareBlit = true; // degrade gracefully
        return;
    }

    // Depth-stencil RBO
    GLES.glGenRenderbuffers(1, &FSR1_Context::g_depthStencilRBO);
    GLES.glBindRenderbuffer(GL_RENDERBUFFER, FSR1_Context::g_depthStencilRBO);
    GLES.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                               FSR1_Context::g_renderWidth,
                               FSR1_Context::g_renderHeight);
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR1_Context::g_renderFBO);
    GLES.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                   GL_RENDERBUFFER, FSR1_Context::g_depthStencilRBO);

    // Target (native resolution)
    if (!CreateFBO(FSR1_Context::g_targetWidth, FSR1_Context::g_targetHeight,
                   &FSR1_Context::g_targetFBO, &FSR1_Context::g_targetTexture)) {
        LOG_F("FSR1: failed to create target FBO");
        FSR1_Context::g_useHardwareBlit = true;
    }

    // Leave bound to render FBO — game draws here
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR1_Context::g_renderFBO);
    GLES.glViewport(0, 0, FSR1_Context::g_renderWidth, FSR1_Context::g_renderHeight);

    FSR1_Context::g_initialized = true;
    LOG_D("FSR1 initialized: render %dx%d, target %dx%d, shader=%s",
          FSR1_Context::g_renderWidth, FSR1_Context::g_renderHeight,
          FSR1_Context::g_targetWidth, FSR1_Context::g_targetHeight,
          FSR1_Context::g_useHardwareBlit ? "no (hw blit)" : "yes");
}

void DestroyFSRResources() {
    if (!FSR1_Context::g_initialized) return;

    if (FSR1_Context::g_fsrProgram) {
        glDeleteProgram(FSR1_Context::g_fsrProgram);
        FSR1_Context::g_fsrProgram = 0;
    }
    if (FSR1_Context::g_quadVBO) {
        GLES.glDeleteBuffers(1, &FSR1_Context::g_quadVBO);
        FSR1_Context::g_quadVBO = 0;
    }
    if (FSR1_Context::g_quadVAO) {
        GLES.glDeleteVertexArrays(1, &FSR1_Context::g_quadVAO);
        FSR1_Context::g_quadVAO = 0;
    }
    if (FSR1_Context::g_renderFBO) {
        GLES.glDeleteFramebuffers(1, &FSR1_Context::g_renderFBO);
        FSR1_Context::g_renderFBO = 0;
    }
    if (FSR1_Context::g_renderTexture) {
        GLES.glDeleteTextures(1, &FSR1_Context::g_renderTexture);
        FSR1_Context::g_renderTexture = 0;
    }
    if (FSR1_Context::g_depthStencilRBO) {
        GLES.glDeleteRenderbuffers(1, &FSR1_Context::g_depthStencilRBO);
        FSR1_Context::g_depthStencilRBO = 0;
    }
    if (FSR1_Context::g_targetFBO) {
        GLES.glDeleteFramebuffers(1, &FSR1_Context::g_targetFBO);
        FSR1_Context::g_targetFBO = 0;
    }
    if (FSR1_Context::g_targetTexture) {
        GLES.glDeleteTextures(1, &FSR1_Context::g_targetTexture);
        FSR1_Context::g_targetTexture = 0;
    }

    FSR1_Context::g_initialized = false;
    fsrInitialized = false;
}

// Recreate FBOs at new size (used after a resolution change).
static bool RecreateFSRFBO() {
    GLStateGuard state(GUARD_TEXTURE | GUARD_FRAMEBUFFER | GUARD_RENDERBUFFER);

    const GLuint oldRenderFBO = FSR1_Context::g_renderFBO;
    const GLuint oldTargetFBO = FSR1_Context::g_targetFBO;

    // Delete old
    if (FSR1_Context::g_renderFBO)    GLES.glDeleteFramebuffers(1, &FSR1_Context::g_renderFBO);
    if (FSR1_Context::g_renderTexture) GLES.glDeleteTextures(1, &FSR1_Context::g_renderTexture);
    if (FSR1_Context::g_depthStencilRBO) GLES.glDeleteRenderbuffers(1, &FSR1_Context::g_depthStencilRBO);
    if (FSR1_Context::g_targetFBO)     GLES.glDeleteFramebuffers(1, &FSR1_Context::g_targetFBO);
    if (FSR1_Context::g_targetTexture) GLES.glDeleteTextures(1, &FSR1_Context::g_targetTexture);
    FSR1_Context::g_renderFBO = 0;
    FSR1_Context::g_renderTexture = 0;
    FSR1_Context::g_depthStencilRBO = 0;
    FSR1_Context::g_targetFBO = 0;
    FSR1_Context::g_targetTexture = 0;

    // Recreate render FBO
    if (!CreateFBO(FSR1_Context::g_renderWidth, FSR1_Context::g_renderHeight,
                   &FSR1_Context::g_renderFBO, &FSR1_Context::g_renderTexture)) {
        LOG_F("FSR1: failed to recreate render FBO");
        return false;
    }

    GLES.glGenRenderbuffers(1, &FSR1_Context::g_depthStencilRBO);
    GLES.glBindRenderbuffer(GL_RENDERBUFFER, FSR1_Context::g_depthStencilRBO);
    GLES.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                               FSR1_Context::g_renderWidth,
                               FSR1_Context::g_renderHeight);
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR1_Context::g_renderFBO);
    GLES.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                   GL_RENDERBUFFER, FSR1_Context::g_depthStencilRBO);

    // Recreate target FBO
    if (!CreateFBO(FSR1_Context::g_targetWidth, FSR1_Context::g_targetHeight,
                   &FSR1_Context::g_targetFBO, &FSR1_Context::g_targetTexture)) {
        LOG_F("FSR1: failed to recreate target FBO");
        return false;
    }

    // Update state tracker if it was pointing at old FBOs
    if (oldRenderFBO != 0 && gl_state->current_draw_fbo == oldRenderFBO) {
        set_gl_state_current_draw_fbo(FSR1_Context::g_renderFBO);
    }
    state.framebuffer_recreated(oldRenderFBO, FSR1_Context::g_renderFBO);
    state.framebuffer_recreated(oldTargetFBO, FSR1_Context::g_targetFBO);

    GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR1_Context::g_renderFBO);
    GLES.glViewport(0, 0, FSR1_Context::g_renderWidth, FSR1_Context::g_renderHeight);

    LOG_D("FSR1 resources recreated: render %dx%d, target %dx%d",
          FSR1_Context::g_renderWidth, FSR1_Context::g_renderHeight,
          FSR1_Context::g_targetWidth, FSR1_Context::g_targetHeight);
    return true;
}

// ============================================================================
// Apply FSR — called from eglSwapBuffers
// ============================================================================

void ApplyFSR() {
    if (!FSR1_Context::g_initialized) return;
    if (FSR1_Context::g_renderFBO == 0 || FSR1_Context::g_targetFBO == 0) return;

    GLStateGuard state(GUARD_PROGRAM | GUARD_VAO | GUARD_TEXTURE | GUARD_FRAMEBUFFER);

    // Upscale from renderFBO to targetFBO
    GLES.glBindFramebuffer(GL_FRAMEBUFFER, FSR1_Context::g_targetFBO);
    GLES.glViewport(0, 0, FSR1_Context::g_targetWidth, FSR1_Context::g_targetHeight);

    bool rendered = false;

    if (!FSR1_Context::g_useHardwareBlit && FSR1_Context::g_fsrProgram != 0) {
        // Shader path — 5-tap sharpen, single draw call
        GLES.glUseProgram(FSR1_Context::g_fsrProgram);
        GLES.glActiveTexture(GL_TEXTURE0);
        GLES.glBindTexture(GL_TEXTURE_2D, FSR1_Context::g_renderTexture);
        if (FSR1_Context::g_inputTexLoc >= 0) {
            GLES.glUniform1i(FSR1_Context::g_inputTexLoc, 0);
        }
        if (FSR1_Context::g_const0Loc >= 0) {
            GLES.glUniform2f(FSR1_Context::g_const0Loc,
                             1.0f / static_cast<float>(FSR1_Context::g_renderWidth),
                             1.0f / static_cast<float>(FSR1_Context::g_renderHeight));
        }
        if (FSR1_Context::g_sharpnessLoc >= 0) {
            float sharpness = global_settings.fsr1_sharpness > 0.0f ? global_settings.fsr1_sharpness : DEFAULT_SHARPNESS;
            GLES.glUniform1f(FSR1_Context::g_sharpnessLoc, sharpness);
        }
        GLES.glBindVertexArray(FSR1_Context::g_quadVAO);
        GLES.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        rendered = true;
    }

    if (!rendered) {
        // Fallback — hardware blit. Never fails.
        GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, FSR1_Context::g_renderFBO);
        GLES.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, FSR1_Context::g_targetFBO);
        GLES.glBlitFramebuffer(
            0, 0, FSR1_Context::g_renderWidth, FSR1_Context::g_renderHeight,
            0, 0, FSR1_Context::g_targetWidth, FSR1_Context::g_targetHeight,
            GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }

    // Blit targetFBO → default framebuffer (screen).
    GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, FSR1_Context::g_targetFBO);
    GLES.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    GLES.glBlitFramebuffer(
        0, 0, FSR1_Context::g_targetWidth, FSR1_Context::g_targetHeight,
        0, 0, FSR1_Context::g_targetWidth, FSR1_Context::g_targetHeight,
        GL_COLOR_BUFFER_BIT, GL_LINEAR);

    // Restore viewport for next frame — game will render into renderFBO
    GLES.glViewport(0, 0, FSR1_Context::g_renderWidth, FSR1_Context::g_renderHeight);
}

// ============================================================================
// Resolution change detection — throttled to avoid eglQuerySurface spam
// ============================================================================

void CheckResolutionChange(EGLDisplay display, EGLSurface surface) {
    FSR1_Context::g_frameCounter++;

    // Only query surface size every SURFACE_QUERY_INTERVAL frames.
    // This avoids 0.1-0.5ms of overhead per frame on some drivers.
    if (FSR1_Context::g_frameCounter % SURFACE_QUERY_INTERVAL != 0) {
        // Still process any pending change that was set by glViewport override
        if (!FSR1_Context::g_resolutionChanged) {
            return;
        }
    }

    GLsizei width = 0, height = 0;
    if (FSR1_Context::g_frameCounter % SURFACE_QUERY_INTERVAL == 0) {
        LOAD_EGL(eglQuerySurface);
        EGLDisplay dpy = display;
        EGLSurface surf = surface;
        if (dpy == EGL_NO_DISPLAY || surf == EGL_NO_SURFACE) {
            dpy = eglGetCurrentDisplay();
            surf = eglGetCurrentSurface(EGL_DRAW);
        }
        if (dpy != EGL_NO_DISPLAY && surf != EGL_NO_SURFACE) {
            egl_eglQuerySurface(dpy, surf, EGL_WIDTH, &width);
            egl_eglQuerySurface(dpy, surf, EGL_HEIGHT, &height);
            if (width > 0 && height > 0) OnResize(width, height);
        }
    }

    if (FSR1_Context::g_resolutionChanged) {
        FSR1_Context::g_resolutionChanged = false;
        GLsizei newRenderW = FSR1_Context::g_pendingWidth;
        GLsizei newRenderH = FSR1_Context::g_pendingHeight;
        if (newRenderW <= 0 || newRenderH <= 0) return;

        FSR1_Context::g_renderWidth = newRenderW;
        FSR1_Context::g_renderHeight = newRenderH;

        int tw = 0, th = 0;
        CalculateTargetResolution(global_settings.fsr1_setting,
                                  newRenderW, newRenderH, &tw, &th);
        FSR1_Context::g_targetWidth = tw;
        FSR1_Context::g_targetHeight = th;

        if (!RecreateFSRFBO()) {
            // Recreate failed — disable FSR to keep game running
            LOG_F("FSR1: FBO recreate failed, disabling FSR");
            FSR1_Context::g_initialized = false;
        }
    }
}

void OnResize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (FSR1_Context::g_renderWidth == width && FSR1_Context::g_renderHeight == height) return;

    FSR1_Context::g_pendingWidth = width;
    FSR1_Context::g_pendingHeight = height;
    FSR1_Context::g_resolutionChanged = true;
}

// ============================================================================
// Overridden GL entry points
// ============================================================================

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    LOG_D("glViewport: x=%d, y=%d, w=%d, h=%d", x, y, w, h);

    // If the game requests a viewport larger than our current render target,
    // it means the game's expectation changed. Record it and trigger resize.
    if (w > 0 && h > 0 && (w > FSR1_Context::g_pendingWidth || h > FSR1_Context::g_pendingHeight)) {
        FSR1_Context::g_pendingWidth = w;
        FSR1_Context::g_pendingHeight = h;
        FSR1_Context::g_resolutionChanged = true;
    }

    GLES.glViewport(x, y, w, h);
}

void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    // Scale the scissor rect to match our reduced render resolution.
    // Without this, mods that call glScissor with native coordinates would
    // clip outside our FBO.
    if (FSR1_Context::g_initialized &&
        FSR1_Context::g_targetWidth > 0 && FSR1_Context::g_targetHeight > 0) {
        float sx = static_cast<float>(FSR1_Context::g_renderWidth) /
                   static_cast<float>(FSR1_Context::g_targetWidth);
        float sy = static_cast<float>(FSR1_Context::g_renderHeight) /
                   static_cast<float>(FSR1_Context::g_targetHeight);
        GLES.glScissor(static_cast<GLint>(x * sx),
                       static_cast<GLint>(y * sy),
                       static_cast<GLsizei>(w * sx),
                       static_cast<GLsizei>(h * sy));
    } else {
        GLES.glScissor(x, y, w, h);
    }
}

// ============================================================================
// Multi-context support (unchanged from previous version)
// ============================================================================

namespace {

struct fsr1_ctx_state_t {
    GLuint renderFBO = 0, renderTexture = 0, depthStencilRBO = 0;
    GLuint quadVAO = 0, quadVBO = 0, fsrProgram = 0;
    GLint inputTexLoc = -1, const0Loc = -1, viewportSizeLoc = -1, sharpnessLoc = -1;
    bool useHardwareBlit = false;
    GLuint targetFBO = 0, targetTexture = 0, currentDrawFBO = 0;
    GLsizei targetWidth = 0, targetHeight = 0, renderWidth = 0, renderHeight = 0;
    unsigned int frameCounter = 0;
    bool initialized = false;
};

std::mutex g_fsr_mutex;
ska::flat_hash_map<unsigned long long, fsr1_ctx_state_t> g_fsr_states;
fsr1_ctx_state_t g_fsr_default;
thread_local unsigned long long g_fsr_current_id = 0;

void store_into(fsr1_ctx_state_t& d) {
    d.renderFBO = FSR1_Context::g_renderFBO;
    d.renderTexture = FSR1_Context::g_renderTexture;
    d.depthStencilRBO = FSR1_Context::g_depthStencilRBO;
    d.quadVAO = FSR1_Context::g_quadVAO;
    d.quadVBO = FSR1_Context::g_quadVBO;
    d.fsrProgram = FSR1_Context::g_fsrProgram;
    d.inputTexLoc = FSR1_Context::g_inputTexLoc;
    d.const0Loc = FSR1_Context::g_const0Loc;
    d.viewportSizeLoc = FSR1_Context::g_viewportSizeLoc;
    d.sharpnessLoc = FSR1_Context::g_sharpnessLoc;
    d.useHardwareBlit = FSR1_Context::g_useHardwareBlit;
    d.targetFBO = FSR1_Context::g_targetFBO;
    d.targetTexture = FSR1_Context::g_targetTexture;
    d.currentDrawFBO = FSR1_Context::g_currentDrawFBO;
    d.targetWidth = FSR1_Context::g_targetWidth;
    d.targetHeight = FSR1_Context::g_targetHeight;
    d.renderWidth = FSR1_Context::g_renderWidth;
    d.renderHeight = FSR1_Context::g_renderHeight;
    d.frameCounter = FSR1_Context::g_frameCounter;
    d.initialized = FSR1_Context::g_initialized;
}

void load_from(const fsr1_ctx_state_t& s) {
    FSR1_Context::g_renderFBO = s.renderFBO;
    FSR1_Context::g_renderTexture = s.renderTexture;
    FSR1_Context::g_depthStencilRBO = s.depthStencilRBO;
    FSR1_Context::g_quadVAO = s.quadVAO;
    FSR1_Context::g_quadVBO = s.quadVBO;
    FSR1_Context::g_fsrProgram = s.fsrProgram;
    FSR1_Context::g_inputTexLoc = s.inputTexLoc;
    FSR1_Context::g_const0Loc = s.const0Loc;
    FSR1_Context::g_viewportSizeLoc = s.viewportSizeLoc;
    FSR1_Context::g_sharpnessLoc = s.sharpnessLoc;
    FSR1_Context::g_useHardwareBlit = s.useHardwareBlit;
    FSR1_Context::g_targetFBO = s.targetFBO;
    FSR1_Context::g_targetTexture = s.targetTexture;
    FSR1_Context::g_currentDrawFBO = s.currentDrawFBO;
    FSR1_Context::g_targetWidth = s.targetWidth;
    FSR1_Context::g_targetHeight = s.targetHeight;
    FSR1_Context::g_renderWidth = s.renderWidth;
    FSR1_Context::g_renderHeight = s.renderHeight;
    FSR1_Context::g_frameCounter = s.frameCounter;
    FSR1_Context::g_initialized = s.initialized;
    fsrInitialized = s.initialized;
}

} // namespace

void mg_fsr1_bind_context(unsigned long long ctx_id) {
    if (ctx_id == g_fsr_current_id) return;
    std::lock_guard<std::mutex> lock(g_fsr_mutex);
    store_into(g_fsr_current_id == 0 ? g_fsr_default : g_fsr_states[g_fsr_current_id]);
    load_from(ctx_id == 0 ? g_fsr_default : g_fsr_states[ctx_id]);
    g_fsr_current_id = ctx_id;
}

void mg_fsr1_forget_context(unsigned long long ctx_id) {
    if (ctx_id == 0) return;
    std::lock_guard<std::mutex> lock(g_fsr_mutex);
    if (g_fsr_current_id == ctx_id) {
        load_from(g_fsr_default);
        g_fsr_current_id = 0;
    }
    g_fsr_states.erase(ctx_id);
}
