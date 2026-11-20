/**
 * @file        graphics/vulkan/swap_fxaa_shader.h
 * @brief       The swap's FXAA compute shader (GLSL): Vulkan backend and native renderer
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

namespace rex::graphics::vulkan {

// swap_post_effect fxaa / fxaa_extreme, compiled to SPIR-V at run time
// (CompileGlslToSpirv). Set 0: the gamma-corrected image with the luma in .a
// (the apply_gamma_*_fxaa_luma shaders write it), a combined image sampler
// (linear, clamp); set 1: the destination, an rgb10_a2 storage image; push
// constants: the size and its inverse. 16x8 invocations a group.
inline const char* GetSwapFxaaComputeSource(bool extreme_quality) {
  return extreme_quality ? R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_fxaa_size;
  vec2 xe_fxaa_size_inv;
};

layout(set = 0, binding = 0) uniform sampler2D xe_fxaa_source;
layout(set = 1, binding = 0, rgb10_a2) writeonly uniform image2D xe_fxaa_dest;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);
const float kEdgeThreshold = 0.063;
const float kEdgeThresholdMin = 0.0312;
const float kSpanMax = 12.0;
const float kDirReduceMul = 0.125;
const float kDirReduceMin = 1.0 / 128.0;

float SampleLuma(vec2 uv) {
  return textureLod(xe_fxaa_source, uv, 0.0).a;
}

void main() {
  uvec2 pixel = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pixel, xe_fxaa_size))) {
    return;
  }

  vec2 uv = (vec2(pixel) + vec2(0.5)) * xe_fxaa_size_inv;
  vec2 texel = xe_fxaa_size_inv;
  vec4 rgbm = textureLod(xe_fxaa_source, uv, 0.0);

  float lumaM = rgbm.a;
  float lumaNW = SampleLuma(uv + vec2(-texel.x, -texel.y));
  float lumaNE = SampleLuma(uv + vec2( texel.x, -texel.y));
  float lumaSW = SampleLuma(uv + vec2(-texel.x,  texel.y));
  float lumaSE = SampleLuma(uv + vec2( texel.x,  texel.y));

  float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
  float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
  float lumaRange = lumaMax - lumaMin;

  vec3 result = rgbm.rgb;
  if (lumaRange >= max(kEdgeThresholdMin, lumaMax * kEdgeThreshold)) {
    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max(
        (lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * kDirReduceMul),
        kDirReduceMin);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-kSpanMax), vec2(kSpanMax)) * texel;

    vec3 rgbA =
        0.5 *
        (textureLod(xe_fxaa_source, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
         textureLod(xe_fxaa_source, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    vec3 rgbB =
        rgbA * 0.5 +
        0.25 *
            (textureLod(xe_fxaa_source, uv + dir * -0.5, 0.0).rgb +
             textureLod(xe_fxaa_source, uv + dir * 0.5, 0.0).rgb);
    float lumaB = dot(rgbB, kLumaWeights);
    result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
  }

  imageStore(xe_fxaa_dest, ivec2(pixel), vec4(result, 1.0));
}
)"
                         : R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_fxaa_size;
  vec2 xe_fxaa_size_inv;
};

layout(set = 0, binding = 0) uniform sampler2D xe_fxaa_source;
layout(set = 1, binding = 0, rgb10_a2) writeonly uniform image2D xe_fxaa_dest;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);
const float kEdgeThreshold = 0.166;
const float kEdgeThresholdMin = 0.0833;
const float kSpanMax = 8.0;
const float kDirReduceMul = 0.125;
const float kDirReduceMin = 1.0 / 128.0;

float SampleLuma(vec2 uv) {
  return textureLod(xe_fxaa_source, uv, 0.0).a;
}

void main() {
  uvec2 pixel = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pixel, xe_fxaa_size))) {
    return;
  }

  vec2 uv = (vec2(pixel) + vec2(0.5)) * xe_fxaa_size_inv;
  vec2 texel = xe_fxaa_size_inv;
  vec4 rgbm = textureLod(xe_fxaa_source, uv, 0.0);

  float lumaM = rgbm.a;
  float lumaNW = SampleLuma(uv + vec2(-texel.x, -texel.y));
  float lumaNE = SampleLuma(uv + vec2( texel.x, -texel.y));
  float lumaSW = SampleLuma(uv + vec2(-texel.x,  texel.y));
  float lumaSE = SampleLuma(uv + vec2( texel.x,  texel.y));

  float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
  float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
  float lumaRange = lumaMax - lumaMin;

  vec3 result = rgbm.rgb;
  if (lumaRange >= max(kEdgeThresholdMin, lumaMax * kEdgeThreshold)) {
    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max(
        (lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * kDirReduceMul),
        kDirReduceMin);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-kSpanMax), vec2(kSpanMax)) * texel;

    vec3 rgbA =
        0.5 *
        (textureLod(xe_fxaa_source, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
         textureLod(xe_fxaa_source, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    vec3 rgbB =
        rgbA * 0.5 +
        0.25 *
            (textureLod(xe_fxaa_source, uv + dir * -0.5, 0.0).rgb +
             textureLod(xe_fxaa_source, uv + dir * 0.5, 0.0).rgb);
    float lumaB = dot(rgbB, kLumaWeights);
    result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
  }

  imageStore(xe_fxaa_dest, ivec2(pixel), vec4(result, 1.0));
}
)";
}

}  // namespace rex::graphics::vulkan
