// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT
#pragma once

#include "common.h"
#include "scene.h"
#include "vk_context.h"

#include <host/backends/vk/ffxm_vk.h>
#include <host/ffxm_fsr3upscaler.h>

#include <cstdint>
#include <vector>

// Wraps the FSR 3.1.4 compute upscaler integration path:
// scratch buffer -> FfxmDevice -> FfxmInterface -> FfxmFsr3UpscalerContext ->
// shared resource allocation -> per-frame compute dispatch across all 7 passes.
class Fsr3Upscaler {
public:
    bool init(VkContext& ctx, VkExtent2D renderExtent, VkExtent2D displayExtent,
              arm::FfxmFsr3UpscalerQualityMode quality);
    void destroy();

    // Records the upscale for this frame across all seven compute passes.
    // `frameIndex` drives the jitter sequence; `reset` should be true on camera cuts.
    bool dispatch(VkCommandBuffer cmd, const Scene& scene, VkExtent2D renderExtent,
                  float frameTimeMs, int32_t frameIndex, bool reset);

    // Sub-pixel jitter in pixels for `frameIndex`, to be folded into the camera.
    void jitterForFrame(int32_t frameIndex, float* outX, float* outY) const;

    static uint32_t initializationFlags();

    const RenderTarget& dilatedDepth() const { return dilatedDepth_; }
    const RenderTarget& dilatedMotionVectors() const { return dilatedMotionVectors_; }
    const RenderTarget& reconstructedPrevNearestDepth() const { return reconstructedPrevNearestDepth_; }

private:
    bool createTarget(VkContext& ctx, RenderTarget& rt, VkFormat format, VkExtent2D extent,
                      VkImageUsageFlags usage, VkImageAspectFlags aspect);
    bool createSharedResources(VkContext& ctx);
    void destroySharedResources(VkContext& ctx);

    VkDevice                 device_        = VK_NULL_HANDLE;
    VkContext*               pVkCtx_        = nullptr;
    arm::FfxmInterface       interface_     = {};
    arm::FfxmFsr3UpscalerContext context_   = {};
    std::vector<uint8_t>     scratchBuffer_;
    bool                     contextCreated_= false;
    int32_t                  jitterPhaseCount_ = 1;
    VkExtent2D               renderExtent_  = {0, 0};
    VkExtent2D               displayExtent_ = {0, 0};

    // Shared resources required by FSR 3.1.4 (dilated depth, dilated motion vectors, reconstructed depth)
    RenderTarget             dilatedDepth_;
    RenderTarget             dilatedMotionVectors_;
    RenderTarget             reconstructedPrevNearestDepth_;
};
