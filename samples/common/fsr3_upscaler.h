// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT
#pragma once

#include "scene.h"

#include <host/backends/vk/ffxm_vk.h>
#include <host/ffxm_fsr3upscaler.h>

#include <cstdint>
#include <memory>
#include <vector>

class VkContext;

namespace arm {

struct VulkanResourceCleanupFunctions {
    void (VKAPI_PTR *vkDestroyImageView)(VkDevice, VkImageView, const VkAllocationCallbacks*) = nullptr;
    void (VKAPI_PTR *vkDestroyImage)(VkDevice, VkImage, const VkAllocationCallbacks*) = nullptr;
    void (VKAPI_PTR *vkFreeMemory)(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) = nullptr;
};

// Production cleanup function for Vulkan render targets (used by Fsr3Upscaler and Scene):
// Destroys image view, then image, then frees bound memory, and resets rt to {}.
inline void destroyRenderTarget(VkDevice device, RenderTarget& rt,
                                const VulkanResourceCleanupFunctions* fn = nullptr)
{
    if (device == VK_NULL_HANDLE) {
        rt = {};
        return;
    }

    if (fn) {
        if (rt.view != VK_NULL_HANDLE) {
            if (fn->vkDestroyImageView) fn->vkDestroyImageView(device, rt.view, nullptr);
            rt.view = VK_NULL_HANDLE;
        }
        if (rt.image != VK_NULL_HANDLE) {
            if (fn->vkDestroyImage) fn->vkDestroyImage(device, rt.image, nullptr);
            rt.image = VK_NULL_HANDLE;
        }
        if (rt.memory != VK_NULL_HANDLE) {
            if (fn->vkFreeMemory) fn->vkFreeMemory(device, rt.memory, nullptr);
            rt.memory = VK_NULL_HANDLE;
        }
    } else {
#if !defined(SWAPCHAIN_SYNC_NO_VULKAN_SYMBOLS)
        if (rt.view != VK_NULL_HANDLE) {
            ::vkDestroyImageView(device, rt.view, nullptr);
            rt.view = VK_NULL_HANDLE;
        }
        if (rt.image != VK_NULL_HANDLE) {
            ::vkDestroyImage(device, rt.image, nullptr);
            rt.image = VK_NULL_HANDLE;
        }
        if (rt.memory != VK_NULL_HANDLE) {
            ::vkFreeMemory(device, rt.memory, nullptr);
            rt.memory = VK_NULL_HANDLE;
        }
#endif
    }
    rt = {};
}

// Production cleanup function for the three FSR 3.1.4 shared resources.
inline void destroySharedResources(VkDevice device,
                                   RenderTarget& dilatedDepth,
                                   RenderTarget& dilatedMotionVectors,
                                   RenderTarget& reconstructedPrevNearestDepth,
                                   const VulkanResourceCleanupFunctions* fn = nullptr)
{
    destroyRenderTarget(device, dilatedDepth, fn);
    destroyRenderTarget(device, dilatedMotionVectors, fn);
    destroyRenderTarget(device, reconstructedPrevNearestDepth, fn);
}

} // namespace arm

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

    static void destroyTarget(VkDevice device, RenderTarget& rt,
                              const arm::VulkanResourceCleanupFunctions* fn = nullptr)
    {
        arm::destroyRenderTarget(device, rt, fn);
    }

    static void destroySharedResourceTargets(VkDevice device,
                                             RenderTarget& dilatedDepth,
                                             RenderTarget& dilatedMotionVectors,
                                             RenderTarget& reconstructedPrevNearestDepth,
                                             const arm::VulkanResourceCleanupFunctions* fn = nullptr)
    {
        arm::destroySharedResources(device, dilatedDepth, dilatedMotionVectors, reconstructedPrevNearestDepth, fn);
    }

    void destroySharedResources(VkDevice device,
                                const arm::VulkanResourceCleanupFunctions* fn = nullptr)
    {
        arm::destroySharedResources(device, dilatedDepth_, dilatedMotionVectors_, reconstructedPrevNearestDepth_, fn);
        sharedResourcesInitialized_ = false;
    }

    void destroySharedResources(VkContext& ctx);
    bool createSharedResources(VkContext& ctx);

    RenderTarget& dilatedDepth() { return dilatedDepth_; }
    RenderTarget& dilatedMotionVectors() { return dilatedMotionVectors_; }
    RenderTarget& reconstructedPrevNearestDepth() { return reconstructedPrevNearestDepth_; }
    const RenderTarget& dilatedDepth() const { return dilatedDepth_; }
    const RenderTarget& dilatedMotionVectors() const { return dilatedMotionVectors_; }
    const RenderTarget& reconstructedPrevNearestDepth() const { return reconstructedPrevNearestDepth_; }

private:
    bool createTarget(VkContext& ctx, RenderTarget& rt, VkFormat format, VkExtent2D extent,
                      VkImageUsageFlags usage, VkImageAspectFlags aspect);

    VkDevice                 device_        = VK_NULL_HANDLE;
    VkContext*               pVkCtx_        = nullptr;
    arm::FfxmInterface       interface_     = {};
    std::unique_ptr<arm::FfxmFsr3UpscalerContext> context_;
    std::vector<uint8_t>     scratchBuffer_;
    bool                     contextCreated_= false;
    int32_t                  jitterPhaseCount_ = 1;
    VkExtent2D               renderExtent_  = {0, 0};
    VkExtent2D               displayExtent_ = {0, 0};

    // Shared resources required by FSR 3.1.4 (dilated depth, dilated motion vectors, reconstructed depth)
    RenderTarget             dilatedDepth_;
    RenderTarget             dilatedMotionVectors_;
    RenderTarget             reconstructedPrevNearestDepth_;
    bool                     sharedResourcesInitialized_ = false;
};
