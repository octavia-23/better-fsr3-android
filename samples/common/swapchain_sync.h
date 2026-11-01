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
/// Implements beginFrame, endFrame, and abortFrame with fence/semaphore lifecycle management.
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
#endif
    }

    void setVulkanFunctions(const VulkanFunctions& fn) { fn_ = fn; }
    VulkanFunctions& vulkanFunctions() { return fn_; }

    void init(VkDevice device, VkQueue queue, VkSwapchainKHR swapchain,
              const std::vector<VkImage>& swapchainImages,
              const VkCommandBuffer* commandBuffers,
              const VkSemaphore* acquireSemaphores,
              const VkSemaphore* releaseSemaphores,
              const VkFence* inFlightFences)
    {
        device_ = device;
        queue_ = queue;
        swapchain_ = swapchain;
        swapchainImages_ = swapchainImages;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            commandBuffers_[i] = commandBuffers[i];
            acquireSemaphores_[i] = acquireSemaphores[i];
            releaseSemaphores_[i] = releaseSemaphores[i];
            inFlightFences_[i] = inFlightFences[i];
        }
        frame_ = 0;
        swapchainSuboptimal_ = false;
    }

    void onSwapchainRecreated()
    {
        swapchainSuboptimal_ = false;
    }

    uint32_t frame() const { return frame_; }
    bool swapchainSuboptimal() const { return swapchainSuboptimal_; }

    bool beginFrame(uint32_t* outImageIndex)
    {
        if (fn_.vkWaitForFences(device_, 1, &inFlightFences_[frame_], VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            return false;
        }

        VkResult acquired = fn_.vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                                      acquireSemaphores_[frame_], VK_NULL_HANDLE,
                                                      outImageIndex);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            // No image acquired, semaphore not signalled, fence remains in signalled state.
            return false;
        }
        if (acquired == VK_SUBOPTIMAL_KHR) {
            // An image was successfully acquired and acquire semaphore will be signalled.
            // We must process this image and present it, but mark swapchain for recreation afterward.
            swapchainSuboptimal_ = true;
        } else if (acquired == VK_SUCCESS) {
            swapchainSuboptimal_ = false;
        } else {
            return false;
        }

        if (fn_.vkResetFences(device_, 1, &inFlightFences_[frame_]) != VK_SUCCESS) {
            return false;
        }
        if (fn_.vkResetCommandBuffer(commandBuffers_[frame_], 0) != VK_SUCCESS) {
            return false;
        }

        VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (fn_.vkBeginCommandBuffer(commandBuffers_[frame_], &beginInfo) != VK_SUCCESS) {
            return false;
        }
        return true;
    }

    bool endFrame(uint32_t imageIndex)
    {
        if (fn_.vkEndCommandBuffer(commandBuffers_[frame_]) != VK_SUCCESS) {
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
        if (fn_.vkQueueSubmit(queue_, 1, &submit, inFlightFences_[frame_]) != VK_SUCCESS) {
            return false;
        }

        VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores    = &releaseSemaphores_[frame_];
        present.swapchainCount     = 1;
        present.pSwapchains        = &swapchain_;
        present.pImageIndices      = &imageIndex;

        VkResult presented = fn_.vkQueuePresentKHR(queue_, &present);
        frame_ = (frame_ + 1) % kFramesInFlight;

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
            return false;
        }

        VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (fn_.vkBeginCommandBuffer(commandBuffers_[frame_], &beginInfo) != VK_SUCCESS) {
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
            return false;
        }

        // Present image to return ownership to the swapchain presentation engine
        VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores    = &releaseSemaphores_[frame_];
        present.swapchainCount     = 1;
        present.pSwapchains        = &swapchain_;
        present.pImageIndices      = &imageIndex;

        VkResult presented = fn_.vkQueuePresentKHR(queue_, &present);
        frame_ = (frame_ + 1) % kFramesInFlight;

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
    uint32_t                 frame_ = 0;
    bool                     swapchainSuboptimal_ = false;
    VulkanFunctions          fn_;
};

} // namespace arm
