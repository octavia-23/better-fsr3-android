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

    enum class PresentResult {
        Success,
        Suboptimal,
        OutOfDate,
        SurfaceLost,
        DeviceLost,
        OtherError
    };

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
        recoveryFailed_ = false;
        surfaceLost_ = false;
        lastPresentResult_ = PresentResult::Success;
    }

    /// Clears all retained Vulkan handles, caller pointers, and sync state without
    /// destroying resources owned by VkContext.
    void reset()
    {
        device_ = VK_NULL_HANDLE;
        queue_ = VK_NULL_HANDLE;
        swapchain_ = VK_NULL_HANDLE;
        swapchainImages_.clear();
        pCallerAcquireSemaphores_ = nullptr;
        pCallerReleaseSemaphores_ = nullptr;
        pCallerInFlightFences_ = nullptr;

        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            commandBuffers_[i] = VK_NULL_HANDLE;
            acquireSemaphores_[i] = VK_NULL_HANDLE;
            releaseSemaphores_[i] = VK_NULL_HANDLE;
            inFlightFences_[i] = VK_NULL_HANDLE;
        }

        frame_ = 0;
        swapchainSuboptimal_ = false;
        imageAcquired_ = false;
        acquiredImageIndex_ = 0;
        acquireSemaphoreConsumed_ = false;
        fenceReset_ = false;
        submissionQueued_ = false;
        deviceLost_ = false;
        recoveryFailed_ = false;
        surfaceLost_ = false;
        lastPresentResult_ = PresentResult::Success;
    }

    void onSwapchainRecreated()
    {
        if (isTerminalError()) {
            return;
        }
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
    bool isRecoveryFailed() const { return recoveryFailed_; }
    bool isSurfaceLost() const { return surfaceLost_; }
    bool isTerminalError() const { return deviceLost_ || recoveryFailed_ || surfaceLost_; }
    PresentResult lastPresentResult() const { return lastPresentResult_; }

    void notifyDeviceLost() { deviceLost_ = true; }
    void notifyRecoveryFailed() { recoveryFailed_ = true; }
    void notifySurfaceLost() { surfaceLost_ = true; }

    VkFence inFlightFence(uint32_t i) const { return (i < kFramesInFlight) ? inFlightFences_[i] : VK_NULL_HANDLE; }
    VkSemaphore acquireSemaphore(uint32_t i) const { return (i < kFramesInFlight) ? acquireSemaphores_[i] : VK_NULL_HANDLE; }
    VkSemaphore releaseSemaphore(uint32_t i) const { return (i < kFramesInFlight) ? releaseSemaphores_[i] : VK_NULL_HANDLE; }
    VkCommandBuffer commandBuffer(uint32_t i) const { return (i < kFramesInFlight) ? commandBuffers_[i] : VK_NULL_HANDLE; }

    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    VkSwapchainKHR swapchain() const { return swapchain_; }
    const std::vector<VkImage>& swapchainImages() const { return swapchainImages_; }
    const VkSemaphore* callerAcquireSemaphores() const { return pCallerAcquireSemaphores_; }
    const VkSemaphore* callerReleaseSemaphores() const { return pCallerReleaseSemaphores_; }
    const VkFence* callerInFlightFences() const { return pCallerInFlightFences_; }

    bool resetFrameSyncObjects(uint32_t frameIndex)
    {
        if (isTerminalError() || !device_) return false;
        if (frameIndex >= kFramesInFlight) return false;

        if (!fn_.vkDeviceWaitIdle || !fn_.vkCreateFence || !fn_.vkDestroyFence ||
            !fn_.vkCreateSemaphore || !fn_.vkDestroySemaphore) {
            recoveryFailed_ = true;
            return false;
        }

        VkResult idleRes = fn_.vkDeviceWaitIdle(device_);
        if (idleRes != VK_SUCCESS) {
            if (idleRes == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            } else {
                recoveryFailed_ = true;
            }
            return false;
        }

        // Transactional replacement of synchronization handles:
        // 1. Create temporary replacement objects without destroying old objects.
        VkFence newFence = VK_NULL_HANDLE;
        VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkResult res = fn_.vkCreateFence(device_, &fenceInfo, nullptr, &newFence);
        if (res != VK_SUCCESS) {
            if (res == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            } else {
                recoveryFailed_ = true;
            }
            return false;
        }

        VkSemaphore newAcquireSem = VK_NULL_HANDLE;
        VkSemaphore newReleaseSem = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        res = fn_.vkCreateSemaphore(device_, &semInfo, nullptr, &newAcquireSem);
        if (res != VK_SUCCESS) {
            if (res == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            } else {
                recoveryFailed_ = true;
            }
            fn_.vkDestroyFence(device_, newFence, nullptr);
            return false;
        }

        res = fn_.vkCreateSemaphore(device_, &semInfo, nullptr, &newReleaseSem);
        if (res != VK_SUCCESS) {
            if (res == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            } else {
                recoveryFailed_ = true;
            }
            fn_.vkDestroySemaphore(device_, newAcquireSem, nullptr);
            fn_.vkDestroyFence(device_, newFence, nullptr);
            return false;
        }

        // 2. All temporary objects created successfully.
        // Save old handles for safe destruction.
        VkFence oldFence = inFlightFences_[frameIndex];
        VkSemaphore oldAcquire = acquireSemaphores_[frameIndex];
        VkSemaphore oldRelease = releaseSemaphores_[frameIndex];

        // Atomically replace internal and caller-owned handles in lockstep
        inFlightFences_[frameIndex] = newFence;
        acquireSemaphores_[frameIndex] = newAcquireSem;
        releaseSemaphores_[frameIndex] = newReleaseSem;

        if (pCallerInFlightFences_) {
            pCallerInFlightFences_[frameIndex] = newFence;
        }
        if (pCallerAcquireSemaphores_) {
            pCallerAcquireSemaphores_[frameIndex] = newAcquireSem;
        }
        if (pCallerReleaseSemaphores_) {
            pCallerReleaseSemaphores_[frameIndex] = newReleaseSem;
        }

        // 3. Destroy old objects only now that new ones are in place.
        if (oldFence != VK_NULL_HANDLE) {
            fn_.vkDestroyFence(device_, oldFence, nullptr);
        }
        if (oldAcquire != VK_NULL_HANDLE) {
            fn_.vkDestroySemaphore(device_, oldAcquire, nullptr);
        }
        if (oldRelease != VK_NULL_HANDLE) {
            fn_.vkDestroySemaphore(device_, oldRelease, nullptr);
        }

        fenceReset_ = false;
        imageAcquired_ = false;
        acquireSemaphoreConsumed_ = false;
        submissionQueued_ = false;
        return true;
    }

    bool beginFrame(uint32_t* outImageIndex)
    {
        if (isTerminalError() || !device_ || !outImageIndex || swapchainImages_.empty()) {
            return false;
        }

        imageAcquired_ = false;
        acquireSemaphoreConsumed_ = false;
        submissionQueued_ = false;

        VkResult waitRes = fn_.vkWaitForFences(device_, 1, &inFlightFences_[frame_], VK_TRUE, UINT64_MAX);
        if (waitRes != VK_SUCCESS) {
            if (waitRes == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            }
            fenceReset_ = false;
            return false;
        }

        VkResult acquired = fn_.vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                                      acquireSemaphores_[frame_], VK_NULL_HANDLE,
                                                      outImageIndex);
        if (acquired == VK_ERROR_DEVICE_LOST) {
            deviceLost_ = true;
            fenceReset_ = false;
            return false;
        }
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            // No image acquired, semaphore not signalled, fence remains in signalled state.
            fenceReset_ = false;
            return false;
        }
        if (acquired == VK_SUBOPTIMAL_KHR) {
            swapchainSuboptimal_ = true;
            imageAcquired_ = true;
            acquiredImageIndex_ = *outImageIndex;
        } else if (acquired == VK_SUCCESS) {
            swapchainSuboptimal_ = false;
            imageAcquired_ = true;
            acquiredImageIndex_ = *outImageIndex;
        } else {
            fenceReset_ = false;
            return false;
        }

        VkResult resetFenceRes = fn_.vkResetFences(device_, 1, &inFlightFences_[frame_]);
        if (resetFenceRes != VK_SUCCESS) {
            if (resetFenceRes == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            }
            resetFrameSyncObjects(frame_);
            imageAcquired_ = false;
            fenceReset_ = false;
            return false;
        }
        fenceReset_ = true;

        VkResult resetCmdRes = fn_.vkResetCommandBuffer(commandBuffers_[frame_], 0);
        if (resetCmdRes != VK_SUCCESS) {
            if (resetCmdRes == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            }
            resetFrameSyncObjects(frame_);
            imageAcquired_ = false;
            fenceReset_ = false;
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
            imageAcquired_ = false;
            fenceReset_ = false;
            return false;
        }
        return true;
    }

    bool endFrame(uint32_t imageIndex)
    {
        if (isTerminalError() || !imageAcquired_) {
            return false;
        }
        if (swapchainImages_.empty() || imageIndex >= swapchainImages_.size()) {
            return false;
        }
        if (imageIndex != acquiredImageIndex_) {
            return false;
        }

        VkResult endCmdRes = fn_.vkEndCommandBuffer(commandBuffers_[frame_]);
        if (endCmdRes != VK_SUCCESS) {
            if (endCmdRes == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            }
            resetFrameSyncObjects(frame_);
            imageAcquired_ = false;
            fenceReset_ = false;
            submissionQueued_ = false;
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
            imageAcquired_ = false;
            fenceReset_ = false;
            submissionQueued_ = false;
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
        imageAcquired_ = false;
        submissionQueued_ = false;

        if (presented == VK_SUCCESS) {
            lastPresentResult_ = swapchainSuboptimal_ ? PresentResult::Suboptimal : PresentResult::Success;
            frame_ = (frame_ + 1) % kFramesInFlight;
            if (swapchainSuboptimal_) {
                swapchainSuboptimal_ = false;
                return false;
            }
            return true;
        }

        if (presented == VK_SUBOPTIMAL_KHR) {
            lastPresentResult_ = PresentResult::Suboptimal;
            swapchainSuboptimal_ = false;
            frame_ = (frame_ + 1) % kFramesInFlight;
            return false;
        }

        if (presented == VK_ERROR_OUT_OF_DATE_KHR) {
            lastPresentResult_ = PresentResult::OutOfDate;
            swapchainSuboptimal_ = false;
            frame_ = (frame_ + 1) % kFramesInFlight;
            return false;
        }

        if (presented == VK_ERROR_SURFACE_LOST_KHR) {
            lastPresentResult_ = PresentResult::SurfaceLost;
            surfaceLost_ = true;
            return false;
        }

        if (presented == VK_ERROR_DEVICE_LOST) {
            lastPresentResult_ = PresentResult::DeviceLost;
            deviceLost_ = true;
            return false;
        }

        // Other Vulkan failures:
        // Uncertain presentation state means synchronization objects cannot be safely reused.
        lastPresentResult_ = PresentResult::OtherError;
        recoveryFailed_ = true;
        return false;
    }

    bool abortFrame(uint32_t imageIndex)
    {
        if (isTerminalError() || !imageAcquired_) {
            return false;
        }
        if (swapchainImages_.empty() || imageIndex >= swapchainImages_.size()) {
            return false;
        }
        if (imageIndex != acquiredImageIndex_) {
            return false;
        }

        // Discard any partial command recording by resetting the command buffer.
        VkResult resetRes = fn_.vkResetCommandBuffer(commandBuffers_[frame_], 0);
        if (resetRes != VK_SUCCESS) {
            if (resetRes == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            }
            resetFrameSyncObjects(frame_);
            imageAcquired_ = false;
            fenceReset_ = false;
            submissionQueued_ = false;
            return false;
        }

        VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkResult beginRes = fn_.vkBeginCommandBuffer(commandBuffers_[frame_], &beginInfo);
        if (beginRes != VK_SUCCESS) {
            if (beginRes == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            }
            resetFrameSyncObjects(frame_);
            imageAcquired_ = false;
            fenceReset_ = false;
            submissionQueued_ = false;
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
        toTransfer.image               = swapchainImages_[imageIndex];
        toTransfer.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        fn_.vkCmdPipelineBarrier(commandBuffers_[frame_],
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

        // Clear swapchain image to solid black so no uninitialized contents appear
        VkClearColorValue blackColor = {};
        VkImageSubresourceRange clearRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        fn_.vkCmdClearColorImage(commandBuffers_[frame_],
                                 swapchainImages_[imageIndex],
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &blackColor, 1, &clearRange);

        // Transition swapchain image from TRANSFER_DST_OPTIMAL to PRESENT_SRC_KHR
        VkImageMemoryBarrier toPresent = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toPresent.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        toPresent.dstAccessMask       = 0;
        toPresent.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toPresent.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.image               = swapchainImages_[imageIndex];
        toPresent.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        fn_.vkCmdPipelineBarrier(commandBuffers_[frame_],
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toPresent);

        VkResult endRes = fn_.vkEndCommandBuffer(commandBuffers_[frame_]);
        if (endRes != VK_SUCCESS) {
            if (endRes == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            }
            resetFrameSyncObjects(frame_);
            imageAcquired_ = false;
            fenceReset_ = false;
            submissionQueued_ = false;
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

        VkResult submitRes = fn_.vkQueueSubmit(queue_, 1, &submit, inFlightFences_[frame_]);
        if (submitRes != VK_SUCCESS) {
            if (submitRes == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            }
            resetFrameSyncObjects(frame_);
            imageAcquired_ = false;
            fenceReset_ = false;
            submissionQueued_ = false;
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
        imageAcquired_ = false;
        submissionQueued_ = false;

        if (presented == VK_SUCCESS) {
            lastPresentResult_ = swapchainSuboptimal_ ? PresentResult::Suboptimal : PresentResult::Success;
            frame_ = (frame_ + 1) % kFramesInFlight;
            if (swapchainSuboptimal_) {
                swapchainSuboptimal_ = false;
                return false;
            }
            return true;
        }

        if (presented == VK_SUBOPTIMAL_KHR) {
            lastPresentResult_ = PresentResult::Suboptimal;
            swapchainSuboptimal_ = false;
            frame_ = (frame_ + 1) % kFramesInFlight;
            return false;
        }

        if (presented == VK_ERROR_OUT_OF_DATE_KHR) {
            lastPresentResult_ = PresentResult::OutOfDate;
            swapchainSuboptimal_ = false;
            frame_ = (frame_ + 1) % kFramesInFlight;
            return false;
        }

        if (presented == VK_ERROR_SURFACE_LOST_KHR) {
            lastPresentResult_ = PresentResult::SurfaceLost;
            surfaceLost_ = true;
            return false;
        }

        if (presented == VK_ERROR_DEVICE_LOST) {
            lastPresentResult_ = PresentResult::DeviceLost;
            deviceLost_ = true;
            return false;
        }

        // Other Vulkan failures:
        // Uncertain presentation state means synchronization objects cannot be safely reused.
        lastPresentResult_ = PresentResult::OtherError;
        recoveryFailed_ = true;
        return false;
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
    bool                     recoveryFailed_ = false;
    bool                     surfaceLost_ = false;
    PresentResult            lastPresentResult_ = PresentResult::Success;
    VulkanFunctions          fn_;
};

} // namespace arm
