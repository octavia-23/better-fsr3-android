// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#version 450

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

layout (set = 0, binding = 0) uniform texture2D r_optical_flow_pyramid;
layout (set = 0, binding = 1, rg16f) uniform image2D rw_optical_flow;

layout (set = 0, binding = 2, std140) uniform cbOpticalFlow_t {
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

    // Current luminance is stored in .r, previous in .g
    float currLuma = texelFetch(r_optical_flow_pyramid, pos, 0).r;

    // First frame or camera cut: optical flow is zero
    if (cbOpticalFlow.uFrameIndex == 0) {
        imageStore(rw_optical_flow, pos, vec4(0.0, 0.0, 0.0, 0.0));
        return;
    }

    // 1. Coarse search (stride 2, range [-4, 4])
    int bestDx = 0;
    int bestDy = 0;
    float minSad = 1e9;

    for (int dy = -4; dy <= 4; dy += 2) {
        for (int dx = -4; dx <= 4; dx += 2) {
            ivec2 samplePos = clamp(pos + ivec2(dx, dy), ivec2(0), cbOpticalFlow.iResolution - 1);
            float prevLuma = texelFetch(r_optical_flow_pyramid, samplePos, 0).g;
            float sad = abs(currLuma - prevLuma) + 0.02 * float(dx * dx + dy * dy); // Regularization penalty
            if (sad < minSad) {
                minSad = sad;
                bestDx = dx;
                bestDy = dy;
            }
        }
    }

    // 2. Fine search (stride 1, range [-1, 1] around coarse candidate)
    int fineBestDx = bestDx;
    int fineBestDy = bestDy;
    float fineMinSad = minSad;

    for (int fdy = -1; fdy <= 1; ++fdy) {
        for (int fdx = -1; fdx <= 1; ++fdx) {
            int curDx = bestDx + fdx;
            int curDy = bestDy + fdy;
            ivec2 samplePos = clamp(pos + ivec2(curDx, curDy), ivec2(0), cbOpticalFlow.iResolution - 1);
            float prevLuma = texelFetch(r_optical_flow_pyramid, samplePos, 0).g;
            float sad = abs(currLuma - prevLuma) + 0.02 * float(curDx * curDx + curDy * curDy);
            if (sad < fineMinSad) {
                fineMinSad = sad;
                fineBestDx = curDx;
                fineBestDy = curDy;
            }
        }
    }

    // 3. Subpixel refinement
    ivec2 leftPos  = clamp(pos + ivec2(fineBestDx - 1, fineBestDy), ivec2(0), cbOpticalFlow.iResolution - 1);
    ivec2 rightPos = clamp(pos + ivec2(fineBestDx + 1, fineBestDy), ivec2(0), cbOpticalFlow.iResolution - 1);
    ivec2 upPos    = clamp(pos + ivec2(fineBestDx, fineBestDy - 1), ivec2(0), cbOpticalFlow.iResolution - 1);
    ivec2 downPos  = clamp(pos + ivec2(fineBestDx, fineBestDy + 1), ivec2(0), cbOpticalFlow.iResolution - 1);

    float sadLeft  = abs(currLuma - texelFetch(r_optical_flow_pyramid, leftPos, 0).g);
    float sadRight = abs(currLuma - texelFetch(r_optical_flow_pyramid, rightPos, 0).g);
    float sadUp    = abs(currLuma - texelFetch(r_optical_flow_pyramid, upPos, 0).g);
    float sadDown  = abs(currLuma - texelFetch(r_optical_flow_pyramid, downPos, 0).g);

    float subX = (sadLeft - sadRight) / (2.0 * max(sadLeft - 2.0 * fineMinSad + sadRight, 1e-4));
    float subY = (sadUp - sadDown) / (2.0 * max(sadUp - 2.0 * fineMinSad + sadDown, 1e-4));
    subX = clamp(subX, -0.5, 0.5);
    subY = clamp(subY, -0.5, 0.5);

    vec2 motionVecPixels = vec2(float(fineBestDx) + subX, float(fineBestDy) + subY);
    // Convert to normalized UV motion
    vec2 motionVecUV = motionVecPixels * cbOpticalFlow.fInvResolution;

    imageStore(rw_optical_flow, pos, vec4(motionVecUV, 0.0, 0.0));
}
