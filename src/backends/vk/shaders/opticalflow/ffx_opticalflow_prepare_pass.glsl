// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#version 450

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

layout (set = 0, binding = 0) uniform texture2D r_input_color;
layout (set = 0, binding = 1, rg16f) uniform image2D rw_optical_flow_pyramid;
layout (set = 0, binding = 2, r32ui) uniform uimage2D rw_scene_change_detection;

layout (set = 0, binding = 3, std140) uniform cbOpticalFlow_t {
    ivec2 iResolution;
    vec2  fInvResolution;
    uint  uFrameIndex;
    float fSceneChangeThreshold;
} cbOpticalFlow;

layout (set = 0, binding = 1000) uniform sampler s_LinearClamp;

#ifndef FFX_OPTICALFLOW_THREAD_GROUP_WIDTH
#define FFX_OPTICALFLOW_THREAD_GROUP_WIDTH 8
#endif
#ifndef FFX_OPTICALFLOW_THREAD_GROUP_HEIGHT
#define FFX_OPTICALFLOW_THREAD_GROUP_HEIGHT 8
#endif

layout (local_size_x = FFX_OPTICALFLOW_THREAD_GROUP_WIDTH,
        local_size_y = FFX_OPTICALFLOW_THREAD_GROUP_HEIGHT,
        local_size_z = 1) in;

void main()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    if (pos.x >= cbOpticalFlow.iResolution.x || pos.y >= cbOpticalFlow.iResolution.y) {
        return;
    }

    vec3 rgb = texelFetch(r_input_color, pos, 0).rgb;
    // Perceptual Rec.709 luminance
    float currLuma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));

    // Previous luminance stored in .r from previous frame
    vec2 prevData = imageLoad(rw_optical_flow_pyramid, pos).rg;
    float prevLuma = prevData.r;

    // Store curr in .r, previous in .g
    imageStore(rw_optical_flow_pyramid, pos, vec4(currLuma, prevLuma, 0.0, 0.0));

    // Scene change detection: measure luminance divergence
    float diff = abs(currLuma - prevLuma);
    if (diff > cbOpticalFlow.fSceneChangeThreshold && cbOpticalFlow.uFrameIndex > 0) {
        imageAtomicAdd(rw_scene_change_detection, ivec2(0, 0), 1u);
    }
}
