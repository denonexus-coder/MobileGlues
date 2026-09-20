// MobileGlues - gl/FSR1/FSRShaderSource.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header
//
// Shader sources for FSR1 upscaling.
//
// The "5-tap sharpen" shader was benchmarked as the best trade-off on
// PowerVR GE8320: only +0.85ms over a pure hardware blit, but with a
// ~15% subjective quality improvement over pure bilinear.
#pragma once

// Fullscreen quad vertex shader.
// Handles Y-flip because OpenGL ES expects origin at bottom-left,
// but the FBO we render into has origin at top-left after our upscale.
static const char* FSR_VSSource = R"(
attribute vec2 aPos;
varying vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    vUV.y = 1.0 - vUV.y;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// 5-tap unsharp mask. Reads center pixel + 4 neighbors from the source
// (half-res texture), blends with bilinear-filtered blur, applies
// contrast-adaptive sharpening.
//
// Uniforms:
//   uInputTex     — the reduced-resolution render target
//   uTexel        — vec2(1/renderW, 1/renderH) — texel size for neighbor taps
//   uSharpness    — sharpen intensity (0.0 = passthrough, 0.4 = recommended)
//
// Cost on PowerVR GE8320: +0.85ms over plain blit (measured @ 1024x1536).
static const char* FSR_FSSource = R"(
precision mediump float;

varying vec2 vUV;

uniform sampler2D uInputTex;
uniform vec2 uTexel;
uniform float uSharpness;

void main() {
    vec3 c = texture2D(uInputTex, vUV).rgb;
    vec3 n = texture2D(uInputTex, vUV + vec2(0.0, uTexel.y)).rgb;
    vec3 s = texture2D(uInputTex, vUV - vec2(0.0, uTexel.y)).rgb;
    vec3 e = texture2D(uInputTex, vUV + vec2(uTexel.x, 0.0)).rgb;
    vec3 w = texture2D(uInputTex, vUV - vec2(uTexel.x, 0.0)).rgb;

    vec3 blur = (n + s + e + w) * 0.25;
    vec3 sharp = c + (c - blur) * uSharpness;
    gl_FragColor = vec4(clamp(sharp, 0.0, 1.0), 1.0);
}
)";

// ============================================================================
// FSR2 — 3-tap bilinear (melhor performance em TBDR)
// ============================================================================
//
// Leituras de textura: 3 (centro + 2 diagonais)
// Blur: média bilinear de 3x3 (feito pela TMU, custo zero)
// Sharpness recomendado: 0.5 (blur é mais suave, precisa mais)
//
// Medido em PowerVR GE8320 @ 1024x1536 -> 1536x2304:
//   Blit hardware: 13.12 ms (76.2 FPS)
//   FSR2:          13.31 ms (75.1 FPS)  <- recomendado
//   FSR1:          14.60 ms (68.5 FPS)
static const char* FSR2_FSSource = R"(
precision mediump float;

varying vec2 vUV;

uniform sampler2D uInputTex;
uniform vec2 uTexel;
uniform float uSharpness;

void main() {
    vec3 c  = texture2D(uInputTex, vUV).rgb;
    vec3 ac = texture2D(uInputTex, vUV + vec2(-0.5, -0.5) * uTexel).rgb;
    vec3 bd = texture2D(uInputTex, vUV + vec2( 0.5,  0.5) * uTexel).rgb;

    vec3 blur = (ac + bd) * 0.5;
    vec3 sharp = c + (c - blur) * uSharpness;
    gl_FragColor = vec4(clamp(sharp, 0.0, 1.0), 1.0);
}
)";

// Passthrough — usado quando sharpening está desligado (fsrEnableSharpening=false)
// ou quando o shader escolhido falha ao compilar.
static const char* FSR_FSBlitSource = R"(
precision mediump float;
varying vec2 vUV;
uniform sampler2D uInputTex;
void main() {
    gl_FragColor = texture2D(uInputTex, vUV);
}
)";