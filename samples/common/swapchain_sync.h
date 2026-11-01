// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#pragma once

#if defined(__ANDROID__)
#	define VK_USE_PLATFORM_ANDROID_KHR 1
#elif defined(_WIN32)
#	define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace arm {

/// Reusable swapchain synchronization state machine.
/// Implements beginFrame, endFrame, and abortFrame with fence/semaphore lifecycle management
/// and fault-tolerant synchronization object recovery.
class SwapchainSync {
public:
    static constexpr uint32_t kFramesInFlight = 2;

    struct VulkanFunctions {
        VkResult (VKAPI_PTR *vkWaitForFences)(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t) = nullptr;
        VkResult (VKAPI_PTR *vkAcquireNextImageKHR)(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*) = nullptr;
        VkResult (VKAPI_PTR *vkResetFences)(VkDevice, uint32_t, const VkFence*) = nullptr;
        VkResult (VKAPI_PTR *vkResetCommandBuffer)(VkCommandBuffer, VkCommandBufferResetFlags) = nullptr;
        VkResult (VKAPI_PTR *vkBeginCommandBuffer)(VkCommandBuffer, const VkCommandBufferBeginInfo*) = nullptr;
        VkResult (VKAPI_PTR *vkEndCommandBuffer)(VkCommandBuffer) = nullptr;
        VkResult (VKAPI_PTR *vkQueueSubmit)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence) = nullptr;
        VkResult (VKAPI_PTR *vkQueuePresentKHR)(VkQueue, const VkPresentInfoKHR*) = nullptr;
        void (VKAPI_PTR *vkCmdPipelineBarrier)(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*) = nullptr;
        void (VKAPI_PTR *vkCmdClearColorImage)(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*, uint32_t, const VkImageSubresourceRange*) = nullptr;
        VkResult (VKAPI_PTR *vkDeviceWaitIdle)(VkDevice) = nullptr;
        VkResult (VKAPI_PTR *vkCreateFence)(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence*) = nullptr;
        void (VKAPI_PTR *vkDestroyFence)(VkDevice, VkFence, const VkAllocationCallbacks*) = nullptr;
        VkResult (VKAPI_PTR *vkCreateSemaphore)(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore*) = nullptr;
        void (VKAPI_PTR *vkDestroySemaphore)(VkDevice, VkSemaphore, const VkAllocationCallbacks*) = nullptr;
    };

    void initStandardFunctions()
    {
#if !defined(SWAPCHAIN_SYNC_NO_VULKAN_SYMBOLS)
        fn_.vkWaitForFences = ::vkWaitForFences;
        fn_.vkAcquireNextImageKHR = ::vkAcquireNextImageKHR;
        fn_.vkResetFences = ::vkResetFences;
        fn_.vkResetCommandBuffer = ::vkResetCommandBuffer;
        fn_.vkBeginCommandBuffer = ::vkBeginCommandBuffer;
        fn_.vkEndCommandBuffer = ::vkEndCommandBuffer;
        fn_.vkQueueSubmit = ::vkQueueSubmit;
        fn_.vkQueuePresentKHR = ::vkQueuePresentKHR;
        fn_.vkCmdPipelineBarrier = ::vkCmdPipelineBarrier;
        fn_.vkCmdClearColorImage = ::vkCmdClearColorImage;
        fn_.vkDeviceWaitIdle = ::vkDeviceWaitIdle;
        fn_.vkCreateFence = ::vkCreateFence;
        fn_.vkDestroyFence = ::vkDestroyFence;
        fn_.vkCreateSemaphore = ::vkCreateSemaphore;
        fn_.vkDestroySemaphore = ::vkDestroySemaphore;
#endif
    }

    void setVulkanFunctions(const VulkanFunctions& fn) { fn_ = fn; }
    VulkanFunctions& vulkanFunctions() { return fn_; }

    void init(VkDevice device, VkQueue queue, VkSwapchainKHR swapchain,
              const std::vector<VkImage>& swapchainImages,
              const VkCommandBuffer* commandBuffers,
              VkSemaphore* acquireSemaphores,
              VkSemaphore* releaseSemaphores,
              VkFence* inFlightFences)
    {
        device_ = device;
        queue_ = queue;
        swapchain_ = swapchain;
        swapchainImages_ = swapchainImages;
        pCallerAcquireSemaphores_ = acquireSemaphores;
        pCallerReleaseSemaphores_ = releaseSemaphores;
        pCallerInFlightFences_ = inFlightFences;

        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            commandBuffers_[i] = commandBuffers ? commandBuffers[i] : VK_NULL_HANDLE;
            acquireSemaphores_[i] = acquireSemaphores ? acquireSemaphores[i] : VK_NULL_HANDLE;
            releaseSemaphores_[i] = releaseSemaphores ? releaseSemaphores[i] : VK_NULL_HANDLE;
            inFlightFences_[i] = inFlightFences ? inFlightFences[i] : VK_NULL_HANDLE;
        }
        frame_ = 0;
        swapchainSuboptimal_ = false;
        imageAcquired_ = false;
        acquiredImageIndex_ = 0;
        acquireSemaphoreConsumed_ = false;
        fenceReset_ = false;
        submissionQueued_ = false;
        deviceLost_ = false;
    }

    void onSwapchainRecreated()
    {
        swapchainSuboptimal_ = false;
        imageAcquired_ = false;
        acquireSemaphoreConsumed_ = false;
        fenceReset_ = false;
        submissionQueued_ = false;
    }

    uint32_t frame() const { return frame_; }
    bool swapchainSuboptimal() const { return swapchainSuboptimal_; }
    bool isImageAcquired() const { return imageAcquired_; }
    uint32_t acquiredImageIndex() const { return acquiredImageIndex_; }
    bool isAcquireSemaphoreConsumed() const { return acquireSemaphoreConsumed_; }
    bool isFenceReset() const { return fenceReset_; }
    bool isSubmissionQueued() const { return submissionQueued_; }
    bool isDeviceLost() const { return deviceLost_; }
    bool isTerminalError() const { return deviceLost_; }

    VkFence inFlightFence(uint32_t i) const { return inFlightFences_[i]; }
    VkSemaphore acquireSemaphore(uint32_t i) const { return acquireSemaphores_[i]; }
    VkSemaphore releaseSemaphore(uint32_t i) const { return releaseSemaphores_[i]; }

    bool resetFrameSyncObjects(uint32_t frameIndex)
    {
        if (!device_) return false;
        if (fn_.vkDeviceWaitIdle) {
            fn_.vkDeviceWaitIdle(device_);
        }

        // 1. Recover Fence: must be in SIGNALED state for next beginFrame
        if (fn_.vkCreateFence && fn_.vkDestroyFence) {
            if (inFlightFences_[frameIndex] != VK_NULL_HANDLE) {
                fn_.vkDestroyFence(device_, inFlightFences_[frameIndex], nullptr);
                inFlightFences_[frameIndex] = VK_NULL_HANDLE;
            }
            VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (fn_.vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[frameIndex]) != VK_SUCCESS) {
                deviceLost_ = true;
                return false;
            }
            if (pCallerInFlightFences_) {
                pCallerInFlightFences_[frameIndex] = inFlightFences_[frameIndex];
            }
        }

        // 2. Recover Acquire Semaphore: must be fresh and UNSIGNALED
        if (fn_.vkCreateSemaphore && fn_.vkDestroySemaphore) {
            if (acquireSemaphores_[frameIndex] != VK_NULL_HANDLE) {
                fn_.vkDestroySemaphore(device_, acquireSemaphores_[frameIndex], nullptr);
                acquireSemaphores_[frameIndex] = VK_NULL_HANDLE;
            }
            VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (fn_.vkCreateSemaphore(device_, &semInfo, nullptr, &acquireSemaphores_[frameIndex]) != VK_SUCCESS) {
                deviceLost_ = true;
                return false;
            }
            if (pCallerAcquireSemaphores_) {
                pCallerAcquireSemaphores_[frameIndex] = acquireSemaphores_[frameIndex];
            }

            // 3. Recover Release Semaphore: must be fresh and UNSIGNALED
            if (releaseSemaphores_[frameIndex] != VK_NULL_HANDLE) {
                fn_.vkDestroySemaphore(device_, releaseSemaphores_[frameIndex], nullptr);
                releaseSemaphores_[frameIndex] = VK_NULL_HANDLE;
            }
            if (fn_.vkCreateSemaphore(device_, &semInfo, nullptr, &releaseSemaphores_[frameIndex]) != VK_SUCCESS) {
                deviceLost_ = true;
                return false;
            }
            if (pCallerReleaseSemaphores_) {
                pCallerReleaseSemaphores_[frameIndex] = releaseSemaphores_[frameIndex];
            }
        }

        fenceReset_ = false;
        imageAcquired_ = false;
        acquireSemaphoreConsumed_ = false;
        submissionQueued_ = false;
        return true;
    }

    bool beginFrame(uint32_t* outImageIndex)
    {
        imageAcquired_ = false;
        acquireSemaphoreConsumed_ = false;
        submissionQueued_ = false;

        VkResult waitRes = fn_.vkWaitForFences(device_, 1, &inFlightFences_[frame_], VK_TRUE, UINT64_MAX);
        if (waitRes != VK_SUCCESS) {
            if (waitRes == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            }
            return false;
        }

        VkResult acquired = fn_.vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                                      acquireSemaphores_[frame_], VK_NULL_HANDLE,
                                                      outImageIndex);
        if (acquired == VK_ERROR_DEVICE_LOST) {
            deviceLost_ = true;
            return false;
        }
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            // No image acquired, semaphore not signalled, fence remains in signalled state.
            return false;
        }
        if (acquired == VK_SUBOPTIMAL_KHR) {
            swapchainSuboptimal_ = true;
            imageAcquired_ = true;
            if (outImageIndex) acquiredImageIndex_ = *outImageIndex;
        } else if (acquired == VK_SUCCESS) {
            swapchainSuboptimal_ = false;
            imageAcquired_ = true;
            if (outImageIndex) acquiredImageIndex_ = *outImageIndex;
        } else {
            return false;
        }

        VkResult resetFenceRes = fn_.vkResetFences(device_, 1, &inFlightFences_[frame_]);
        if (resetFenceRes != VK_SUCCESS) {
            if (resetFenceRes == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            }
            resetFrameSyncObjects(frame_);
            return false;
        }
        fenceReset_ = true;

        VkResult resetCmdRes = fn_.vkResetCommandBuffer(commandBuffers_[frame_], 0);
        if (resetCmdRes != VK_SUCCESS) {
            if (resetCmdRes == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            }
            resetFrameSyncObjects(frame_);
            return false;
        }

        VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkResult beginCmdRes = fn_.vkBeginCommandBuffer(commandBuffers_[frame_], &beginInfo);
        if (beginCmdRes != VK_SUCCESS) {
            if (beginCmdRes == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            }
            resetFrameSyncObjects(frame_);
            return false;
        }
        return true;
    }

    bool endFrame(uint32_t imageIndex)
    {
        VkResult endCmdRes = fn_.vkEndCommandBuffer(commandBuffers_[frame_]);
        if (endCmdRes != VK_SUCCESS) {
            if (endCmdRes == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            }
            resetFrameSyncObjects(frame_);
            return false;
        }

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount   = 1;
        submit.pWaitSemaphores      = &acquireSemaphores_[frame_];
        submit.pWaitDstStageMask    = &waitStage;
        submit.commandBufferCount   = 1;
        submit.pCommandBuffers      = &commandBuffers_[frame_];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores    = &releaseSemaphores_[frame_];

        VkResult submitRes = fn_.vkQueueSubmit(queue_, 1, &submit, inFlightFences_[frame_]);
        if (submitRes != VK_SUCCESS) {
            if (submitRes == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            }
            resetFrameSyncObjects(frame_);
            return false;
        }

        submissionQueued_ = true;
        acquireSemaphoreConsumed_ = true;
        fenceReset_ = false;

        VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores    = &releaseSemaphores_[frame_];
        present.swapchainCount     = 1;
        present.pSwapchains        = &swapchain_;
        present.pImageIndices      = &imageIndex;

        VkResult presented = fn_.vkQueuePresentKHR(queue_, &present);
        frame_ = (frame_ + 1) % kFramesInFlight;
        imageAcquired_ = false;
        submissionQueued_ = false;

        if (presented == VK_ERROR_DEVICE_LOST) {
            deviceLost_ = true;
            return false;
        }
        if (swapchainSuboptimal_ || presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
            swapchainSuboptimal_ = false;
            return false;
        }
        if (presented != VK_SUCCESS) {
            return false;
        }
        return true;
    }

    bool abortFrame(uint32_t imageIndex)
    {
        // Discard any partial command recording by resetting the command buffer.
        if (fn_.vkResetCommandBuffer(commandBuffers_[frame_], 0) != VK_SUCCESS) {
            resetFrameSyncObjects(frame_);
            return false;
        }

        VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (fn_.vkBeginCommandBuffer(commandBuffers_[frame_], &beginInfo) != VK_SUCCESS) {
            resetFrameSyncObjects(frame_);
            return false;
        }

        // Transition swapchain image from UNDEFINED to TRANSFER_DST_OPTIMAL
        VkImageMemoryBarrier toTransfer = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransfer.srcAccessMask       = 0;
        toTransfer.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image               = (imageIndex < swapchainImages_.size()) ? swapchainImages_[imageIndex] : VK_NULL_HANDLE;
        toTransfer.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        fn_.vkCmdPipelineBarrier(commandBuffers_[frame_],
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

        // Clear swapchain image to solid black so no uninitialized contents appear
        VkClearColorValue blackColor = {};
        VkImageSubresourceRange clearRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        fn_.vkCmdClearColorImage(commandBuffers_[frame_],
                                 (imageIndex < swapchainImages_.size()) ? swapchainImages_[imageIndex] : VK_NULL_HANDLE,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &blackColor, 1, &clearRange);

        // Transition swapchain image from TRANSFER_DST_OPTIMAL to PRESENT_SRC_KHR
        VkImageMemoryBarrier toPresent = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toPresent.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        toPresent.dstAccessMask       = 0;
        toPresent.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toPresent.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.image               = (imageIndex < swapchainImages_.size()) ? swapchainImages_[imageIndex] : VK_NULL_HANDLE;
        toPresent.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        fn_.vkCmdPipelineBarrier(commandBuffers_[frame_],
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toPresent);

        if (fn_.vkEndCommandBuffer(commandBuffers_[frame_]) != VK_SUCCESS) {
            resetFrameSyncObjects(frame_);
            return false;
        }

        // Submit and signal release semaphore + inFlight fence so synchronization remains intact
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount   = 1;
        submit.pWaitSemaphores      = &acquireSemaphores_[frame_];
        submit.pWaitDstStageMask    = &waitStage;
        submit.commandBufferCount   = 1;
        submit.pCommandBuffers      = &commandBuffers_[frame_];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores    = &releaseSemaphores_[frame_];
        if (fn_.vkQueueSubmit(queue_, 1, &submit, inFlightFences_[frame_]) != VK_SUCCESS) {
            resetFrameSyncObjects(frame_);
            return false;
        }

        submissionQueued_ = true;
        acquireSemaphoreConsumed_ = true;
        fenceReset_ = false;

        // Present image to return ownership to the swapchain presentation engine
        VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores    = &releaseSemaphores_[frame_];
        present.swapchainCount     = 1;
        present.pSwapchains        = &swapchain_;
        present.pImageIndices      = &imageIndex;

        VkResult presented = fn_.vkQueuePresentKHR(queue_, &present);
        frame_ = (frame_ + 1) % kFramesInFlight;
        imageAcquired_ = false;
        submissionQueued_ = false;

        if (presented == VK_ERROR_DEVICE_LOST) {
            deviceLost_ = true;
            return false;
        }
        if (swapchainSuboptimal_ || presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
            swapchainSuboptimal_ = false;
            return false;
        }
        if (presented != VK_SUCCESS) {
            return false;
        }
        return true;
    }

private:
    VkDevice                 device_ = VK_NULL_HANDLE;
    VkQueue                  queue_ = VK_NULL_HANDLE;
    VkSwapchainKHR           swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage>     swapchainImages_;
    VkCommandBuffer          commandBuffers_[kFramesInFlight] = {};
    VkSemaphore              acquireSemaphores_[kFramesInFlight] = {};
    VkSemaphore              releaseSemaphores_[kFramesInFlight] = {};
    VkFence                  inFlightFences_[kFramesInFlight] = {};
    VkSemaphore*             pCallerAcquireSemaphores_ = nullptr;
    VkSemaphore*             pCallerReleaseSemaphores_ = nullptr;
    VkFence*                 pCallerInFlightFences_ = nullptr;
    uint32_t                 frame_ = 0;
    bool                     swapchainSuboptimal_ = false;
    bool                     imageAcquired_ = false;
    uint32_t                 acquiredImageIndex_ = 0;
    bool                     acquireSemaphoreConsumed_ = false;
    bool                     fenceReset_ = false;
    bool                     submissionQueued_ = false;
    bool                     deviceLost_ = false;
    VulkanFunctions          fn_;
};

} // namespace arm
