// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT
#pragma once

#include "common.h"
#include "swapchain_sync.h"

#include <cstdint>
#include <string>
#include <vector>

struct ANativeWindow;

#ifdef DeviceCapabilities
#undef DeviceCapabilities
#endif

// Everything Arm ASR and FSR 3.1.4 need to know about the device. Checked once
// at startup and logged, because the prebuilt SPIR-V blobs shipped with the
// library are SPIR-V 1.4 with explicit 16-bit types and there is no fp32
// fallback permutation set.
struct DeviceCapabilities {
    uint32_t apiVersion                 = 0;
    bool     spirv14                    = false;  // Vulkan 1.2, or 1.1 + VK_KHR_spirv_1_4
    bool     shaderFloat16              = false;  // VK_KHR_shader_float16_int8
    bool     storage16bit               = false;  // VK_KHR_16bit_storage
    bool     subgroupSizeCtrl           = false;  // VK_EXT_subgroup_size_control (optional)
    bool     storageExtendedFormats     = false;  // shaderStorageImageExtendedFormats
    bool     storageWriteWithoutFormat  = false;  // shaderStorageImageWriteWithoutFormat
    bool     storageFormatsSupported    = false;  // R32F, RG16F, R32U, RGBA16F, R8
    bool     sampledFormatsSupported    = false;  // RGBA16F, D32F, RG16F

    bool sufficientForArmAsr() const {
        return spirv14 && shaderFloat16 && storage16bit &&
               storageExtendedFormats && storageWriteWithoutFormat &&
               storageFormatsSupported && sampledFormatsSupported;
    }
};

class VkContext {
public:
    static constexpr uint32_t kFramesInFlight = 2;

#if defined(FFXM_VK_CONTEXT_TESTING)
    struct TestSeam {
        bool failInstanceCreation        = false;
        bool failSurfaceCreation         = false;
        bool failPhysicalDevice          = false;
        bool failDeviceCreation          = false;
        bool failSwapchainCreation       = false;
        bool failFrameResources          = false;
        int  timestampValidBitsOverride  = -1;
        int  failSemaphoreIndex          = -1;
        int  failFenceIndex              = -1;

        uint32_t waitIdleCalls           = 0;
        uint32_t semaphoreDestructions   = 0;
        uint32_t fenceDestructions       = 0;
        uint32_t commandPoolDestructions = 0;
        uint32_t queryPoolDestructions   = 0;
        uint32_t imageViewDestructions   = 0;
        uint32_t swapchainDestructions   = 0;
        uint32_t deviceDestructions      = 0;
        uint32_t surfaceDestructions     = 0;
        uint32_t instanceDestructions    = 0;

        void resetCounters() {
            waitIdleCalls           = 0;
            semaphoreDestructions   = 0;
            fenceDestructions       = 0;
            commandPoolDestructions = 0;
            queryPoolDestructions   = 0;
            imageViewDestructions   = 0;
            swapchainDestructions   = 0;
            deviceDestructions      = 0;
            surfaceDestructions     = 0;
            instanceDestructions    = 0;
        }
    };

    void setTestSeam(TestSeam* seam) { testSeam_ = seam; }
    TestSeam* testSeam() const { return testSeam_; }
    bool isInitialized() const { return device_ != VK_NULL_HANDLE && swapchain_ != VK_NULL_HANDLE; }
#endif

    VkContext();
    ~VkContext();
    VkContext(const VkContext&) = delete;
    VkContext& operator=(const VkContext&) = delete;
    VkContext(VkContext&&) = delete;
    VkContext& operator=(VkContext&&) = delete;

    bool init(ANativeWindow* window);
    void destroy();

    // Acquires the next swapchain image and begins this frame's command buffer.
    // Returns false when the swapchain needs recreating or acquisition failed.
    bool beginFrame(uint32_t* outImageIndex);
    bool endFrame(uint32_t imageIndex);
    bool abortFrame(uint32_t imageIndex);

    bool recreateSwapchain(ANativeWindow* window);

    VkInstance       instance()       const { return instance_; }
    VkSurfaceKHR     surface()        const { return surface_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkDevice         device()         const { return device_; }
    VkQueue          queue()          const { return queue_; }
    uint32_t         queueFamily()    const { return queueFamily_; }
    VkSwapchainKHR   swapchain()      const { return swapchain_; }
    uint32_t         currentFrameSlot() const { return sync_.frame(); }
    VkCommandBuffer  commandBuffer()  const { return commandBuffers_[sync_.frame()]; }
    VkExtent2D       swapchainExtent()const { return swapchainExtent_; }
    VkFormat         swapchainFormat()const { return swapchainFormat_; }
    VkImage          swapchainImage(uint32_t i)     const { return swapchainImages_[i]; }
    VkImageView      swapchainImageView(uint32_t i) const { return swapchainImageViews_[i]; }
    uint32_t         swapchainImageCount()          const { return (uint32_t)swapchainImages_.size(); }
    bool             swapchainSuboptimal()          const { return sync_.swapchainSuboptimal(); }
    const DeviceCapabilities& caps() const { return caps_; }

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    bool     waitIdle() const;

    arm::SwapchainSync& sync() { return sync_; }
    const arm::SwapchainSync& sync() const { return sync_; }

    // GPU timestamp profiling support
    VkQueryPool queryPool(uint32_t frameSlot) const {
        return (frameSlot < kFramesInFlight) ? queryPools_[frameSlot] : VK_NULL_HANDLE;
    }
    uint32_t timestampValidBits() const { return timestampValidBits_; }
    float    timestampPeriod()    const { return timestampPeriod_; }
    const std::string& deviceName() const { return deviceName_; }
    uint32_t driverVersion()      const { return driverVersion_; }
    uint32_t apiVersion()         const { return apiVersion_; }

    VkResult getQueryPoolResults(uint32_t frameSlot, uint32_t firstQuery, uint32_t queryCount,
                                 size_t dataSize, void* data, VkDeviceSize stride,
                                 VkQueryResultFlags flags) const;
    void cmdResetQueryPool(VkCommandBuffer cmd, uint32_t frameSlot, uint32_t firstQuery, uint32_t queryCount) const;

private:
    bool createInstance();
    bool createSurface(ANativeWindow* window);
    bool pickPhysicalDevice();
    bool createDevice();
    bool createSwapchain();
    void destroySwapchain();
    bool createFrameResources();

#if defined(FFXM_VK_CONTEXT_TESTING)
    TestSeam*        testSeam_       = nullptr;
#endif
    VkInstance       instance_       = VK_NULL_HANDLE;
    VkSurfaceKHR     surface_        = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice         device_         = VK_NULL_HANDLE;
    VkQueue          queue_          = VK_NULL_HANDLE;
    uint32_t         queueFamily_    = 0;

    VkSwapchainKHR           swapchain_       = VK_NULL_HANDLE;
    VkFormat                 swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D               swapchainExtent_ = {0, 0};
    std::vector<VkImage>     swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;

    VkCommandPool   commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffers_[kFramesInFlight] = {};
    VkSemaphore     acquireSemaphores_[kFramesInFlight] = {};
    VkSemaphore     releaseSemaphores_[kFramesInFlight] = {};
    VkFence         inFlightFences_[kFramesInFlight] = {};

    VkQueryPool     queryPools_[kFramesInFlight] = {};
    uint32_t        timestampValidBits_ = 0;
    float           timestampPeriod_    = 0.0f;
    std::string     deviceName_;
    uint32_t        driverVersion_      = 0;
    uint32_t        apiVersion_         = 0;

    arm::SwapchainSync sync_;
    DeviceCapabilities caps_;
};
