// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#version 450

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

layout (set = 0, binding = 0) uniform texture2D r_current_back_buffer;
layout (set = 0, binding = 1) uniform texture2D r_history_color;
layout (set = 0, binding = 2) uniform texture2D r_optical_flow;
layout (set = 0, binding = 3) uniform texture2D r_dilated_depth;
layout (set = 0, binding = 4) uniform texture2D r_dilated_motion_vectors;
layout (set = 0, binding = 5) uniform texture2D r_reconstructed_prev_nearest_depth;

layout (set = 0, binding = 6, rgba16f) uniform image2D rw_interpolated_output;

layout (set = 0, binding = 7, std140) uniform cbFrameInterpolation_t {
    ivec2 iDisplaySize;
    vec2  fInvDisplaySize;
    float fInterpolationTime;
    uint  uReset;
    float fDepthThreshold;
    float fMotionScale;
} cbFrameInterpolation;

layout (set = 0, binding = 1000) uniform sampler s_LinearClamp;
layout (set = 0, binding = 1001) uniform sampler s_PointClamp;

#ifndef FFX_FRAMEINTERPOLATION_THREAD_GROUP_WIDTH
#define FFX_FRAMEINTERPOLATION_THREAD_GROUP_WIDTH 8
#endif
#ifndef FFX_FRAMEINTERPOLATION_THREAD_GROUP_HEIGHT
#define FFX_FRAMEINTERPOLATION_THREAD_GROUP_HEIGHT 8
#endif

layout (local_size_x = FFX_FRAMEINTERPOLATION_THREAD_GROUP_WIDTH,
        local_size_y = FFX_FRAMEINTERPOLATION_THREAD_GROUP_HEIGHT,
        local_size_z = 1) in;

void main()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    if (pos.x >= cbFrameInterpolation.iDisplaySize.x || pos.y >= cbFrameInterpolation.iDisplaySize.y) {
        return;
    }

    // Reset frame or camera jump: pass current back buffer through directly
    if (cbFrameInterpolation.uReset != 0) {
        vec4 currColor = texelFetch(r_current_back_buffer, pos, 0);
        imageStore(rw_interpolated_output, pos, currColor);
        return;
    }

    vec2 uv = (vec2(pos) + 0.5) * cbFrameInterpolation.fInvDisplaySize;

    // 1. Fetch game dilated motion vector and optical flow vector
    vec2 gameMv = texelFetch(r_dilated_motion_vectors, pos, 0).rg;
    vec2 flowMv = texelFetch(r_optical_flow, pos, 0).rg;

    // 2. Motion vector fusion: blend game motion vectors with optical flow
    vec2 mv;
    if (dot(gameMv, gameMv) > 1e-7) {
        mv = mix(gameMv, flowMv, 0.2);
    } else {
        mv = flowMv;
    }
    mv *= cbFrameInterpolation.fMotionScale;

    // 3. Bidirectional temporal coordinates at interpolationTime (typically 0.5)
    float t = clamp(cbFrameInterpolation.fInterpolationTime, 0.0, 1.0);
    vec2 uvCurr = uv - (1.0 - t) * mv;
    vec2 uvPrev = uv + t * mv;

    // Clamp to valid screen boundaries
    uvCurr = clamp(uvCurr, vec2(0.001), vec2(0.999));
    uvPrev = clamp(uvPrev, vec2(0.001), vec2(0.999));

    // 4. Sample bidirectional colors
    vec4 colorCurr = textureLod(sampler2D(r_current_back_buffer, s_LinearClamp), uvCurr, 0.0);
    vec4 colorPrev = textureLod(sampler2D(r_history_color, s_LinearClamp), uvPrev, 0.0);

    // 5. Depth disocclusion testing
    float currDepth = texelFetch(r_dilated_depth, pos, 0).r;
    float prevDepth = textureLod(sampler2D(r_reconstructed_prev_nearest_depth, s_PointClamp), uvPrev, 0.0).r;
    float depthDelta = abs(currDepth - prevDepth);

    // If disoccluded, geometry was newly revealed in current frame: fallback to current frame
    float wPrev = (depthDelta > cbFrameInterpolation.fDepthThreshold) ? 0.0 : (1.0 - t);
    float wCurr = 1.0 - wPrev;

    vec4 interpolatedColor = wCurr * colorCurr + wPrev * colorPrev;
    imageStore(rw_interpolated_output, pos, interpolatedColor);
}
