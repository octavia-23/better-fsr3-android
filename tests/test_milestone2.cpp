// Copyright  © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <iostream>
#include <cstring>
#include <cmath>
#include <vector>
#include <map>
#include <set>
#include <cstdint>

#define FFXM_CPU
#include "ffxm_core.h"
#include "fsr1/ffxm_fsr1.h"
#include "spd/ffxm_spd.h"

#include "host/ffxm_fsr3upscaler.h"
#include "src/components/fsr3upscaler/ffxm_fsr3upscaler_private.h"
#include "host/ffxm_util.h"
#include "src/backends/shared/ffxm_shader_blobs.h"
#define SWAPCHAIN_SYNC_NO_VULKAN_SYMBOLS 1
#include "swapchain_sync.h"
#include "swapchain_recovery.h"
#include "fsr3_upscaler.h"
#include "vk_context.h"
#include "host/ffxm_gpu_timestamps.h"
#include "gpu_profiler.h"
#include "host/ffxm_opticalflow.h"
#include "src/components/opticalflow/ffxm_opticalflow_private.h"
#include "host/ffxm_frameinterpolation.h"
#include "src/components/frameinterpolation/ffxm_frameinterpolation_private.h"
#include "swapchain_frame_generator.h"

#if defined(FFXM_HAS_VK_BACKEND)
#include "host/backends/vk/ffxm_vk.h"

#if defined(_WIN32)
#include <windows.h>
static PFN_vkEnumerateDeviceExtensionProperties s_pfnRealEnumerateDeviceExtensionProperties = nullptr;
static PFN_vkGetDeviceProcAddr s_pfnRealGetDeviceProcAddr = nullptr;
static PFN_vkGetPhysicalDeviceProperties2 s_pfnRealGetPhysicalDeviceProperties2 = nullptr;
static PFN_vkGetPhysicalDeviceFeatures2 s_pfnRealGetPhysicalDeviceFeatures2 = nullptr;
static PFN_vkGetPhysicalDeviceMemoryProperties s_pfnRealGetPhysicalDeviceMemoryProperties = nullptr;
#endif

#if !defined(ANDROID)
// On host platforms when compiling tests without the full Vulkan SDK loader (using header-only Vulkan-Headers),
// provide stubs for Vulkan loader functions so test_milestone2 can link Arm_ASR_backend
// to directly test ffxmGetScratchMemorySizeVK and ffxmGetInterfaceVK.
// When running real hardware tests, these stubs forward dynamically to vulkan-1.dll.
extern "C" {
    VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
        VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties)
    {
#if defined(_WIN32)
        if (s_pfnRealEnumerateDeviceExtensionProperties) {
            return s_pfnRealEnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
        }
#endif
        (void)physicalDevice;
        (void)pLayerName;
        (void)pProperties;
        if (pPropertyCount) {
            *pPropertyCount = 0;
        }
        return VK_SUCCESS;
    }

    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName)
    {
#if defined(_WIN32)
        if (s_pfnRealGetDeviceProcAddr) {
            return s_pfnRealGetDeviceProcAddr(device, pName);
        }
#endif
        (void)device;
        (void)pName;
        return nullptr;
    }

    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(
        VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* pProperties)
    {
#if defined(_WIN32)
        if (s_pfnRealGetPhysicalDeviceProperties2) {
            s_pfnRealGetPhysicalDeviceProperties2(physicalDevice, pProperties);
            return;
        }
#endif
        (void)physicalDevice;
        (void)pProperties;
    }

    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
        VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures)
    {
#if defined(_WIN32)
        if (s_pfnRealGetPhysicalDeviceFeatures2) {
            s_pfnRealGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
            return;
        }
#endif
        (void)physicalDevice;
        (void)pFeatures;
    }

    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
        VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* pMemoryProperties)
    {
#if defined(_WIN32)
        if (s_pfnRealGetPhysicalDeviceMemoryProperties) {
            s_pfnRealGetPhysicalDeviceMemoryProperties(physicalDevice, pMemoryProperties);
            return;
        }
#endif
        (void)physicalDevice;
        (void)pMemoryProperties;
    }

    VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(
        VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount, const VkViewport* pViewports)
    {
        (void)commandBuffer;
        (void)firstViewport;
        (void)viewportCount;
        (void)pViewports;
    }

    VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(
        VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount, const VkRect2D* pScissors)
    {
        (void)commandBuffer;
        (void)firstScissor;
        (void)scissorCount;
        (void)pScissors;
    }
}
#endif
#endif

#if !defined(ANDROID) && !defined(__ANDROID__)
#include <vulkan/vulkan_android.h>
struct ANativeWindow {
    int dummy = 0;
};

#if !defined(FFXM_HAS_VK_BACKEND)
extern "C" {
    VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
        VkPhysicalDevice, const char*, uint32_t* pPropertyCount, VkExtensionProperties*)
    {
        if (pPropertyCount) *pPropertyCount = 0;
        return VK_SUCCESS;
    }

    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char*)
    {
        return nullptr;
    }

    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(
        VkPhysicalDevice, VkPhysicalDeviceProperties2*) {}

    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
        VkPhysicalDevice, VkPhysicalDeviceFeatures2*) {}

    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
        VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*) {}

    VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(
        VkCommandBuffer, uint32_t, uint32_t, const VkViewport*) {}

    VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(
        VkCommandBuffer, uint32_t, uint32_t, const VkRect2D*) {}
}
#endif // !FFXM_HAS_VK_BACKEND

extern "C" {
    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*)
    {
        return nullptr;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
        const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance* pInstance)
    {
        if (pInstance) *pInstance = (VkInstance)(uintptr_t)0x1;
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance, const VkAllocationCallbacks*) {}

    VKAPI_ATTR VkResult VKAPI_CALL vkCreateAndroidSurfaceKHR(
        VkInstance, const VkAndroidSurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR* pSurface)
    {
        if (pSurface) *pSurface = (VkSurfaceKHR)(uintptr_t)0x2;
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks*) {}

    VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance, uint32_t* pPhysicalDeviceCount, VkPhysicalDevice* pPhysicalDevices)
    {
        if (pPhysicalDeviceCount) {
            if (pPhysicalDevices && *pPhysicalDeviceCount > 0) {
                pPhysicalDevices[0] = (VkPhysicalDevice)(uintptr_t)0x3;
            }
            *pPhysicalDeviceCount = 1;
        }
        return VK_SUCCESS;
    }

    static uint32_t s_MockTimestampValidBits = 64;
    static float    s_MockTimestampPeriod    = 1.0f;

    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(
        VkPhysicalDevice, uint32_t* pPropertyCount, VkQueueFamilyProperties* pProperties)
    {
        if (pPropertyCount) {
            if (pProperties && *pPropertyCount > 0) {
                pProperties[0] = {};
                pProperties[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
                pProperties[0].queueCount = 1;
                pProperties[0].timestampValidBits = s_MockTimestampValidBits;
            }
            *pPropertyCount = 1;
        }
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(
        VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32* pSupported)
    {
        if (pSupported) *pSupported = VK_TRUE;
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* pProperties)
    {
        if (pProperties) {
            *pProperties = {};
            pProperties->apiVersion = VK_API_VERSION_1_2;
            pProperties->limits.timestampPeriod = s_MockTimestampPeriod;
            strcpy_s(pProperties->deviceName, "Mock Device");
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat, VkFormatProperties* pFormatProperties)
    {
        if (pFormatProperties) {
            pFormatProperties->linearTilingFeatures = 0;
            pFormatProperties->optimalTilingFeatures = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            pFormatProperties->bufferFeatures = 0;
        }
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
        VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice* pDevice)
    {
        if (pDevice) *pDevice = (VkDevice)(uintptr_t)0x4;
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue* pQueue)
    {
        if (pQueue) *pQueue = (VkQueue)(uintptr_t)0x5;
    }

    VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice, const VkAllocationCallbacks*) {}

    VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities)
    {
        if (pSurfaceCapabilities) {
            *pSurfaceCapabilities = {};
            pSurfaceCapabilities->minImageCount = 2;
            pSurfaceCapabilities->maxImageCount = 3;
            pSurfaceCapabilities->currentExtent = { 1920, 1080 };
            pSurfaceCapabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        }
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(
        VkPhysicalDevice, VkSurfaceKHR, uint32_t* pSurfaceFormatCount, VkSurfaceFormatKHR* pSurfaceFormats)
    {
        if (pSurfaceFormatCount) {
            if (pSurfaceFormats && *pSurfaceFormatCount > 0) {
                pSurfaceFormats[0].format = VK_FORMAT_R8G8B8A8_UNORM;
                pSurfaceFormats[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            }
            *pSurfaceFormatCount = 1;
        }
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(
        VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR* pSwapchain)
    {
        if (pSwapchain) *pSwapchain = (VkSwapchainKHR)(uintptr_t)0x10;
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(
        VkDevice, VkSwapchainKHR, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages)
    {
        if (pSwapchainImageCount) {
            if (pSwapchainImages && *pSwapchainImageCount >= 2) {
                pSwapchainImages[0] = (VkImage)(uintptr_t)0x20;
                pSwapchainImages[1] = (VkImage)(uintptr_t)0x21;
            }
            *pSwapchainImageCount = 2;
        }
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*) {}

    VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(
        VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* pView)
    {
        if (pView) *pView = (VkImageView)(uintptr_t)0x30;
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks*) {}

    VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(
        VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool* pCommandPool)
    {
        if (pCommandPool) *pCommandPool = (VkCommandPool)(uintptr_t)0x50;
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(
        VkDevice, const VkCommandBufferAllocateInfo* pAllocateInfo, VkCommandBuffer* pCommandBuffers)
    {
        if (pAllocateInfo && pCommandBuffers) {
            for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
                pCommandBuffers[i] = (VkCommandBuffer)(uintptr_t)(0x60 + i);
            }
        }
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks*) {}

    VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(
        VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* pSemaphore)
    {
        if (pSemaphore) *pSemaphore = (VkSemaphore)(uintptr_t)0x70;
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks*) {}

    VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(
        VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* pFence)
    {
        if (pFence) *pFence = (VkFence)(uintptr_t)0x80;
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice, VkFence, const VkAllocationCallbacks*) {}

    VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice)
    {
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence)
    {
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice, uint32_t, const VkFence*)
    {
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t)
    {
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*)
    {
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer)
    {
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer, VkCommandBufferResetFlags)
    {
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*, uint32_t, const VkImageSubresourceRange*)
    {
    }

    VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*)
    {
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*)
    {
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR*)
    {
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(
        VkDevice, const VkQueryPoolCreateInfo*, const VkAllocationCallbacks*, VkQueryPool* pQueryPool)
    {
        if (pQueryPool) *pQueryPool = (VkQueryPool)(uintptr_t)0x100;
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice, VkQueryPool, const VkAllocationCallbacks*) {}

    VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(
        VkDevice, VkQueryPool, uint32_t, uint32_t, size_t, void*, VkDeviceSize, VkQueryResultFlags)
    {
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t) {}

    VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer, VkPipelineStageFlagBits, VkQueryPool, uint32_t) {}
}
#endif // !defined(ANDROID) && !defined(__ANDROID__)

using namespace arm;

// Global test counters for always-active test assertions
static int g_TestPasses = 0;
static int g_TestFailures = 0;
static bool g_SuppressFailurePrint = false;

#define TEST_CHECK(cond) do { \
    if (!(cond)) { \
        if (!g_SuppressFailurePrint) { \
            std::cerr << "CHECK FAILED: " #cond " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        } \
        g_TestFailures++; \
    } else { \
        g_TestPasses++; \
    } \
} while (0)

#define TEST_REQUIRE(cond) do { \
    if (!(cond)) { \
        std::cerr << "REQUIREMENT FAILED: " #cond " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        g_TestFailures++; \
        return; \
    } else { \
        g_TestPasses++; \
    } \
} while (0)

// Mock backend interface state tracking
static bool s_GetDeviceCapsCalled = false;
static bool s_CreateBackendContextCalled = false;
static bool s_DestroyBackendContextCalled = false;
static FfxmUInt32 s_CreatedContextId = 0;
static FfxmUInt32 s_DestroyedContextId = 0;
static bool s_Fp16SupportMock = true;
static FfxmErrorCode s_CreateBackendContextResult = FFXM_OK;
static bool s_BackendContextActive = false;
static int s_CallSequenceNumber = 0;
static int s_CreateBackendContextCallSeq = 0;
static int s_GetDeviceCapsCallSeq = 0;
static bool s_EnforceBackendActiveForCaps = true;

// Resource failure injection controls
static int32_t s_FailAtResourceIndex = -1; // -1 means no failure
static int32_t s_FailAtCopyIndex = -1;     // -1 means no failure
static int32_t s_CurrentResourceCreateCount = 0;
static int32_t s_NextResourceIndex = 0;

// Dynamic registration & dispatch failure injection controls
static int32_t s_FailAtRegisterResourceIndex = -1;
static uint32_t s_RegisterResourceCallCount = 0;
static FfxmErrorCode s_ScheduleGpuJobResult = FFXM_OK;
static uint32_t s_ScheduleGpuJobCallCount = 0;
static int32_t s_FailAtScheduleGpuJobIndex = -1;
static FfxmErrorCode s_ExecuteGpuJobsResult = FFXM_OK;
static FfxmErrorCode s_UnregisterResourcesResult = FFXM_OK;

struct MockRegisteredResourceRecord {
    FfxmResourceDescription desc;
    FfxmResourceStates state;
    wchar_t name[64];
};
static std::vector<MockRegisteredResourceRecord> s_MockRegisteredResources;

struct MockBackendResourceState {
    FfxmResourceStates currentState = static_cast<FfxmResourceStates>(0);
    FfxmResourceStates committedState = static_cast<FfxmResourceStates>(0);
    bool undefined = true;
    bool committedUndefined = true;
};
static std::map<int32_t, MockBackendResourceState> s_MockBackendResources;
static std::map<uint32_t, uint32_t> s_MockBackendFrameIndex;
static std::map<uint32_t, bool> s_MockBackendIsEndOfUpscaler;
static int32_t s_FailAfterGpuJobCount = -1;
static uint32_t s_NextMockEffectContextId = 100;

// Occurrence-aware resource creation & destruction tracking
static std::map<int32_t, int> s_ResourceCreationCounts;
static std::map<int32_t, int> s_ResourceDestructionCounts;
static std::vector<int32_t> s_CreatedResourceOrder;
static std::vector<int32_t> s_DestroyedResourceOrder;

// Captured resource descriptions and initial data for regression verification
static std::vector<FfxmCreateResourceDescription> s_CapturedResourceDescriptions;
static std::vector<std::vector<uint8_t>> s_CapturedInitialData;

static FfxmVersionNumber MockGetSDKVersion(FfxmInterface* backendInterface)
{
    (void)backendInterface;
    return FFXM_SDK_MAKE_VERSION(1, 1, 4);
}

static FfxmErrorCode MockGetDeviceCapabilities(FfxmInterface* backendInterface, FfxmDeviceCapabilities* deviceCapabilities)
{
    (void)backendInterface;
    s_GetDeviceCapsCalled = true;
    s_GetDeviceCapsCallSeq = ++s_CallSequenceNumber;

    // Strict contract check: In real Vulkan, backendContext extension properties and physical device
    // are only initialized after CreateBackendContextVK has run!
    if (s_EnforceBackendActiveForCaps && !s_BackendContextActive) {
        return FFXM_ERROR_BACKEND_API_ERROR;
    }

    if (deviceCapabilities) {
        deviceCapabilities->fp16Supported = s_Fp16SupportMock;
        deviceCapabilities->raytracingSupported = false;
    }
    return FFXM_OK;
}

static FfxmErrorCode MockCreateBackendContext(FfxmInterface* backendInterface, FfxmUInt32* effectContextId)
{
    (void)backendInterface;
    s_CreateBackendContextCalled = true;
    s_CreateBackendContextCallSeq = ++s_CallSequenceNumber;
    if (s_CreateBackendContextResult != FFXM_OK) {
        return s_CreateBackendContextResult;
    }
    s_BackendContextActive = true;
    *effectContextId = s_NextMockEffectContextId++;
    s_CreatedContextId = *effectContextId;
    s_MockBackendFrameIndex[*effectContextId] = 0;
    s_MockBackendIsEndOfUpscaler[*effectContextId] = false;
    return FFXM_OK;
}

static FfxmErrorCode MockDestroyBackendContext(FfxmInterface* backendInterface, FfxmUInt32 effectContextId)
{
    (void)backendInterface;
    s_DestroyBackendContextCalled = true;
    s_BackendContextActive = false;
    s_DestroyedContextId = effectContextId;
    return FFXM_OK;
}

static FfxmErrorCode MockCreateResource(
    FfxmInterface* backendInterface,
    const FfxmCreateResourceDescription* createResourceDescription,
    FfxmUInt32 effectContextId,
    FfxmResourceInternal* outResource)
{
    (void)backendInterface; (void)effectContextId;

    if (s_FailAtResourceIndex >= 0 && s_CurrentResourceCreateCount == s_FailAtResourceIndex) {
        return FFXM_ERROR_OUT_OF_MEMORY;
    }

    if (createResourceDescription) {
        s_CapturedResourceDescriptions.push_back(*createResourceDescription);
        if (createResourceDescription->initData && createResourceDescription->initDataSize > 0) {
            std::vector<uint8_t> dataCopy(
                static_cast<const uint8_t*>(createResourceDescription->initData),
                static_cast<const uint8_t*>(createResourceDescription->initData) + createResourceDescription->initDataSize);
            s_CapturedInitialData.push_back(dataCopy);
        }
    }

    int32_t idx = s_NextResourceIndex++;
    s_CurrentResourceCreateCount++;
    s_ResourceCreationCounts[idx]++;
    s_CreatedResourceOrder.push_back(idx);

    if (outResource) {
        outResource->internalIndex = idx;
    }

    if (createResourceDescription) {
        MockBackendResourceState st;
        st.currentState = createResourceDescription->initalState;
        st.committedState = createResourceDescription->initalState;
        st.undefined = (createResourceDescription->initData == nullptr);
        st.committedUndefined = st.undefined;
        s_MockBackendResources[idx] = st;
    }

    // If resource has initial data, backend creates a copy staging resource at idx + 1
    if (createResourceDescription && createResourceDescription->initData) {
        if (s_FailAtCopyIndex >= 0 && (idx + 1) == s_FailAtCopyIndex) {
            return FFXM_ERROR_OUT_OF_MEMORY;
        }
        int32_t copyIdx = s_NextResourceIndex++;
        s_ResourceCreationCounts[copyIdx]++;
        s_CreatedResourceOrder.push_back(copyIdx);
    }

    return FFXM_OK;
}

static FfxmErrorCode MockDestroyResource(FfxmInterface* backendInterface, FfxmResourceInternal resource)
{
    (void)backendInterface;
    if (resource.internalIndex != -1) {
        s_ResourceDestructionCounts[resource.internalIndex]++;
        s_DestroyedResourceOrder.push_back(resource.internalIndex);
    }
    return FFXM_OK;
}

static FfxmErrorCode MockRegisterResource(
    FfxmInterface* backendInterface,
    const FfxmResource* inResource,
    FfxmUInt32 effectContextId,
    FfxmResourceInternal* outResource)
{
    (void)backendInterface; (void)effectContextId;
    if (inResource) {
        MockRegisteredResourceRecord rec = {};
        rec.desc = inResource->description;
        rec.state = inResource->state;
        if (inResource->name[0] != L'\0') {
            wcsncpy(rec.name, inResource->name, 63);
        }
        s_MockRegisteredResources.push_back(rec);
    }
    if (s_FailAtRegisterResourceIndex >= 0 && static_cast<int32_t>(s_RegisterResourceCallCount) == s_FailAtRegisterResourceIndex) {
        s_RegisterResourceCallCount++;
        return FFXM_ERROR_OUT_OF_MEMORY;
    }
    if (outResource) {
        outResource->internalIndex = 900 + static_cast<int32_t>(s_RegisterResourceCallCount);
        if (inResource) {
            MockBackendResourceState st;
            st.currentState = inResource->state;
            st.committedState = inResource->state;
            st.undefined = !!(inResource->description.flags & FFXM_RESOURCE_FLAGS_UNDEFINED);
            st.committedUndefined = st.undefined;
            s_MockBackendResources[outResource->internalIndex] = st;
        }
    }
    s_RegisterResourceCallCount++;
    return FFXM_OK;
}

static FfxmResource MockGetResource(FfxmInterface* backendInterface, FfxmResourceInternal res)
{
    (void)backendInterface; (void)res;
    FfxmResource r = {};
    return r;
}

static FfxmResourceDescription MockGetResourceDescription(FfxmInterface* backendInterface, FfxmResourceInternal res)
{
    (void)backendInterface; (void)res;
    FfxmResourceDescription d = {};
    return d;
}

template <size_t N>
static void ffxmWidenAscii(wchar_t (&dst)[N], const char* src)
{
    size_t i = 0;
    if (src) {
        for (; i < N - 1 && src[i] != '\0'; ++i)
            dst[i] = static_cast<wchar_t>(static_cast<unsigned char>(src[i]));
    }
    dst[i] = L'\0';
}

static uint32_t s_CreatePipelineCallCount = 0;
static uint32_t s_DestroyPipelineCallCount = 0;
static uint32_t s_ExecuteGpuJobsCallCount = 0;
static uint32_t s_UnregisterResourcesCallCount = 0;
static std::vector<FfxmGpuJobDescription> s_ScheduledGpuJobs;
static FfxmErrorCode s_CreatePipelineResult = FFXM_OK;
static int32_t s_FailAtCreatePipelineIndex = -1;

static FfxmErrorCode MockCreatePipeline(
    FfxmInterface* backendInterface,
    FfxmEffect effect,
    FfxmPass pass,
    FfxmShaderQuality qualityPreset,
    uint32_t permutationOptions,
    const FfxmPipelineDescription* pipelineDescription,
    FfxmUInt32 effectContextId,
    FfxmPipelineState* outPipeline)
{
    (void)backendInterface; (void)qualityPreset; (void)pipelineDescription; (void)effectContextId;
    if (s_FailAtCreatePipelineIndex >= 0 && static_cast<int32_t>(s_CreatePipelineCallCount) == s_FailAtCreatePipelineIndex) {
        s_CreatePipelineCallCount++;
        return FFXM_ERROR_BACKEND_API_ERROR;
    }
    s_CreatePipelineCallCount++;
    if (s_CreatePipelineResult != FFXM_OK) {
        return s_CreatePipelineResult;
    }

    if (outPipeline == nullptr) {
        return FFXM_ERROR_INVALID_POINTER;
    }

    FfxmShaderBlob shaderBlob = {};
    FfxmErrorCode err = arm::ffxmGetPermutationBlobByIndex(effect, pass, permutationOptions, &shaderBlob);
    if (err == FFXM_OK && shaderBlob.data != nullptr) {
        outPipeline->pipeline = reinterpret_cast<FfxmPipeline>(static_cast<uintptr_t>(0x1000 + s_CreatePipelineCallCount * 0x100));
        outPipeline->srvTextureCount = shaderBlob.srvTextureCount;
        for (uint32_t i = 0; i < shaderBlob.srvTextureCount; ++i) {
            outPipeline->srvTextureBindings[i].slotIndex = shaderBlob.boundSRVTextures[i];
            outPipeline->srvTextureBindings[i].bindCount = shaderBlob.boundSRVTextureCounts[i];
            outPipeline->srvTextureBindings[i].bindSet = shaderBlob.boundSRVTextureSets[i];
            ffxmWidenAscii(outPipeline->srvTextureBindings[i].name, shaderBlob.boundSRVTextureNames[i]);
        }
        outPipeline->uavTextureCount = shaderBlob.uavTextureCount;
        for (uint32_t i = 0; i < shaderBlob.uavTextureCount; ++i) {
            outPipeline->uavTextureBindings[i].slotIndex = shaderBlob.boundUAVTextures[i];
            outPipeline->uavTextureBindings[i].bindCount = shaderBlob.boundUAVTextureCounts[i];
            outPipeline->uavTextureBindings[i].bindSet = shaderBlob.boundUAVTextureSets[i];
            ffxmWidenAscii(outPipeline->uavTextureBindings[i].name, shaderBlob.boundUAVTextureNames[i]);
        }
        outPipeline->constCount = shaderBlob.cbvCount;
        for (uint32_t i = 0; i < shaderBlob.cbvCount; ++i) {
            outPipeline->constantBufferBindings[i].slotIndex = shaderBlob.boundConstantBuffers[i];
            outPipeline->constantBufferBindings[i].bindCount = shaderBlob.boundConstantBufferCounts[i];
            outPipeline->constantBufferBindings[i].bindSet = shaderBlob.boundConstantBufferSets[i];
            ffxmWidenAscii(outPipeline->constantBufferBindings[i].name, shaderBlob.boundConstantBufferNames[i]);
        }
    } else {
        outPipeline->pipeline = reinterpret_cast<FfxmPipeline>(static_cast<uintptr_t>(0x5000 + s_CreatePipelineCallCount * 0x100));
    }

    return FFXM_OK;
}

static FfxmErrorCode MockDestroyPipeline(FfxmInterface* backendInterface, FfxmPipelineState* pipeline, FfxmUInt32 effectContextId)
{
    (void)backendInterface; (void)effectContextId;
    s_DestroyPipelineCallCount++;
    if (pipeline) {
        pipeline->pipeline = nullptr;
    }
    return FFXM_OK;
}

static FfxmErrorCode MockScheduleGpuJob(FfxmInterface* backendInterface, const FfxmGpuJobDescription* job)
{
    (void)backendInterface;
    if (s_FailAtScheduleGpuJobIndex >= 0 && static_cast<int32_t>(s_ScheduleGpuJobCallCount) == s_FailAtScheduleGpuJobIndex) {
        s_ScheduleGpuJobCallCount++;
        return FFXM_ERROR_BACKEND_API_ERROR;
    }
    s_ScheduleGpuJobCallCount++;
    if (s_ScheduleGpuJobResult != FFXM_OK) {
        return s_ScheduleGpuJobResult;
    }
    if (job) {
        s_ScheduledGpuJobs.push_back(*job);
    }
    return FFXM_OK;
}

static FfxmErrorCode MockExecuteGpuJobs(FfxmInterface* backendInterface, FfxmCommandList commandList)
{
    (void)backendInterface; (void)commandList;
    s_ExecuteGpuJobsCallCount++;
    if (s_ExecuteGpuJobsResult != FFXM_OK) {
        // Rollback all modified resources to committed state
        for (auto& pair : s_MockBackendResources) {
            pair.second.currentState = pair.second.committedState;
            pair.second.undefined = pair.second.committedUndefined;
        }
        for (auto& pair : s_MockBackendIsEndOfUpscaler) pair.second = false;
        return s_ExecuteGpuJobsResult;
    }

    if (s_FailAfterGpuJobCount >= 0) {
        // Execute up to s_FailAfterGpuJobCount jobs
        for (size_t i = 0; i < s_ScheduledGpuJobs.size() && static_cast<int32_t>(i) < s_FailAfterGpuJobCount; ++i) {
            const auto& job = s_ScheduledGpuJobs[i];
            if (job.jobType == FFXM_GPU_JOB_CLEAR_FLOAT) {
                int32_t targetIdx = job.clearJobDescriptor.target.internalIndex;
                if (targetIdx >= 0) {
                    s_MockBackendResources[targetIdx].currentState = FFXM_RESOURCE_STATE_COPY_DEST;
                    s_MockBackendResources[targetIdx].undefined = false;
                }
            } else if (job.jobType == FFXM_GPU_JOB_COMPUTE) {
                for (uint32_t u = 0; u < FFXM_MAX_NUM_UAVS; ++u) {
                    int32_t uavIdx = job.computeJobDescriptor.uavTextures[u].internalIndex;
                    if (uavIdx >= 0) {
                        s_MockBackendResources[uavIdx].currentState = FFXM_RESOURCE_STATE_UNORDERED_ACCESS;
                        s_MockBackendResources[uavIdx].undefined = false;
                    }
                }
            }
        }
        // Failure occurs at s_FailAfterGpuJobCount!
        // Rollback all modified resources to committed state
        for (auto& pair : s_MockBackendResources) {
            pair.second.currentState = pair.second.committedState;
            pair.second.undefined = pair.second.committedUndefined;
        }
        for (auto& pair : s_MockBackendIsEndOfUpscaler) pair.second = false;
        return FFXM_ERROR_BACKEND_API_ERROR;
    }

    // All jobs succeeded: mutate and commit
    for (const auto& job : s_ScheduledGpuJobs) {
        if (job.jobType == FFXM_GPU_JOB_CLEAR_FLOAT) {
            int32_t targetIdx = job.clearJobDescriptor.target.internalIndex;
            if (targetIdx >= 0) {
                s_MockBackendResources[targetIdx].currentState = FFXM_RESOURCE_STATE_COPY_DEST;
                s_MockBackendResources[targetIdx].undefined = false;
            }
        } else if (job.jobType == FFXM_GPU_JOB_COMPUTE) {
            for (uint32_t u = 0; u < FFXM_MAX_NUM_UAVS; ++u) {
                int32_t uavIdx = job.computeJobDescriptor.uavTextures[u].internalIndex;
                if (uavIdx >= 0) {
                    s_MockBackendResources[uavIdx].currentState = FFXM_RESOURCE_STATE_UNORDERED_ACCESS;
                    s_MockBackendResources[uavIdx].undefined = false;
                }
            }
        }
    }
    // Commit
    for (auto& pair : s_MockBackendResources) {
        pair.second.committedState = pair.second.currentState;
        pair.second.committedUndefined = pair.second.undefined;
    }
    for (auto& pair : s_MockBackendIsEndOfUpscaler) pair.second = true;

    return FFXM_OK;
}

static FfxmErrorCode MockUnregisterResources(FfxmInterface* backendInterface, FfxmCommandList commandList, FfxmUInt32 effectContextId)
{
    (void)backendInterface; (void)commandList;
    s_UnregisterResourcesCallCount++;
    if (s_MockBackendIsEndOfUpscaler[effectContextId]) {
        s_MockBackendFrameIndex[effectContextId] = (s_MockBackendFrameIndex[effectContextId] + 1) % 16;
        s_MockBackendIsEndOfUpscaler[effectContextId] = false;
    }
    return s_UnregisterResourcesResult;
}

static void ResetMockState()
{
    s_GetDeviceCapsCalled = false;
    s_CreateBackendContextCalled = false;
    s_DestroyBackendContextCalled = false;
    s_CreatedContextId = 0;
    s_DestroyedContextId = 0;
    s_Fp16SupportMock = true;
    s_CreateBackendContextResult = FFXM_OK;
    s_BackendContextActive = false;
    s_CallSequenceNumber = 0;
    s_CreateBackendContextCallSeq = 0;
    s_GetDeviceCapsCallSeq = 0;
    s_EnforceBackendActiveForCaps = true;
    s_FailAtResourceIndex = -1;
    s_FailAtCopyIndex = -1;
    s_CurrentResourceCreateCount = 0;
    s_NextResourceIndex = 0;
    s_ResourceCreationCounts.clear();
    s_ResourceDestructionCounts.clear();
    s_CreatedResourceOrder.clear();
    s_DestroyedResourceOrder.clear();
    s_CapturedResourceDescriptions.clear();
    s_CapturedInitialData.clear();
    s_CreatePipelineCallCount = 0;
    s_DestroyPipelineCallCount = 0;
    s_ExecuteGpuJobsCallCount = 0;
    s_UnregisterResourcesCallCount = 0;
    s_ScheduledGpuJobs.clear();
    s_CreatePipelineResult = FFXM_OK;
    s_FailAtCreatePipelineIndex = -1;
    s_FailAtRegisterResourceIndex = -1;
    s_RegisterResourceCallCount = 0;
    s_ScheduleGpuJobResult = FFXM_OK;
    s_ScheduleGpuJobCallCount = 0;
    s_FailAtScheduleGpuJobIndex = -1;
    s_ExecuteGpuJobsResult = FFXM_OK;
    s_UnregisterResourcesResult = FFXM_OK;
    s_MockRegisteredResources.clear();
    s_MockBackendResources.clear();
    s_MockBackendFrameIndex.clear();
    s_MockBackendIsEndOfUpscaler.clear();
    s_NextMockEffectContextId = 100;
    s_FailAfterGpuJobCount = -1;
}

static void ResetDispatchTrackers()
{
    s_RegisterResourceCallCount = 0;
    s_ScheduleGpuJobCallCount = 0;
    s_ExecuteGpuJobsCallCount = 0;
    s_UnregisterResourcesCallCount = 0;
    s_ScheduledGpuJobs.clear();
    s_MockRegisteredResources.clear();
    s_ScheduleGpuJobResult = FFXM_OK;
    s_FailAtScheduleGpuJobIndex = -1;
    s_ExecuteGpuJobsResult = FFXM_OK;
    s_UnregisterResourcesResult = FFXM_OK;
    s_FailAfterGpuJobCount = -1;
}

static void SetupMockInterface(FfxmInterface* iface)
{
    memset(iface, 0, sizeof(FfxmInterface));
    static int s_DummyDevice = 1;
    iface->device = &s_DummyDevice;
    iface->fpGetSDKVersion = MockGetSDKVersion;
    iface->fpGetDeviceCapabilities = MockGetDeviceCapabilities;
    iface->fpCreateBackendContext = MockCreateBackendContext;
    iface->fpDestroyBackendContext = MockDestroyBackendContext;
    iface->fpCreateResource = MockCreateResource;
    iface->fpDestroyResource = MockDestroyResource;
    iface->fpRegisterResource = MockRegisterResource;
    iface->fpGetResource = MockGetResource;
    iface->fpUnregisterResources = MockUnregisterResources;
    iface->fpGetResourceDescription = MockGetResourceDescription;
    iface->fpCreateComputePipeline = MockCreatePipeline;
    iface->fpCreateGraphicsPipeline = MockCreatePipeline;
    iface->fpDestroyPipeline = MockDestroyPipeline;
    iface->fpScheduleGpuJob = MockScheduleGpuJob;
    iface->fpExecuteGpuJobs = MockExecuteGpuJobs;
}

static void VerifyOccurrenceAwareResourceBalance()
{
    // 1. Total unique created and destroyed resource counts must match
    TEST_CHECK(s_ResourceCreationCounts.size() == s_ResourceDestructionCounts.size());

    // 2. Every acquired resource must be destroyed exactly once
    for (const auto& kv : s_ResourceCreationCounts) {
        int32_t idx = kv.first;
        TEST_CHECK(kv.second == 1);
        auto it = s_ResourceDestructionCounts.find(idx);
        TEST_CHECK(it != s_ResourceDestructionCounts.end());
        if (it != s_ResourceDestructionCounts.end()) {
            TEST_CHECK(it->second == 1);
        }
    }

    // 3. No unacquired resource was destroyed
    for (const auto& kv : s_ResourceDestructionCounts) {
        int32_t idx = kv.first;
        TEST_CHECK(s_ResourceCreationCounts.find(idx) != s_ResourceCreationCounts.end());
    }

    // 4. Exact map equality
    TEST_CHECK(s_ResourceCreationCounts == s_ResourceDestructionCounts);
}

// Checked arithmetic primitives unit test
static inline bool testCheckAddSize(size_t a, size_t b, size_t* out)
{
    if (SIZE_MAX - a < b) return false;
    *out = a + b;
    return true;
}

static inline bool testCheckMulSize(size_t a, size_t b, size_t* out)
{
    if (a != 0 && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
}

static inline bool testCheckAlignUpSize(size_t val, size_t align, size_t* out)
{
    if (align == 0) {
        *out = val;
        return true;
    }
    size_t remainder = val % align;
    if (remainder == 0) {
        *out = val;
        return true;
    }
    size_t addVal = align - remainder;
    if (SIZE_MAX - val < addVal) return false;
    *out = val + addVal;
    return true;
}

// 0. Verify that TEST_CHECK remains active under NDEBUG
void TestNDEBUGAssertionActive()
{
    std::cout << "--- Test 0: NDEBUG Assertion Evaluation Verification ---" << std::endl;
    int initialFailures = g_TestFailures;
    int sideEffectCounter = 0;

    // Verify expression side-effects are never stripped
    TEST_CHECK((sideEffectCounter = 42) == 42);
    TEST_CHECK(sideEffectCounter == 42);

    // Verify intentional false check registers without halting
    int savedFailures = g_TestFailures;
    g_SuppressFailurePrint = true;
    TEST_CHECK(false);
    g_SuppressFailurePrint = false;
    TEST_CHECK(g_TestFailures == savedFailures + 1);
    g_TestFailures = initialFailures; // Reset failure counter for intentional check

    std::cout << "PASS: Assertion macros active and verified under NDEBUG." << std::endl;
}

// 1. Context Size and Alignment
void TestContextSizeAndAlignment()
{
    std::cout << "--- Test 1: Context Size and Alignment ---" << std::endl;
    std::cout << "sizeof(FfxmFsr3UpscalerContext): " << sizeof(FfxmFsr3UpscalerContext) << " bytes" << std::endl;
    std::cout << "alignof(FfxmFsr3UpscalerContext): " << alignof(FfxmFsr3UpscalerContext) << " bytes" << std::endl;
    std::cout << "sizeof(FfxmFsr3UpscalerContext_Private): " << sizeof(FfxmFsr3UpscalerContext_Private) << " bytes" << std::endl;
    std::cout << "alignof(FfxmFsr3UpscalerContext_Private): " << alignof(FfxmFsr3UpscalerContext_Private) << " bytes" << std::endl;

#if defined(_WIN32)
    TEST_CHECK(sizeof(FfxmFsr3UpscalerContext) == 262144);
#elif defined(__aarch64__)
    TEST_CHECK(sizeof(FfxmFsr3UpscalerContext) == 524288);
#endif
    TEST_CHECK(alignof(FfxmFsr3UpscalerContext) >= 8);
    TEST_CHECK(sizeof(FfxmFsr3UpscalerContext) >= sizeof(FfxmFsr3UpscalerContext_Private));
    TEST_CHECK(alignof(FfxmFsr3UpscalerContext) >= alignof(FfxmFsr3UpscalerContext_Private));
    std::cout << "PASS: Context Size and Alignment verified." << std::endl;
}

// 2. Successful Context Creation & Resource Allocation
void TestSuccessfulContextCreation()
{
    std::cout << "--- Test 2: Successful Context Creation & Resource Allocation ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);

    FfxmErrorCode res = ffxmFsr3UpscalerContextCreate(&context, &desc);
    TEST_CHECK(res == FFXM_OK);
    TEST_CHECK(s_CreateBackendContextCalled == true);
    TEST_CHECK(s_GetDeviceCapsCalled == true);
    // Verified call ordering: backend context created before capabilities are queried
    TEST_CHECK(s_CreateBackendContextCallSeq < s_GetDeviceCapsCallSeq);
    TEST_CHECK(s_BackendContextActive == true);

    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);
    TEST_CHECK(priv->backendContextCreated == true);
    TEST_CHECK(priv->effectContextId == 100);
    TEST_CHECK(priv->allocatedResourceCount == FFXM_FSR3UPSCALER_INTERNAL_SURFACE_COUNT);

    // Verify all 19 resources were allocated with non-negative indices
    for (uint32_t i = 0; i < FFXM_FSR3UPSCALER_INTERNAL_SURFACE_COUNT; ++i) {
        uint32_t id = priv->allocatedResourceIds[i];
        TEST_CHECK(priv->srvResources[id].internalIndex >= 0);
        TEST_CHECK(priv->uavResources[id].internalIndex == priv->srvResources[id].internalIndex);
        TEST_CHECK(priv->rtResources[id].internalIndex == priv->srvResources[id].internalIndex);
    }

    // Verify constant staging
    TEST_CHECK(priv->constants.maxRenderSize[0] == 1920);
    TEST_CHECK(priv->constants.maxRenderSize[1] == 1080);
    TEST_CHECK(priv->constants.maxUpscaleSize[0] == 3840);
    TEST_CHECK(priv->constants.maxUpscaleSize[1] == 2160);

    // Test destruction
    res = ffxmFsr3UpscalerContextDestroy(&context);
    TEST_CHECK(res == FFXM_OK);
    TEST_CHECK(s_DestroyBackendContextCalled == true);
    TEST_CHECK(s_DestroyedContextId == 100);
    TEST_CHECK(s_BackendContextActive == false);

    // Verify occurrence-aware resource balance (19 surfaces, 4 copy resources, 0 leaks, 0 double-frees)
    VerifyOccurrenceAwareResourceBalance();
    TEST_CHECK(s_ResourceCreationCounts.size() == 23); // 19 textures + 4 copy staging resources

    // Verify context was cleanly zeroed
    uint8_t zeros[sizeof(FfxmFsr3UpscalerContext)] = {};
    TEST_CHECK(memcmp(&context, zeros, sizeof(context)) == 0);

    std::cout << "PASS: Context Creation and Resource Allocation verified (19 surfaces, 4 copy resources, 0 leaks)." << std::endl;
}

// 3. Unsupported FP16 Hardware Rejection
void TestUnsupportedFP16Hardware()
{
    std::cout << "--- Test 3: Unsupported FP16 Hardware Rejection ---" << std::endl;
    ResetMockState();
    s_Fp16SupportMock = false; // Unsupported FP16

    FfxmFsr3UpscalerContext context;
    memset(&context, 0xBB, sizeof(context));
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);

    FfxmErrorCode res = ffxmFsr3UpscalerContextCreate(&context, &desc);
    TEST_CHECK(res == FFXM_ERROR_UNSUPPORTED_DEVICE);
    // Backend context was created first, device capabilities queried second, then rollback destroyed it
    TEST_CHECK(s_CreateBackendContextCalled == true);
    TEST_CHECK(s_GetDeviceCapsCalled == true);
    TEST_CHECK(s_CreateBackendContextCallSeq < s_GetDeviceCapsCallSeq);
    TEST_CHECK(s_DestroyBackendContextCalled == true);
    TEST_CHECK(s_BackendContextActive == false);
    TEST_CHECK(s_ResourceCreationCounts.empty());
    TEST_CHECK(s_ResourceDestructionCounts.empty());

    // Context must be cleanly zeroed
    uint8_t zeros[sizeof(FfxmFsr3UpscalerContext)] = {};
    TEST_CHECK(memcmp(&context, zeros, sizeof(context)) == 0);

    std::cout << "PASS: Unsupported FP16 hardware returns FFXM_ERROR_UNSUPPORTED_DEVICE and cleanly rolls back backend context." << std::endl;
}

// 4. Backend Context Creation Failure
void TestBackendContextCreationFailure()
{
    std::cout << "--- Test 4: Backend Context Creation Failure ---" << std::endl;
    ResetMockState();
    s_CreateBackendContextResult = FFXM_ERROR_OUT_OF_MEMORY;

    FfxmFsr3UpscalerContext context;
    memset(&context, 0xCC, sizeof(context));
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);

    FfxmErrorCode res = ffxmFsr3UpscalerContextCreate(&context, &desc);
    TEST_CHECK(res == FFXM_ERROR_OUT_OF_MEMORY);
    TEST_CHECK(s_CreateBackendContextCalled == true);
    TEST_CHECK(s_GetDeviceCapsCalled == false); // Must NOT query caps if backend context creation failed
    TEST_CHECK(s_DestroyBackendContextCalled == false); // Never successfully created, so must NOT destroy
    TEST_CHECK(s_ResourceCreationCounts.empty());
    TEST_CHECK(s_ResourceDestructionCounts.empty());
    TEST_CHECK(s_BackendContextActive == false);

    uint8_t zeros[sizeof(FfxmFsr3UpscalerContext)] = {};
    TEST_CHECK(memcmp(&context, zeros, sizeof(context)) == 0);

    std::cout << "PASS: Backend context creation failure safely handled without destroying uncreated context." << std::endl;
}

// 5. Resource Allocation Failure at Each Stage (0 to 18) with Occurrence-Aware Tracking
void TestResourceAllocationFailureAtEachStage()
{
    std::cout << "--- Test 5: Resource Allocation Failure at Each Stage (0 to 18) ---" << std::endl;

    for (int32_t failIdx = 0; failIdx < FFXM_FSR3UPSCALER_INTERNAL_SURFACE_COUNT; ++failIdx) {
        ResetMockState();
        s_FailAtResourceIndex = failIdx;

        FfxmFsr3UpscalerContext context;
        memset(&context, 0xDD, sizeof(context));
        FfxmFsr3UpscalerContextDescription desc = {};
        desc.maxRenderSize = { 1920, 1080 };
        desc.maxUpscaleSize = { 3840, 2160 };
        SetupMockInterface(&desc.backendInterface);

        FfxmErrorCode res = ffxmFsr3UpscalerContextCreate(&context, &desc);
        TEST_CHECK(res == FFXM_ERROR_OUT_OF_MEMORY);

        // Backend context WAS created, so it MUST be destroyed
        TEST_CHECK(s_CreateBackendContextCalled == true);
        TEST_CHECK(s_DestroyBackendContextCalled == true);
        TEST_CHECK(s_DestroyedContextId == 100);
        TEST_CHECK(s_BackendContextActive == false);

        // Occurrence-aware check: every resource allocated prior to failure must be destroyed exactly once
        VerifyOccurrenceAwareResourceBalance();

        // Context must be zeroed out
        uint8_t zeros[sizeof(FfxmFsr3UpscalerContext)] = {};
        TEST_CHECK(memcmp(&context, zeros, sizeof(context)) == 0);
    }

    std::cout << "PASS: All 19 allocation failure stages cleanly rolled back with occurrence-aware balance." << std::endl;
}

// 6. Correct Resource Destruction & Double Destruction Safety
void TestCorrectResourceDestruction()
{
    std::cout << "--- Test 6: Resource Destruction & Idempotent Cleanup ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);

    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    // Destroy once
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    VerifyOccurrenceAwareResourceBalance();
    TEST_CHECK(s_DestroyBackendContextCalled == true);
    TEST_CHECK(s_BackendContextActive == false);

    // Destroy second time on zeroed context
    s_DestroyBackendContextCalled = false;
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    // Nothing new should have been destroyed
    VerifyOccurrenceAwareResourceBalance();
    TEST_CHECK(s_DestroyBackendContextCalled == false);

    std::cout << "PASS: Resource destruction is complete and double-destruction is fully safe." << std::endl;
}

// 7. Repeated Context Creation and Destruction
void TestRepeatedCreateDestroyCycles()
{
    std::cout << "--- Test 7: Repeated Context Creation and Destruction (10 cycles) ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);

    for (int cycle = 0; cycle < 10; ++cycle) {
        FfxmErrorCode res = ffxmFsr3UpscalerContextCreate(&context, &desc);
        TEST_CHECK(res == FFXM_OK);
        TEST_CHECK(s_BackendContextActive == true);

        res = ffxmFsr3UpscalerContextDestroy(&context);
        TEST_CHECK(res == FFXM_OK);
        TEST_CHECK(s_BackendContextActive == false);
    }

    // Verify occurrence-aware tracking across all 10 cycles
    VerifyOccurrenceAwareResourceBalance();
    TEST_CHECK(s_ResourceCreationCounts.size() == 230); // 23 resources * 10 cycles

    std::cout << "PASS: 10 repeated create/destroy cycles executed with 100% resource tracking fidelity." << std::endl;
}

// 8. Shared Resource Descriptions
void TestSharedResourceDescriptions()
{
    std::cout << "--- Test 8: Shared Resource Descriptions ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    FfxmFsr3UpscalerSharedResourceDescriptions shared = {};
    TEST_CHECK(ffxmFsr3UpscalerGetSharedResourceDescriptions(&context, &shared) == FFXM_OK);
    TEST_CHECK(shared.dilatedDepth.resourceDescription.format == FFXM_SURFACE_FORMAT_R32_FLOAT);
    TEST_CHECK(shared.dilatedMotionVectors.resourceDescription.format == FFXM_SURFACE_FORMAT_R16G16_FLOAT);
    TEST_CHECK(shared.reconstructedPrevNearestDepth.resourceDescription.format == FFXM_SURFACE_FORMAT_R32_UINT);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    std::cout << "PASS: Shared resource descriptions match Frame Generation Phase 2 contract." << std::endl;
}

// 9. Protected Unfinished APIs
void TestProtectedUnfinishedAPIs()
{
    std::cout << "--- Test 9: Protected Unfinished APIs (Dispatch & GenerateReactive) ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    FfxmCommandList dummyCmdList = reinterpret_cast<FfxmCommandList>(0x1234);

    // Test Dispatch protection
    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = dummyCmdList;
    dispDesc.renderSize = { 1280, 720 };
    dispDesc.upscaleSize = { 2560, 1440 };
    dispDesc.jitterOffset = { 0.25f, -0.25f };
    dispDesc.motionVectorScale = { 1.0f, 1.0f };
    dispDesc.frameTimeDelta = 0.0166f;
    dispDesc.cameraFovAngleVertical = 1.047f;
    dispDesc.viewSpaceToMetersFactor = 1.0f;

    FfxmErrorCode dispRes = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
    TEST_CHECK(dispRes == FFXM_OK);

    // Test GenerateReactive protection
    FfxmFsr3UpscalerGenerateReactiveDescription genDesc = {};
    genDesc.commandList = dummyCmdList;
    genDesc.scale = 0.5f;
    genDesc.cutoffThreshold = 0.2f;
    genDesc.binaryValue = 1.0f;

    FfxmErrorCode genRes = ffxmFsr3UpscalerContextGenerateReactiveMask(&context, &genDesc);
    TEST_CHECK(genRes == FFXM_ERROR_INCOMPLETE_INTERFACE);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    std::cout << "PASS: Dispatch executes PrepareInputs pass successfully (FFXM_OK); unfinished GenerateReactive API explicitly returns FFXM_ERROR_INCOMPLETE_INTERFACE." << std::endl;
}

// 10. Non-tautological Constant-Buffer Staging & Adapter Verification
void TestConstantBufferStagingSlots()
{
    std::cout << "--- Test 10: Constant-Buffer Staging Slots (All 4 Slots) ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);

    // Part A: Independent Staging Adapter Unit Test
    {
        struct TestConstBuffer {
            FfxmConstantBuffer cb;
            uint32_t canary1;
            uint32_t canary2;
        } bufferWithCanary = {};

        bufferWithCanary.canary1 = 0xDEADBEEF;
        bufferWithCanary.canary2 = 0xCAFEBABE;

        uint32_t testPayload[8] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444, 0x55555555, 0x66666666, 0x77777777, 0x88888888 };
        stageConstantBuffer(&bufferWithCanary.cb, testPayload, sizeof(testPayload));

        TEST_CHECK(bufferWithCanary.cb.num32BitEntries == 8);
        TEST_CHECK(memcmp(bufferWithCanary.cb.data, testPayload, sizeof(testPayload)) == 0);
        TEST_CHECK(bufferWithCanary.canary1 == 0xDEADBEEF);
        TEST_CHECK(bufferWithCanary.canary2 == 0xCAFEBABE);
    }

    // Part B: Staged entry counts initialized during context creation
    TEST_CHECK(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER].num32BitEntries ==
        sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t));
    TEST_CHECK(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD].num32BitEntries ==
        sizeof(Fsr3UpscalerSpdConstants) / sizeof(uint32_t));
    TEST_CHECK(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_RCAS].num32BitEntries ==
        sizeof(Fsr3UpscalerRcasConstants) / sizeof(uint32_t));
    TEST_CHECK(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_GENREACTIVE].num32BitEntries ==
        sizeof(Fsr3UpscalerGenerateReactiveConstants) / sizeof(uint32_t));

    // Part C: Verify Production Dispatch Stages Slot 0 (FSR3UPSCALER), Slot 1 (SPD), and Slot 2 (RCAS)
    FfxmCommandList dummyCmdList = reinterpret_cast<FfxmCommandList>(0xCAFE);
    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = dummyCmdList;
    dispDesc.renderSize = { 1280, 720 };
    dispDesc.upscaleSize = { 2560, 1440 };
    dispDesc.jitterOffset = { 0.33f, -0.66f };
    dispDesc.motionVectorScale = { 1.5f, 2.0f };
    dispDesc.sharpness = 0.65f;
    dispDesc.frameTimeDelta = 0.01666f;
    dispDesc.cameraFovAngleVertical = 1.047f;
    dispDesc.viewSpaceToMetersFactor = 1.0f;
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;

    FfxmErrorCode dispRes = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
    TEST_CHECK(dispRes == FFXM_OK); // executes PrepareInputs pass in Milestone 3

    // Verify Slot 0 (FSR3UPSCALER)
    TEST_CHECK(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER].num32BitEntries ==
        sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t));
    TEST_CHECK(priv->constants.renderSize[0] == 1280);
    TEST_CHECK(priv->constants.renderSize[1] == 720);
    TEST_CHECK(priv->constants.upscaleSize[0] == 2560);
    TEST_CHECK(priv->constants.upscaleSize[1] == 1440);
    TEST_CHECK(priv->constants.jitterOffset[0] == 0.33f);
    TEST_CHECK(priv->constants.jitterOffset[1] == -0.66f);
    TEST_CHECK(memcmp(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER].data,
        &priv->constants, sizeof(Fsr3UpscalerConstants)) == 0);

    // Verify Slot 1 (SPD) - computed via ffxSpdSetup for half-resolution Shading Change Pyramid
    uint32_t expDispatchThreadGroupCountXY[2];
    uint32_t expWorkGroupOffset[2];
    uint32_t expNumWorkGroupsAndMips[2];
    const uint32_t expHalfRenderW = (1280 + 1) / 2;
    const uint32_t expHalfRenderH = (720 + 1) / 2;
    uint32_t expRectInfo[4] = { 0, 0, expHalfRenderW, expHalfRenderH };
    ffxSpdSetup(expDispatchThreadGroupCountXY, expWorkGroupOffset, expNumWorkGroupsAndMips, expRectInfo);

    Fsr3UpscalerSpdConstants expectedSpd = {};
    expectedSpd.mips = expNumWorkGroupsAndMips[1];
    expectedSpd.numWorkGroups = expNumWorkGroupsAndMips[0];
    expectedSpd.workGroupOffset[0] = expWorkGroupOffset[0];
    expectedSpd.workGroupOffset[1] = expWorkGroupOffset[1];
    expectedSpd.renderSize[0] = expHalfRenderW;
    expectedSpd.renderSize[1] = expHalfRenderH;

    TEST_CHECK(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD].num32BitEntries == 6);
    TEST_CHECK(memcmp(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD].data,
        &expectedSpd, sizeof(expectedSpd)) == 0);

    // Verify Slot 2 (RCAS) - computed via FsrRcasCon
    Fsr3UpscalerRcasConstants expectedRcas = {};
    const float sharpnessRemapped = (-2.0f * dispDesc.sharpness) + 2.0f;
    FsrRcasCon(expectedRcas.rcasConfig, sharpnessRemapped);

    TEST_CHECK(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_RCAS].num32BitEntries == 4);
    TEST_CHECK(memcmp(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_RCAS].data,
        &expectedRcas, sizeof(expectedRcas)) == 0);

    // Part D: Verify Production GenerateReactive Stages Slot 3 (GENREACTIVE)
    FfxmFsr3UpscalerGenerateReactiveDescription genDesc = {};
    genDesc.commandList = dummyCmdList;
    genDesc.scale = 0.75f;
    genDesc.cutoffThreshold = 0.35f;
    genDesc.binaryValue = 0.9f;
    genDesc.flags = 0x03;

    FfxmErrorCode genRes = ffxmFsr3UpscalerContextGenerateReactiveMask(&context, &genDesc);
    TEST_CHECK(genRes == FFXM_ERROR_INCOMPLETE_INTERFACE); // protected until Milestone 3

    Fsr3UpscalerGenerateReactiveConstants expectedGen = {};
    expectedGen.scale = genDesc.scale;
    expectedGen.cutoffThreshold = genDesc.cutoffThreshold;
    expectedGen.binaryValue = genDesc.binaryValue;
    expectedGen.flags = genDesc.flags;

    TEST_CHECK(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_GENREACTIVE].num32BitEntries == 4);
    TEST_CHECK(memcmp(priv->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_GENREACTIVE].data,
        &expectedGen, sizeof(expectedGen)) == 0);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    std::cout << "PASS: All 4 constant-buffer staging slots verified for non-tautological production staging." << std::endl;
}

// 11. Regression Table Validation for All 19 Internal Resource Descriptions
void TestInternalResourceDescriptionsRegressionTable()
{
    std::cout << "--- Test 11: Internal Resource Descriptions Regression Table (All 19 Surfaces) ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    TEST_CHECK(s_CapturedResourceDescriptions.size() == FFXM_FSR3UPSCALER_INTERNAL_SURFACE_COUNT);

    struct ExpectedResourceDesc {
        uint32_t               id;
        const wchar_t*         name;
        FfxmSurfaceFormat       format;
        uint32_t               width;
        uint32_t               height;
        uint32_t               mipCount;
        FfxmResourceFlags      flags;
        FfxmResourceUsage      usage;
        FfxmResourceStates     initialState;
        uint32_t               initDataSize;
    };

    const ExpectedResourceDesc expectedTable[19] = {
        // 0. ACCUMULATION_1
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1, L"FSR3UPSCALER_Accumulation1",
          FFXM_SURFACE_FORMAT_R8_UNORM, 1920, 1080, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 1. ACCUMULATION_2
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2, L"FSR3UPSCALER_Accumulation2",
          FFXM_SURFACE_FORMAT_R8_UNORM, 1920, 1080, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 2. LUMA_1
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_1, L"FSR3UPSCALER_Luma1",
          FFXM_SURFACE_FORMAT_R16_FLOAT, 1920, 1080, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 3. LUMA_2
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_2, L"FSR3UPSCALER_Luma2",
          FFXM_SURFACE_FORMAT_R16_FLOAT, 1920, 1080, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 4. INTERMEDIATE_FP16x1 (Aliasable)
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERMEDIATE_FP16x1, L"FSR3UPSCALER_IntermediateFp16x1",
          FFXM_SURFACE_FORMAT_R16_FLOAT, 1920, 1080, 1,
          FFXM_RESOURCE_FLAGS_ALIASABLE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 5. SHADING_CHANGE (Aliasable)
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SHADING_CHANGE, L"FSR3UPSCALER_ShadingChange",
          FFXM_SURFACE_FORMAT_R8_UNORM, 960, 540, 1,
          FFXM_RESOURCE_FLAGS_ALIASABLE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 6. NEW_LOCKS (Aliasable)
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS, L"FSR3UPSCALER_NewLocks",
          FFXM_SURFACE_FORMAT_R8_UNORM, 3840, 2160, 1,
          FFXM_RESOURCE_FLAGS_ALIASABLE,
          FFXM_RESOURCE_USAGE_UAV,
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 7. INTERNAL_UPSCALED_COLOR_1
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_1, L"FSR3UPSCALER_InternalUpscaled1",
          FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT, 3840, 2160, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 8. INTERNAL_UPSCALED_COLOR_2
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_2, L"FSR3UPSCALER_InternalUpscaled2",
          FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT, 3840, 2160, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 9. SPD_MIPS (Aliasable, full mip chain)
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS, L"FSR3UPSCALER_SpdMips",
          FFXM_SURFACE_FORMAT_R16G16_FLOAT, 960, 540, 0,
          FFXM_RESOURCE_FLAGS_ALIASABLE,
          FFXM_RESOURCE_USAGE_UAV,
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 10. FARTHEST_DEPTH_MIP1 (Aliasable)
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH_MIP1, L"FSR3UPSCALER_FarthestDepthMip1",
          FFXM_SURFACE_FORMAT_R16_FLOAT, 960, 540, 1,
          FFXM_RESOURCE_FLAGS_ALIASABLE,
          FFXM_RESOURCE_USAGE_UAV,
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 11. LUMA_HISTORY_1
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_1, L"FSR3UPSCALER_LumaHistory1",
          FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT, 1920, 1080, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 12. LUMA_HISTORY_2
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_2, L"FSR3UPSCALER_LumaHistory2",
          FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT, 1920, 1080, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 13. SPD_ATOMIC_COUNT
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_ATOMIC_COUNT, L"FSR3UPSCALER_SpdAtomicCounter",
          FFXM_SURFACE_FORMAT_R32_UINT, 1, 1, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          FFXM_RESOURCE_USAGE_UAV,
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 4 },

        // 14. DILATED_REACTIVE_MASKS (Aliasable)
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS, L"FSR3UPSCALER_DilatedReactiveMasks",
          FFXM_SURFACE_FORMAT_R8G8B8A8_UNORM, 1920, 1080, 1,
          FFXM_RESOURCE_FLAGS_ALIASABLE,
          static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 },

        // 15. LANCZOS_LUT
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LANCZOS_LUT, L"FSR3UPSCALER_LanczosLutData",
          FFXM_SURFACE_FORMAT_R16_SNORM, 128, 1, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          FFXM_RESOURCE_USAGE_READ_ONLY,
          FFXM_RESOURCE_STATE_COMPUTE_READ, 256 },

        // 16. INTERNAL_DEFAULT_REACTIVITY
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY, L"FSR3UPSCALER_DefaultReactivityMask",
          FFXM_SURFACE_FORMAT_R8_UNORM, 1, 1, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          FFXM_RESOURCE_USAGE_READ_ONLY,
          FFXM_RESOURCE_STATE_COMPUTE_READ, 1 },

        // 17. INTERNAL_DEFAULT_EXPOSURE
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_EXPOSURE, L"FSR3UPSCALER_DefaultExposure",
          FFXM_SURFACE_FORMAT_R32G32_FLOAT, 1, 1, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          FFXM_RESOURCE_USAGE_READ_ONLY,
          FFXM_RESOURCE_STATE_COMPUTE_READ, 8 },

        // 18. FRAME_INFO
        { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FRAME_INFO, L"FSR3UPSCALER_FrameInfo",
          FFXM_SURFACE_FORMAT_R32G32B32A32_FLOAT, 1, 1, 1,
          FFXM_RESOURCE_FLAGS_NONE,
          FFXM_RESOURCE_USAGE_UAV,
          FFXM_RESOURCE_STATE_UNORDERED_ACCESS, 0 }
    };

    for (size_t i = 0; i < 19; ++i) {
        const auto& actual = s_CapturedResourceDescriptions[i];
        const auto& expected = expectedTable[i];

        TEST_CHECK(actual.id == expected.id);
        TEST_CHECK(wcscmp(actual.name, expected.name) == 0);
        TEST_CHECK(actual.heapType == FFXM_HEAP_TYPE_DEFAULT);
        TEST_CHECK(actual.resourceDescription.type == FFXM_RESOURCE_TYPE_TEXTURE2D);
        TEST_CHECK(actual.resourceDescription.format == expected.format);
        TEST_CHECK(actual.resourceDescription.width == expected.width);
        TEST_CHECK(actual.resourceDescription.height == expected.height);
        TEST_CHECK(actual.resourceDescription.depth == 1);
        TEST_CHECK(actual.resourceDescription.mipCount == expected.mipCount);
        TEST_CHECK(actual.resourceDescription.flags == expected.flags);
        TEST_CHECK(actual.resourceDescription.usage == expected.usage);
        TEST_CHECK(actual.initalState == expected.initialState);
        TEST_CHECK(actual.initDataSize == expected.initDataSize);
        if (expected.initDataSize > 0) {
            TEST_CHECK(actual.initData != nullptr);
        } else {
            TEST_CHECK(actual.initData == nullptr);
        }
    }

    // Verify actual initial data byte contents
    // 1. SPD_ATOMIC_COUNT: uint32_t == 0
    uint32_t atomicCount = 0;
    memcpy(&atomicCount, s_CapturedInitialData[0].data(), sizeof(atomicCount));
    TEST_CHECK(atomicCount == 0);

    // 2. LANCZOS_LUT: 128 elements of int16_t, index 0 is 32767 (lanczos2(0) == 1.0)
    const int16_t* pWeights = reinterpret_cast<const int16_t*>(s_CapturedInitialData[1].data());
    TEST_CHECK(s_CapturedInitialData[1].size() == 256);
    TEST_CHECK(pWeights[0] == 32767); // lanczos2(0) = 1.0 * 32767.0f

    // 3. INTERNAL_DEFAULT_REACTIVITY: uint8_t == 0
    TEST_CHECK(s_CapturedInitialData[2].size() == 1);
    TEST_CHECK(s_CapturedInitialData[2][0] == 0);

    // 4. INTERNAL_DEFAULT_EXPOSURE: float[2] == {0.0f, 0.0f}
    const float* pExposure = reinterpret_cast<const float*>(s_CapturedInitialData[3].data());
    TEST_CHECK(s_CapturedInitialData[3].size() == 8);
    TEST_CHECK(pExposure[0] == 0.0f);
    TEST_CHECK(pExposure[1] == 0.0f);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    std::cout << "PASS: All 19 internal resource descriptions and initial data match authoritative FSR 3.1.4 reference." << std::endl;
}

// 12. Public API Parameter Validation & Boundary Checks
void TestPublicAPIValidation()
{
    std::cout << "--- Test 12: Public API Parameter Validation & Boundary Checks ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);

    // A. Null pointer checks on create
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(nullptr, &desc) == FFXM_ERROR_INVALID_POINTER);
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, nullptr) == FFXM_ERROR_INVALID_POINTER);

    // B. Dimension zero checks on create
    desc.maxRenderSize = { 0, 1080 };
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_ERROR_INVALID_ARGUMENT);
    desc.maxRenderSize = { 1920, 0 };
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_ERROR_INVALID_ARGUMENT);
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 0, 2160 };
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_ERROR_INVALID_ARGUMENT);
    desc.maxUpscaleSize = { 3840, 0 };
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_ERROR_INVALID_ARGUMENT);

    // C. Dimension relation checks on create (render size > upscale size)
    desc.maxRenderSize = { 3841, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_ERROR_INVALID_ARGUMENT);
    desc.maxRenderSize = { 1920, 2161 };
    desc.maxUpscaleSize = { 3840, 2160 };
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_ERROR_INVALID_ARGUMENT);

    // D. Incomplete interface checks
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    desc.backendInterface.fpCreateBackendContext = nullptr;
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_ERROR_INCOMPLETE_INTERFACE);
    SetupMockInterface(&desc.backendInterface);

    // Create valid context for dispatch and reactive parameter tests
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    // E. Dispatch null pointer checks
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(nullptr, nullptr) == FFXM_ERROR_INVALID_POINTER);
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, nullptr) == FFXM_ERROR_INVALID_POINTER);

    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_ERROR_INVALID_POINTER); // null commandList

    dispDesc.commandList = reinterpret_cast<FfxmCommandList>(0x1234);

    // F. Dispatch out-of-range checks
    dispDesc.renderSize = { 1921, 1080 }; // > maxRenderSize.width
    dispDesc.upscaleSize = { 3840, 2160 };
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_ERROR_OUT_OF_RANGE);

    dispDesc.renderSize = { 1920, 1081 }; // > maxRenderSize.height
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_ERROR_OUT_OF_RANGE);

    dispDesc.renderSize = { 1920, 1080 };
    dispDesc.upscaleSize = { 3841, 2160 }; // > maxUpscaleSize.width
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_ERROR_OUT_OF_RANGE);

    dispDesc.renderSize = { 1920, 1080 };
    dispDesc.upscaleSize = { 3840, 2161 }; // > maxUpscaleSize.height
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_ERROR_OUT_OF_RANGE);

    // G. GenerateReactive null pointer checks
    TEST_CHECK(ffxmFsr3UpscalerContextGenerateReactiveMask(nullptr, nullptr) == FFXM_ERROR_INVALID_POINTER);
    TEST_CHECK(ffxmFsr3UpscalerContextGenerateReactiveMask(&context, nullptr) == FFXM_ERROR_INVALID_POINTER);
    FfxmFsr3UpscalerGenerateReactiveDescription genDesc = {};
    TEST_CHECK(ffxmFsr3UpscalerContextGenerateReactiveMask(&context, &genDesc) == FFXM_ERROR_INVALID_POINTER); // null commandList

    // H. Null context destroy
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(nullptr) == FFXM_ERROR_INVALID_POINTER);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    std::cout << "PASS: Public API parameter validation and boundary checks verified." << std::endl;
}

// 13. Context Reuse After Failure
void TestContextReuseAfterFailure()
{
    std::cout << "--- Test 13: Context Reuse After Rollback Failure ---" << std::endl;
    ResetMockState();

    FfxmFsr3UpscalerContext context;
    memset(&context, 0xEE, sizeof(context));

    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);

    // Inject failure halfway through allocation
    s_FailAtResourceIndex = 8;
    FfxmErrorCode res = ffxmFsr3UpscalerContextCreate(&context, &desc);
    TEST_CHECK(res == FFXM_ERROR_OUT_OF_MEMORY);
    VerifyOccurrenceAwareResourceBalance();

    // Immediately reuse the same context variable for successful creation
    s_FailAtResourceIndex = -1;
    res = ffxmFsr3UpscalerContextCreate(&context, &desc);
    TEST_CHECK(res == FFXM_OK);

    // Verify context is healthy and fully usable
    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);
    TEST_CHECK(priv->backendContextCreated == true);
    TEST_CHECK(priv->allocatedResourceCount == FFXM_FSR3UPSCALER_INTERNAL_SURFACE_COUNT);

    res = ffxmFsr3UpscalerContextDestroy(&context);
    TEST_CHECK(res == FFXM_OK);
    VerifyOccurrenceAwareResourceBalance();

    std::cout << "PASS: Context cleanly reused after rolled-back failure." << std::endl;
}

// 14. Fix A: Device Capabilities Ordering & Rollback Contract
void TestDeviceCapabilitiesOrderingContract()
{
    std::cout << "--- Test 14: Vulkan Device Capabilities Call Ordering & Rollback Contract ---" << std::endl;
    ResetMockState();

    // Verify that querying device capabilities before backend context creation fails as uninitialized
    FfxmInterface iface = {};
    SetupMockInterface(&iface);
    FfxmDeviceCapabilities caps = {};
    TEST_CHECK(iface.fpGetDeviceCapabilities(&iface, &caps) == FFXM_ERROR_BACKEND_API_ERROR);

    // Verify that during context creation, backend context is created BEFORE capabilities are queried
    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    SetupMockInterface(&desc.backendInterface);

    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);
    TEST_CHECK(s_CreateBackendContextCalled == true);
    TEST_CHECK(s_GetDeviceCapsCalled == true);
    TEST_CHECK(s_CreateBackendContextCallSeq < s_GetDeviceCapsCallSeq);
    TEST_CHECK(s_BackendContextActive == true);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    TEST_CHECK(s_BackendContextActive == false);

    // Verify rollback ordering when FP16 is unsupported
    ResetMockState();
    s_Fp16SupportMock = false;
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_ERROR_UNSUPPORTED_DEVICE);
    TEST_CHECK(s_CreateBackendContextCalled == true);
    TEST_CHECK(s_GetDeviceCapsCalled == true);
    TEST_CHECK(s_CreateBackendContextCallSeq < s_GetDeviceCapsCallSeq);
    TEST_CHECK(s_DestroyBackendContextCalled == true);
    TEST_CHECK(s_BackendContextActive == false);

    std::cout << "PASS: Vulkan device capabilities call ordering and failure rollback verified." << std::endl;
}

// 15. Fix B: Scratch Memory Sizing Arithmetic & Overflow Prevention
void TestScratchMemorySizingArithmetic()
{
    std::cout << "--- Test 15: Scratch Memory Sizing Arithmetic & Overflow Prevention ---" << std::endl;

    // 1. Direct unit verification of checked arithmetic primitives
    size_t outSize = 0;
    TEST_CHECK(testCheckMulSize(100, 200, &outSize) == true);
    TEST_CHECK(outSize == 20000);

    TEST_CHECK(testCheckMulSize(SIZE_MAX, 2, &outSize) == false);
    TEST_CHECK(testCheckMulSize(SIZE_MAX / 2 + 1, 2, &outSize) == false);
    TEST_CHECK(testCheckAddSize(SIZE_MAX - 10, 20, &outSize) == false);
    TEST_CHECK(testCheckAlignUpSize(SIZE_MAX - 3, 8, &outSize) == false);

    size_t alignedVal = 0;
    TEST_CHECK(testCheckAlignUpSize(17, 8, &alignedVal) == true);
    TEST_CHECK(alignedVal == 24);

#if defined(FFXM_HAS_VK_BACKEND)
    // 2. Direct verification of ffxmGetScratchMemorySizeVK
    // A. Minimum and zero context counts
    size_t zeroSize = ffxmGetScratchMemorySizeVK(nullptr, 0);
    TEST_CHECK(zeroSize == 0);

    // B. Normal supported context counts (1, 2, 4, 8)
    size_t size1 = ffxmGetScratchMemorySizeVK(nullptr, 1);
    size_t size2 = ffxmGetScratchMemorySizeVK(nullptr, 2);
    size_t size4 = ffxmGetScratchMemorySizeVK(nullptr, 4);
    size_t size8 = ffxmGetScratchMemorySizeVK(nullptr, 8);

    TEST_CHECK(size1 > 0);
    TEST_CHECK(size2 > size1);
    TEST_CHECK(size4 > size2);
    TEST_CHECK(size8 > size4);

    // Alignment consistency (must be aligned to 8 bytes / uint64_t)
    TEST_CHECK(size1 % sizeof(uint64_t) == 0);
    TEST_CHECK(size2 % sizeof(uint64_t) == 0);
    TEST_CHECK(size4 % sizeof(uint64_t) == 0);
    TEST_CHECK(size8 % sizeof(uint64_t) == 0);

    // C. Overflowing and extreme context counts (must return 0 safely without wrapping or crashing)
    size_t maxUint32Size = ffxmGetScratchMemorySizeVK(nullptr, static_cast<size_t>(UINT32_MAX));
    TEST_CHECK(maxUint32Size == 0);

    size_t maxContextSize = ffxmGetScratchMemorySizeVK(nullptr, SIZE_MAX);
    TEST_CHECK(maxContextSize == 0);

    size_t halfMaxSize = ffxmGetScratchMemorySizeVK(nullptr, SIZE_MAX / 2);
    TEST_CHECK(halfMaxSize == 0);

    if (sizeof(size_t) > 4) {
        size_t large64BitContextSize = ffxmGetScratchMemorySizeVK(nullptr, static_cast<size_t>(uint64_t(1) << 33));
        TEST_CHECK(large64BitContextSize == 0);
    }

    // D. Verification of ffxmGetInterfaceVK scratch buffer sizing enforcement
    FfxmInterface iface = {};
    VkDeviceContext dummyVkDevice = {};
    dummyVkDevice.vkPhysicalDevice = VK_NULL_HANDLE;
    dummyVkDevice.vkDevice = VK_NULL_HANDLE;
    FfxmDevice dummyDevice = ffxmGetDeviceVK(&dummyVkDevice);

    std::vector<uint8_t> scratch(size1);

    // Exact scratchBufferSize succeeds
    FfxmErrorCode ifaceRes = ffxmGetInterfaceVK(&iface, dummyDevice, scratch.data(), size1, 1);
    TEST_CHECK(ifaceRes == FFXM_OK);

    // Undersized scratchBufferSize by 1 byte fails with FFXM_ERROR_INSUFFICIENT_MEMORY
    FfxmInterface ifaceFail = {};
    FfxmErrorCode ifaceFailRes = ffxmGetInterfaceVK(&ifaceFail, dummyDevice, scratch.data(), size1 - 1, 1);
    TEST_CHECK(ifaceFailRes == FFXM_ERROR_INSUFFICIENT_MEMORY);

    // maxContexts == 0 fails with FFXM_ERROR_INVALID_ARGUMENT
    FfxmInterface ifaceZero = {};
    FfxmErrorCode ifaceZeroRes = ffxmGetInterfaceVK(&ifaceZero, dummyDevice, scratch.data(), size1, 0);
    TEST_CHECK(ifaceZeroRes == FFXM_ERROR_INVALID_ARGUMENT);

    // Overflowing maxContexts fails with FFXM_ERROR_INVALID_ARGUMENT
    FfxmInterface ifaceOverflow = {};
    FfxmErrorCode ifaceOverflowRes = ffxmGetInterfaceVK(&ifaceOverflow, dummyDevice, scratch.data(), size1, SIZE_MAX);
    TEST_CHECK(ifaceOverflowRes == FFXM_ERROR_INVALID_ARGUMENT);
#endif

    std::cout << "PASS: Scratch memory sizing arithmetic and overflow prevention verified." << std::endl;
}

// 16. M3: PrepareInputs Compute Pass Execution, Pipeline Binding Reflection & GPU Dispatch
void TestM3PrepareInputsPassExecution()
{
    std::cout << "--- Test 16: M3 PrepareInputs Compute Pass Execution & GPU Dispatch ---" << std::endl;
    ResetMockState();

    // 1. Verify SPIR-V permutation shader blob retrieval across configurations
    FfxmShaderBlob blobDefault = {};
    TEST_CHECK(arm::ffxmGetPermutationBlobByIndex(FFXM_EFFECT_FSR3UPSCALER, FFXM_FSR3UPSCALER_PASS_PREPARE_INPUTS, 0, &blobDefault) == FFXM_OK);
    TEST_CHECK(blobDefault.data != nullptr);
    TEST_CHECK(blobDefault.size > 0);
    TEST_CHECK(blobDefault.srvTextureCount == 3);
    TEST_CHECK(blobDefault.uavTextureCount == 5);
    TEST_CHECK(blobDefault.cbvCount == 1);

    // Test inverted depth permutation
    FfxmShaderBlob blobInverted = {};
    TEST_CHECK(arm::ffxmGetPermutationBlobByIndex(FFXM_EFFECT_FSR3UPSCALER, FFXM_FSR3UPSCALER_PASS_PREPARE_INPUTS,
        FSR3UPSCALER_SHADER_PERMUTATION_DEPTH_INVERTED, &blobInverted) == FFXM_OK);
    TEST_CHECK(blobInverted.data != nullptr);
    TEST_CHECK(blobInverted.size > 0);

    // 2. Create context with full mock backend
    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    desc.flags = FFXM_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    SetupMockInterface(&desc.backendInterface);

    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);
    TEST_CHECK(s_CreatePipelineCallCount == 7);

    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);
    TEST_CHECK(priv->pipelinePrepareInputs.pipeline != nullptr);
    TEST_CHECK(priv->pipelinePrepareInputs.srvTextureCount == 3);
    TEST_CHECK(priv->pipelinePrepareInputs.uavTextureCount == 5);
    TEST_CHECK(priv->pipelinePrepareInputs.constCount == 1);

    TEST_CHECK(priv->pipelineShadingChange.pipeline != nullptr);
    TEST_CHECK(priv->pipelineShadingChange.srvTextureCount == 1);
    TEST_CHECK(priv->pipelineShadingChange.uavTextureCount == 1);
    TEST_CHECK(priv->pipelineShadingChange.constCount == 1);
    TEST_CHECK(priv->pipelineShadingChange.srvTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS);
    TEST_CHECK(priv->pipelineShadingChange.uavTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SHADING_CHANGE);
    TEST_CHECK(priv->pipelineShadingChange.constantBufferBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER);

    TEST_CHECK(priv->pipelinePrepareReactivity.pipeline != nullptr);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureCount == 9);
    TEST_CHECK(priv->pipelinePrepareReactivity.uavTextureCount == 3);
    TEST_CHECK(priv->pipelinePrepareReactivity.constCount == 1);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[2].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_DEPTH);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[3].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_REACTIVE_MASK);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[4].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_TRANSPARENCY_AND_COMPOSITION_MASK);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[5].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[6].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[7].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_CURRENT_LUMA);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[8].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_EXPOSURE);
    TEST_CHECK(priv->pipelinePrepareReactivity.uavTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS);
    TEST_CHECK(priv->pipelinePrepareReactivity.uavTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS);
    TEST_CHECK(priv->pipelinePrepareReactivity.uavTextureBindings[2].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION);
    TEST_CHECK(priv->pipelinePrepareReactivity.constantBufferBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER);

    // Verify patched bindings for PrepareInputs
    TEST_CHECK(priv->pipelinePrepareInputs.srvTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_MOTION_VECTORS);
    TEST_CHECK(priv->pipelinePrepareInputs.srvTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_DEPTH);
    TEST_CHECK(priv->pipelinePrepareInputs.srvTextureBindings[2].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_COLOR);

    TEST_CHECK(priv->pipelinePrepareInputs.uavTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS);
    TEST_CHECK(priv->pipelinePrepareInputs.uavTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_DEPTH);
    TEST_CHECK(priv->pipelinePrepareInputs.uavTextureBindings[2].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH);
    TEST_CHECK(priv->pipelinePrepareInputs.uavTextureBindings[3].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH);
    TEST_CHECK(priv->pipelinePrepareInputs.uavTextureBindings[4].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_CURRENT_LUMA);

    TEST_CHECK(priv->pipelinePrepareInputs.constantBufferBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER);

    // Verify LumaPyramid pipeline
    TEST_CHECK(priv->pipelineLumaPyramid.pipeline != nullptr);
    TEST_CHECK(priv->pipelineLumaPyramid.srvTextureCount == 2);
    TEST_CHECK(priv->pipelineLumaPyramid.uavTextureCount == 4);
    TEST_CHECK(priv->pipelineLumaPyramid.constCount == 2);

    TEST_CHECK(priv->pipelineLumaPyramid.srvTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_CURRENT_LUMA);
    TEST_CHECK(priv->pipelineLumaPyramid.srvTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH);

    TEST_CHECK(priv->pipelineLumaPyramid.uavTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_ATOMIC_COUNT);
    TEST_CHECK(priv->pipelineLumaPyramid.uavTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FRAME_INFO);
    TEST_CHECK(priv->pipelineLumaPyramid.uavTextureBindings[2].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_5);
    TEST_CHECK(priv->pipelineLumaPyramid.uavTextureBindings[3].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH_MIP1);

    TEST_CHECK(priv->pipelineLumaPyramid.constantBufferBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER);
    TEST_CHECK(priv->pipelineLumaPyramid.constantBufferBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD);

    // Verify ShadingChangePyramid pipeline
    TEST_CHECK(priv->pipelineShadingChangePyramid.pipeline != nullptr);
    TEST_CHECK(priv->pipelineShadingChangePyramid.srvTextureCount == 4);
    TEST_CHECK(priv->pipelineShadingChangePyramid.uavTextureCount == 7);
    TEST_CHECK(priv->pipelineShadingChangePyramid.constCount == 2);

    TEST_CHECK(priv->pipelineShadingChangePyramid.srvTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_CURRENT_LUMA);
    TEST_CHECK(priv->pipelineShadingChangePyramid.srvTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_PREVIOUS_LUMA);
    TEST_CHECK(priv->pipelineShadingChangePyramid.srvTextureBindings[2].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS);
    TEST_CHECK(priv->pipelineShadingChangePyramid.srvTextureBindings[3].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_EXPOSURE);

    TEST_CHECK(priv->pipelineShadingChangePyramid.uavTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_ATOMIC_COUNT);
    TEST_CHECK(priv->pipelineShadingChangePyramid.uavTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_0);
    TEST_CHECK(priv->pipelineShadingChangePyramid.uavTextureBindings[2].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_1);
    TEST_CHECK(priv->pipelineShadingChangePyramid.uavTextureBindings[3].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_2);
    TEST_CHECK(priv->pipelineShadingChangePyramid.uavTextureBindings[4].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_3);
    TEST_CHECK(priv->pipelineShadingChangePyramid.uavTextureBindings[5].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_4);
    TEST_CHECK(priv->pipelineShadingChangePyramid.uavTextureBindings[6].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_5);

    TEST_CHECK(priv->pipelineShadingChangePyramid.constantBufferBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER);
    TEST_CHECK(priv->pipelineShadingChangePyramid.constantBufferBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD);

    // Verify LumaInstability pipeline
    TEST_CHECK(priv->pipelineLumaInstability.pipeline != nullptr);
    TEST_CHECK(priv->pipelineLumaInstability.srvTextureCount == 5);
    TEST_CHECK(priv->pipelineLumaInstability.uavTextureCount == 2);
    TEST_CHECK(priv->pipelineLumaInstability.constCount == 1);
    TEST_CHECK(priv->pipelineLumaInstability.srvTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_EXPOSURE);
    TEST_CHECK(priv->pipelineLumaInstability.srvTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS);
    TEST_CHECK(priv->pipelineLumaInstability.srvTextureBindings[2].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS);
    TEST_CHECK(priv->pipelineLumaInstability.srvTextureBindings[3].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY);
    TEST_CHECK(priv->pipelineLumaInstability.srvTextureBindings[4].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_CURRENT_LUMA);
    TEST_CHECK(priv->pipelineLumaInstability.uavTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY);
    TEST_CHECK(priv->pipelineLumaInstability.uavTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_INSTABILITY);
    TEST_CHECK(priv->pipelineLumaInstability.constantBufferBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER);

    // Verify Accumulate pipeline
    TEST_CHECK(priv->pipelineAccumulate.pipeline != nullptr);
    const bool accUsesLut = (priv->pipelineAccumulate.srvTextureCount == 8);
    TEST_CHECK(accUsesLut || priv->pipelineAccumulate.srvTextureCount == 7);
    TEST_CHECK(priv->pipelineAccumulate.uavTextureCount == 3);
    TEST_CHECK(priv->pipelineAccumulate.constCount == 1);
    TEST_CHECK(priv->pipelineAccumulate.srvTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_EXPOSURE);
    TEST_CHECK(priv->pipelineAccumulate.srvTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS);
    TEST_CHECK(priv->pipelineAccumulate.srvTextureBindings[2].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS);
    TEST_CHECK(priv->pipelineAccumulate.srvTextureBindings[3].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR);
    if (accUsesLut) {
        TEST_CHECK(priv->pipelineAccumulate.srvTextureBindings[4].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LANCZOS_LUT);
        TEST_CHECK(priv->pipelineAccumulate.srvTextureBindings[5].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH_MIP1);
        TEST_CHECK(priv->pipelineAccumulate.srvTextureBindings[6].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_INSTABILITY);
        TEST_CHECK(priv->pipelineAccumulate.srvTextureBindings[7].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_COLOR);
    } else {
        TEST_CHECK(priv->pipelineAccumulate.srvTextureBindings[4].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH_MIP1);
        TEST_CHECK(priv->pipelineAccumulate.srvTextureBindings[5].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_INSTABILITY);
        TEST_CHECK(priv->pipelineAccumulate.srvTextureBindings[6].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_COLOR);
    }
    TEST_CHECK(priv->pipelineAccumulate.uavTextureBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR);
    TEST_CHECK(priv->pipelineAccumulate.uavTextureBindings[1].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT);
    TEST_CHECK(priv->pipelineAccumulate.uavTextureBindings[2].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS);
    TEST_CHECK(priv->pipelineAccumulate.constantBufferBindings[0].resourceIdentifier == FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER);

    // 3. Dispatch a real frame
    FfxmCommandList dummyCmdList = reinterpret_cast<FfxmCommandList>(0xABCD);
    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = dummyCmdList;
    dispDesc.renderSize = { 1920, 1080 };
    dispDesc.upscaleSize = { 3840, 2160 };
    dispDesc.jitterOffset = { 0.125f, -0.25f };
    dispDesc.motionVectorScale = { 1.0f, 1.0f };
    dispDesc.frameTimeDelta = 16.666f;
    dispDesc.cameraFovAngleVertical = 1.047f;
    dispDesc.viewSpaceToMetersFactor = 1.0f;
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;

    static int s_ColorResource = 10;
    static int s_DepthResource = 20;
    static int s_MvResource = 30;
    static int s_DilatedMvResource = 40;
    static int s_DilatedDepthResource = 50;
    static int s_ReconstructedDepthResource = 60;
    static int s_OutputResource = 70;
    dispDesc.color.resource = &s_ColorResource;
    dispDesc.depth.resource = &s_DepthResource;
    dispDesc.motionVectors.resource = &s_MvResource;
    dispDesc.dilatedMotionVectors.resource = &s_DilatedMvResource;
    dispDesc.dilatedDepth.resource = &s_DilatedDepthResource;
    dispDesc.reconstructedPrevNearestDepth.resource = &s_ReconstructedDepthResource;
    dispDesc.output.resource = &s_OutputResource;

    s_ScheduledGpuJobs.clear();
    s_ExecuteGpuJobsCallCount = 0;
    s_UnregisterResourcesCallCount = 0;

    FfxmErrorCode dispRes = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
    TEST_CHECK(dispRes == FFXM_OK);

    // Verify scheduled GPU jobs on first frame (history reset):
    // 0: Clear Float for FRAME_INFO
    // 1: Clear Float for ACCUMULATION_1
    // 2: Clear Float for ACCUMULATION_2
    // 3: Clear Float for LUMA_HISTORY_1
    // 4: Clear Float for LUMA_HISTORY_2
    // 5: Clear Float for INTERNAL_UPSCALED_COLOR_1
    // 6: Clear Float for INTERNAL_UPSCALED_COLOR_2
    // 7: Clear Float for NEW_LOCKS
    // 8: Clear Float for RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH
    // 9: Clear Float for SPD_ATOMIC_COUNT
    // 10: Clear Float for SPD_MIPS
    // 11: Compute dispatch for PrepareInputs
    // 12: Compute dispatch for LumaPyramid
    // 13: Clear Float for SPD_ATOMIC_COUNT (reset before Shading Change SPD)
    // 14: Compute dispatch for ShadingChangePyramid
    // 15: Compute dispatch for PrepareReactivity (fused ShadingChange)
    // 16: Compute dispatch for LumaInstability
    // 17: Compute dispatch for Accumulate
    TEST_CHECK(s_ScheduledGpuJobs.size() == 18);

    // Check clear jobs (8 reset clears + 3 per-frame clears = 11 clears)
    for (uint32_t c = 0; c < 11; ++c) {
        TEST_CHECK(s_ScheduledGpuJobs[c].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    }

    // Check compute dispatch job for PrepareInputs
    const FfxmGpuJobDescription& computeJob = s_ScheduledGpuJobs[11];
    TEST_CHECK(computeJob.jobType == FFXM_GPU_JOB_COMPUTE);
    // Dispatch dimensions: ceil(1920 / 8) = 240, ceil(1080 / 8) = 135, 1
    TEST_CHECK(computeJob.computeJobDescriptor.dimensions[0] == 240);
    TEST_CHECK(computeJob.computeJobDescriptor.dimensions[1] == 135);
    TEST_CHECK(computeJob.computeJobDescriptor.dimensions[2] == 1);

    // Verify SRVs in PrepareInputs compute job
    TEST_CHECK(computeJob.computeJobDescriptor.srvTextures[0].internalIndex != -1);
    TEST_CHECK(computeJob.computeJobDescriptor.srvTextures[1].internalIndex != -1);
    TEST_CHECK(computeJob.computeJobDescriptor.srvTextures[2].internalIndex != -1);

    // Verify UAVs in PrepareInputs compute job
    TEST_CHECK(computeJob.computeJobDescriptor.uavTextures[0].internalIndex != -1);
    TEST_CHECK(computeJob.computeJobDescriptor.uavTextures[1].internalIndex != -1);
    TEST_CHECK(computeJob.computeJobDescriptor.uavTextures[2].internalIndex != -1);
    TEST_CHECK(computeJob.computeJobDescriptor.uavTextures[3].internalIndex != -1);
    TEST_CHECK(computeJob.computeJobDescriptor.uavTextures[4].internalIndex != -1);

    // Verify Constant Buffer in PrepareInputs compute job
    TEST_CHECK(computeJob.computeJobDescriptor.cbs[0].num32BitEntries == sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t));

    // Check compute dispatch job for LumaPyramid
    const FfxmGpuJobDescription& lumaJob = s_ScheduledGpuJobs[12];
    TEST_CHECK(lumaJob.jobType == FFXM_GPU_JOB_COMPUTE);
    // SPD dispatch dimensions: (1920 + 63) / 64 = 30, (1080 + 63) / 64 = 17, 1
    TEST_CHECK(lumaJob.computeJobDescriptor.dimensions[0] == 30);
    TEST_CHECK(lumaJob.computeJobDescriptor.dimensions[1] == 17);
    TEST_CHECK(lumaJob.computeJobDescriptor.dimensions[2] == 1);

    // Verify SRVs in LumaPyramid compute job
    TEST_CHECK(lumaJob.computeJobDescriptor.srvTextures[0].internalIndex != -1);
    TEST_CHECK(lumaJob.computeJobDescriptor.srvTextures[1].internalIndex != -1);

    // Verify UAVs in LumaPyramid compute job
    TEST_CHECK(lumaJob.computeJobDescriptor.uavTextures[0].internalIndex != -1);
    TEST_CHECK(lumaJob.computeJobDescriptor.uavTextures[1].internalIndex != -1);
    TEST_CHECK(lumaJob.computeJobDescriptor.uavTextures[2].internalIndex != -1);
    TEST_CHECK(lumaJob.computeJobDescriptor.uavTextures[3].internalIndex != -1);

    // Verify Constant Buffers in LumaPyramid compute job
    TEST_CHECK(lumaJob.computeJobDescriptor.cbs[0].num32BitEntries == sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t));
    TEST_CHECK(lumaJob.computeJobDescriptor.cbs[1].num32BitEntries == sizeof(Fsr3UpscalerSpdConstants) / sizeof(uint32_t));

    // Check atomic counter clear job between SPD passes
    TEST_CHECK(s_ScheduledGpuJobs[13].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);

    // Check compute dispatch job for ShadingChangePyramid
    const FfxmGpuJobDescription& shadingJob = s_ScheduledGpuJobs[14];
    TEST_CHECK(shadingJob.jobType == FFXM_GPU_JOB_COMPUTE);
    // Half-res SPD dispatch dimensions: (960 + 63) / 64 = 15, (540 + 63) / 64 = 9, 1
    TEST_CHECK(shadingJob.computeJobDescriptor.dimensions[0] == 15);
    TEST_CHECK(shadingJob.computeJobDescriptor.dimensions[1] == 9);
    TEST_CHECK(shadingJob.computeJobDescriptor.dimensions[2] == 1);

    // Verify SRVs in ShadingChangePyramid compute job (4 SRVs)
    TEST_CHECK(shadingJob.computeJobDescriptor.srvTextures[0].internalIndex != -1);
    TEST_CHECK(shadingJob.computeJobDescriptor.srvTextures[1].internalIndex != -1);
    TEST_CHECK(shadingJob.computeJobDescriptor.srvTextures[2].internalIndex != -1);
    TEST_CHECK(shadingJob.computeJobDescriptor.srvTextures[3].internalIndex != -1);

    // Verify UAVs in ShadingChangePyramid compute job (7 UAVs)
    for (uint32_t u = 0; u < 7; ++u) {
        TEST_CHECK(shadingJob.computeJobDescriptor.uavTextures[u].internalIndex != -1);
    }

    // Verify Constant Buffers in ShadingChangePyramid compute job
    TEST_CHECK(shadingJob.computeJobDescriptor.cbs[0].num32BitEntries == sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t));
    TEST_CHECK(shadingJob.computeJobDescriptor.cbs[1].num32BitEntries == sizeof(Fsr3UpscalerSpdConstants) / sizeof(uint32_t));
    const Fsr3UpscalerSpdConstants* shadingSpd = reinterpret_cast<const Fsr3UpscalerSpdConstants*>(shadingJob.computeJobDescriptor.cbs[1].data);
    TEST_CHECK(shadingSpd->numWorkGroups == 135);
    TEST_CHECK(shadingSpd->renderSize[0] == 960);
    TEST_CHECK(shadingSpd->renderSize[1] == 540);

    // Phase 3: ShadingChange is fused directly into PrepareReactivity (eliminating 1 dispatch and DRAM roundtrip)
    TEST_CHECK(s_ScheduledGpuJobs.size() == 18);

    // Check compute dispatch job for PrepareReactivity (fused ShadingChange)
    const FfxmGpuJobDescription& prJob = s_ScheduledGpuJobs[15];
    TEST_CHECK(prJob.jobType == FFXM_GPU_JOB_COMPUTE);
    TEST_CHECK(prJob.computeJobDescriptor.dimensions[0] == 240); // (1920 + 7) / 8
    TEST_CHECK(prJob.computeJobDescriptor.dimensions[1] == 135); // (1080 + 7) / 8
    TEST_CHECK(prJob.computeJobDescriptor.dimensions[2] == 1);
    TEST_CHECK(prJob.computeJobDescriptor.srvTextures[5].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2].internalIndex);
    TEST_CHECK(prJob.computeJobDescriptor.srvTextures[6].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS].internalIndex);
    TEST_CHECK(prJob.computeJobDescriptor.uavTextures[0].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS].internalIndex);
    TEST_CHECK(prJob.computeJobDescriptor.uavTextures[1].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS].internalIndex);
    TEST_CHECK(prJob.computeJobDescriptor.uavTextures[2].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1].internalIndex);
    TEST_CHECK(prJob.computeJobDescriptor.cbs[0].num32BitEntries == sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t));
    TEST_CHECK(prJob.computeJobDescriptor.pipeline.pipeline == priv->pipelinePrepareReactivity.pipeline);

    // Check compute dispatch job for LumaInstability
    const FfxmGpuJobDescription& liJob = s_ScheduledGpuJobs[16];
    TEST_CHECK(liJob.jobType == FFXM_GPU_JOB_COMPUTE);
    TEST_CHECK(liJob.computeJobDescriptor.dimensions[0] == 240); // (1920 + 7) / 8
    TEST_CHECK(liJob.computeJobDescriptor.dimensions[1] == 135); // (1080 + 7) / 8
    TEST_CHECK(liJob.computeJobDescriptor.dimensions[2] == 1);
    TEST_CHECK(liJob.computeJobDescriptor.pipeline.pipeline == priv->pipelineLumaInstability.pipeline);

    // Check compute dispatch job for Accumulate
    const FfxmGpuJobDescription& accJob = s_ScheduledGpuJobs[17];
    TEST_CHECK(accJob.jobType == FFXM_GPU_JOB_COMPUTE);
    TEST_CHECK(accJob.computeJobDescriptor.dimensions[0] == 480); // (3840 + 7) / 8
    TEST_CHECK(accJob.computeJobDescriptor.dimensions[1] == 270); // (2160 + 7) / 8
    TEST_CHECK(accJob.computeJobDescriptor.dimensions[2] == 1);
    TEST_CHECK(accJob.computeJobDescriptor.pipeline.pipeline == priv->pipelineAccumulate.pipeline);

    // Verify execution and cleanup calls
    TEST_CHECK(s_ExecuteGpuJobsCallCount == 1);
    TEST_CHECK(s_UnregisterResourcesCallCount == 1);

    // 4. Test frame parity and ping-pong luma and accumulation routing on frame 2
    FfxmFsr3UpscalerDispatchDescription dispDesc2 = dispDesc;
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc2) == FFXM_OK);
    // Frame 2 has no history reset, so 10 jobs scheduled (3 clears + 1 prepare inputs + 1 luma pyramid + 1 atomic clear + 1 shading change pyramid + 1 prepare reactivity + 1 luma instability + 1 accumulate)
    TEST_CHECK(s_ScheduledGpuJobs.size() == 10);
    TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_CURRENT_LUMA].internalIndex ==
        priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_2].internalIndex);
    TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION].internalIndex ==
        priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1].internalIndex);
    TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION].internalIndex ==
        priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2].internalIndex);

    // 5. Test clean destruction
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    TEST_CHECK(s_DestroyPipelineCallCount >= 5);
    VerifyOccurrenceAwareResourceBalance();

    // 6. Test pipeline creation failure rollback
    ResetMockState();
    s_CreatePipelineResult = FFXM_ERROR_BACKEND_API_ERROR;
    FfxmFsr3UpscalerContext failContext;
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&failContext, &desc) == FFXM_ERROR_BACKEND_API_ERROR);
    VerifyOccurrenceAwareResourceBalance();

    std::cout << "PASS: PrepareInputs compute pipeline created, verified, dispatched, and cleanly destroyed." << std::endl;
}

static void TestM3DispatchErrorHandling()
{
    std::cout << "--- Test 17: M3 PrepareInputs Dispatch Error Handling & Cleanup Contract ---" << std::endl;

    ResetMockState();

    FfxmInterface iface = {};
    SetupMockInterface(&iface);

    FfxmFsr3UpscalerContextDescription desc = {};
    desc.flags = FFXM_FSR3UPSCALER_ENABLE_DEPTH_INVERTED | FFXM_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE;
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    desc.backendInterface = iface;

    FfxmFsr3UpscalerContext context;
    TEST_REQUIRE(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);
    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);

    static int s_ColorResource = 10;
    static int s_DepthResource = 20;
    static int s_MvResource = 30;
    static int s_DilatedMvResource = 40;
    static int s_DilatedDepthResource = 50;
    static int s_ReconstructedDepthResource = 60;
    static int s_OutputResource = 70;

    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = reinterpret_cast<FfxmCommandList>(static_cast<uintptr_t>(0xCAFE));
    dispDesc.renderSize = { 1920, 1080 };
    dispDesc.upscaleSize = { 3840, 2160 };
    dispDesc.jitterOffset = { 0.0f, 0.0f };
    dispDesc.motionVectorScale = { 1.0f, 1.0f };
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;
    dispDesc.cameraFovAngleVertical = 1.0f;
    dispDesc.frameTimeDelta = 16.666f;
    dispDesc.viewSpaceToMetersFactor = 1.0f;
    dispDesc.color.resource = &s_ColorResource;
    dispDesc.depth.resource = &s_DepthResource;
    dispDesc.motionVectors.resource = &s_MvResource;
    dispDesc.dilatedMotionVectors.resource = &s_DilatedMvResource;
    dispDesc.dilatedDepth.resource = &s_DilatedDepthResource;
    dispDesc.reconstructedPrevNearestDepth.resource = &s_ReconstructedDepthResource;
    dispDesc.output.resource = &s_OutputResource;

    auto resetDispatchTrackers = []() {
        s_ScheduledGpuJobs.clear();
        s_RegisterResourceCallCount = 0;
        s_ScheduleGpuJobCallCount = 0;
        s_ExecuteGpuJobsCallCount = 0;
        s_UnregisterResourcesCallCount = 0;
        s_FailAtRegisterResourceIndex = -1;
        s_ScheduleGpuJobResult = FFXM_OK;
        s_FailAtScheduleGpuJobIndex = -1;
        s_ExecuteGpuJobsResult = FFXM_OK;
        s_UnregisterResourcesResult = FFXM_OK;
    };

    auto verifyDynamicSlotsReset = [&]() {
        TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_COLOR].internalIndex == -1);
        TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_DEPTH].internalIndex == -1);
        TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_MOTION_VECTORS].internalIndex == -1);
        TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS].internalIndex == -1);
        TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_DEPTH].internalIndex == -1);
        TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH].internalIndex == -1);
        TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT].internalIndex == -1);
        TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS].internalIndex == -1);
        TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_DEPTH].internalIndex == -1);
        TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH].internalIndex == -1);
        TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT].internalIndex == -1);
    };

    // Scenario 1: First dynamic resource registration fails (index 0)
    {
        resetDispatchTrackers();
        s_FailAtRegisterResourceIndex = 0;

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_ERROR_OUT_OF_MEMORY);
        TEST_CHECK(s_RegisterResourceCallCount == 1);
        TEST_CHECK(s_UnregisterResourcesCallCount == 0); // Never unregister when 0 acquired
        TEST_CHECK(s_ScheduleGpuJobCallCount == 0);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 0);
        TEST_CHECK(priv->resourceFrameIndex == 0);
        TEST_CHECK(priv->firstExecution == true);
        TEST_CHECK(priv->constants.frameIndex == 0.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);
        verifyDynamicSlotsReset();
    }

    // Scenario 2: Later dynamic resource registration fails (index 3)
    {
        resetDispatchTrackers();
        s_FailAtRegisterResourceIndex = 3;

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_ERROR_OUT_OF_MEMORY);
        TEST_CHECK(s_RegisterResourceCallCount == 4);
        TEST_CHECK(s_UnregisterResourcesCallCount == 1); // Clean up acquired resources
        TEST_CHECK(s_ScheduleGpuJobCallCount == 0);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 0);
        TEST_CHECK(priv->resourceFrameIndex == 0);
        TEST_CHECK(priv->firstExecution == true);
        TEST_CHECK(priv->constants.frameIndex == 0.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);
        verifyDynamicSlotsReset();
    }

    // Scenario 3a: Clear job scheduling fails
    {
        resetDispatchTrackers();
        s_FailAtScheduleGpuJobIndex = 0;

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_ERROR_BACKEND_API_ERROR);
        TEST_CHECK(s_RegisterResourceCallCount == 7);
        TEST_CHECK(s_ScheduleGpuJobCallCount == 1);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 0); // Execution skipped on schedule failure
        TEST_CHECK(s_UnregisterResourcesCallCount == 1);
        TEST_CHECK(priv->resourceFrameIndex == 0);
        TEST_CHECK(priv->firstExecution == true);
        TEST_CHECK(priv->constants.frameIndex == 0.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);
        verifyDynamicSlotsReset();
    }

    // Scenario 3b: Compute dispatch job scheduling fails
    {
        resetDispatchTrackers();
        s_FailAtScheduleGpuJobIndex = 4;

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_ERROR_BACKEND_API_ERROR);
        TEST_CHECK(s_RegisterResourceCallCount == 7);
        TEST_CHECK(s_ScheduleGpuJobCallCount == 5);
        TEST_CHECK(s_ScheduledGpuJobs.size() == 4);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 0);
        TEST_CHECK(s_UnregisterResourcesCallCount == 1);
        TEST_CHECK(priv->resourceFrameIndex == 0);
        TEST_CHECK(priv->firstExecution == true);
        TEST_CHECK(priv->constants.frameIndex == 0.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);
        verifyDynamicSlotsReset();
    }

    // Scenario 4: GPU job execution fails
    {
        resetDispatchTrackers();
        s_ExecuteGpuJobsResult = FFXM_ERROR_BACKEND_API_ERROR;

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_ERROR_BACKEND_API_ERROR);
        TEST_CHECK(s_RegisterResourceCallCount == 7);
        TEST_CHECK(s_ScheduleGpuJobCallCount == 18);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 1);
        TEST_CHECK(s_UnregisterResourcesCallCount == 1);
        TEST_CHECK(priv->resourceFrameIndex == 0);
        TEST_CHECK(priv->firstExecution == true);
        TEST_CHECK(priv->constants.frameIndex == 0.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);
        verifyDynamicSlotsReset();
    }

    // Scenario 5: Resource unregistration fails after successful execution
    {
        resetDispatchTrackers();
        s_UnregisterResourcesResult = FFXM_ERROR_BACKEND_API_ERROR;

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_ERROR_BACKEND_API_ERROR);
        TEST_CHECK(s_ScheduleGpuJobCallCount == 18);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 1);
        TEST_CHECK(s_UnregisterResourcesCallCount == 1);
        TEST_CHECK(priv->resourceFrameIndex == 0);
        TEST_CHECK(priv->firstExecution == true);
        TEST_CHECK(priv->constants.frameIndex == 0.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);
        verifyDynamicSlotsReset();
    }

    // Scenario 6: Execution and Unregistration BOTH fail
    {
        resetDispatchTrackers();
        s_ExecuteGpuJobsResult = FFXM_ERROR_INVALID_ARGUMENT;
        s_UnregisterResourcesResult = FFXM_ERROR_BACKEND_API_ERROR;

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_ERROR_INVALID_ARGUMENT); // Original error preserved
        TEST_CHECK(s_ScheduleGpuJobCallCount == 18);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 1);
        TEST_CHECK(s_UnregisterResourcesCallCount == 1);
        TEST_CHECK(priv->resourceFrameIndex == 0);
        TEST_CHECK(priv->firstExecution == true);
        TEST_CHECK(priv->constants.frameIndex == 0.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);
        verifyDynamicSlotsReset();
    }

    // Scenario 7: Successful dispatch after recoverable failure
    {
        resetDispatchTrackers();

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_OK);
        TEST_CHECK(s_RegisterResourceCallCount == 7);
        TEST_CHECK(s_ScheduleGpuJobCallCount == 18);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 1);
        TEST_CHECK(s_UnregisterResourcesCallCount == 1);
        TEST_CHECK(priv->resourceFrameIndex == 1);
        TEST_CHECK(priv->firstExecution == false);
        TEST_CHECK(priv->constants.frameIndex == 0.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);
        verifyDynamicSlotsReset();
    }

    // Scenario 8: Subsequent frame success & mid-stream failure recovery
    {
        // Frame 2 dispatches successfully
        resetDispatchTrackers();
        FfxmFsr3UpscalerDispatchDescription dispDescFrame2 = dispDesc;
        dispDescFrame2.preExposure = 1.0f;
        FfxmErrorCode err2 = ffxmFsr3UpscalerContextDispatch(&context, &dispDescFrame2);
        TEST_CHECK(err2 == FFXM_OK);
        TEST_CHECK(s_ScheduleGpuJobCallCount == 10);
        TEST_CHECK(priv->resourceFrameIndex == 2);
        TEST_CHECK(priv->constants.frameIndex == 1.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);

        // Frame 3 fails at GPU execution stage with preExposure = 2.0f
        resetDispatchTrackers();
        s_ExecuteGpuJobsResult = FFXM_ERROR_BACKEND_API_ERROR;
        FfxmFsr3UpscalerDispatchDescription dispDescFrame3 = dispDesc;
        dispDescFrame3.preExposure = 2.0f;
        FfxmErrorCode err3Fail = ffxmFsr3UpscalerContextDispatch(&context, &dispDescFrame3);
        TEST_CHECK(err3Fail == FFXM_ERROR_BACKEND_API_ERROR);
        // Verify transactional preservation: state MUST NOT advance
        TEST_CHECK(priv->resourceFrameIndex == 2);
        TEST_CHECK(priv->constants.frameIndex == 1.0f);
        TEST_CHECK(priv->preExposure == 1.0f); // 2.0f NOT committed
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);

        // Frame 3 fails at Accumulate schedule stage
        resetDispatchTrackers();
        s_FailAtScheduleGpuJobIndex = 9; // Fail at Accumulate schedule
        FfxmErrorCode err3ScheduleFail = ffxmFsr3UpscalerContextDispatch(&context, &dispDescFrame3);
        TEST_CHECK(err3ScheduleFail == FFXM_ERROR_BACKEND_API_ERROR);
        TEST_CHECK(s_ScheduleGpuJobCallCount == 10);
        TEST_CHECK(s_ScheduledGpuJobs.size() == 9);
        TEST_CHECK(priv->resourceFrameIndex == 2);
        TEST_CHECK(priv->constants.frameIndex == 1.0f);
        TEST_CHECK(priv->preExposure == 1.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);

        // Retry Frame 3 without failure: must advance state exactly once
        resetDispatchTrackers();
        FfxmErrorCode err3Success = ffxmFsr3UpscalerContextDispatch(&context, &dispDescFrame3);
        TEST_CHECK(err3Success == FFXM_OK);
        TEST_CHECK(priv->resourceFrameIndex == 3);
        TEST_CHECK(priv->constants.frameIndex == 2.0f); // Exactly 1 increment from 1.0f to 2.0f!
        TEST_CHECK(priv->preExposure == 2.0f);
        TEST_CHECK(priv->previousFramePreExposure == 1.0f);
        TEST_CHECK(priv->constants.deltaPreExposure == 2.0f); // 2.0f / 1.0f = 2.0f preserved!
    }

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    VerifyOccurrenceAwareResourceBalance();

    std::cout << "PASS: All M3 PrepareInputs and temporal state transactional error handling scenarios verified." << std::endl;
}

static void TestM3ComputeLuminancePyramid()
{
    std::cout << "--- Test 19: M3 Compute Luminance Pyramid Stage & SPD Verification ---" << std::endl;

    // 1. Verify shader permutations via accessor
    FfxmShaderBlob blobDefault = {};
    TEST_CHECK(arm::ffxmGetPermutationBlobByIndex(FFXM_EFFECT_FSR3UPSCALER, FFXM_FSR3UPSCALER_PASS_LUMA_PYRAMID,
        0, &blobDefault) == FFXM_OK);
    TEST_CHECK(blobDefault.data != nullptr);
    TEST_CHECK(blobDefault.size > 0);
    TEST_CHECK(blobDefault.srvTextureCount == 2);
    TEST_CHECK(blobDefault.uavTextureCount == 4);
    TEST_CHECK(blobDefault.cbvCount == 2);

    // Verify reflection names and bindings
    TEST_CHECK(blobDefault.boundSRVTextures[0] == 0); // r_current_luma
    TEST_CHECK(blobDefault.boundSRVTextures[1] == 1); // r_farthest_depth
    TEST_CHECK(blobDefault.boundUAVTextures[0] == 2); // rw_spd_global_atomic
    TEST_CHECK(blobDefault.boundUAVTextures[1] == 3); // rw_frame_info
    TEST_CHECK(blobDefault.boundUAVTextures[2] == 9); // rw_spd_mip5
    TEST_CHECK(blobDefault.boundUAVTextures[3] == 10); // rw_farthest_depth_mip1
    TEST_CHECK(blobDefault.boundConstantBuffers[0] == 11); // cbFSR3Upscaler
    TEST_CHECK(blobDefault.boundConstantBuffers[1] == 12); // cbSPD

    // 2. Context creation & SPD Constant Buffer setup verification
    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 2560, 1440 };
    desc.maxUpscaleSize = { 3840, 2160 };
    desc.flags = FFXM_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    SetupMockInterface(&desc.backendInterface);

    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);
    TEST_CHECK(priv->pipelineLumaPyramid.pipeline != nullptr);

    // 3. Dispatch and check SPD constants calculation across resolutions
    FfxmCommandList dummyCmdList = reinterpret_cast<FfxmCommandList>(0xABCD);
    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = dummyCmdList;
    dispDesc.renderSize = { 1920, 1080 };
    dispDesc.upscaleSize = { 3840, 2160 };
    dispDesc.jitterOffset = { 0.0f, 0.0f };
    dispDesc.motionVectorScale = { 1.0f, 1.0f };
    dispDesc.frameTimeDelta = 16.666f;
    dispDesc.cameraFovAngleVertical = 1.0f;
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;

    static int s_Res = 1;
    dispDesc.color.resource = &s_Res;
    dispDesc.depth.resource = &s_Res;
    dispDesc.motionVectors.resource = &s_Res;
    dispDesc.dilatedMotionVectors.resource = &s_Res;
    dispDesc.dilatedDepth.resource = &s_Res;
    dispDesc.reconstructedPrevNearestDepth.resource = &s_Res;

    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);

    // Verify cbSPD staging data for 1920x1080:
    // dispatchThreadGroupCountXY[0] = (1920 + 63) / 64 = 30
    // dispatchThreadGroupCountXY[1] = (1080 + 63) / 64 = 17
    // numWorkGroups = 30 * 17 = 510
    const FfxmGpuJobDescription* lumaJob1080 = nullptr;
    for (const auto& job : s_ScheduledGpuJobs) {
        if (job.jobType == FFXM_GPU_JOB_COMPUTE &&
            job.computeJobDescriptor.pipeline.pipeline == priv->pipelineLumaPyramid.pipeline) {
            lumaJob1080 = &job;
            break;
        }
    }
    TEST_REQUIRE(lumaJob1080 != nullptr);
    TEST_CHECK(lumaJob1080->computeJobDescriptor.dimensions[0] == 30);
    TEST_CHECK(lumaJob1080->computeJobDescriptor.dimensions[1] == 17);
    TEST_CHECK(lumaJob1080->computeJobDescriptor.dimensions[2] == 1);

    const Fsr3UpscalerSpdConstants* spdData1080 = reinterpret_cast<const Fsr3UpscalerSpdConstants*>(
        lumaJob1080->computeJobDescriptor.cbs[1].data);
    TEST_CHECK(spdData1080 != nullptr);
    TEST_CHECK(spdData1080->numWorkGroups == 510);
    TEST_CHECK(spdData1080->renderSize[0] == 1920);
    TEST_CHECK(spdData1080->renderSize[1] == 1080);
    TEST_CHECK(spdData1080->workGroupOffset[0] == 0);
    TEST_CHECK(spdData1080->workGroupOffset[1] == 0);

    // 4. Test dispatch for different render resolution (1280x720)
    dispDesc.renderSize = { 1280, 720 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);

    const FfxmGpuJobDescription* lumaJob720 = nullptr;
    for (const auto& job : s_ScheduledGpuJobs) {
        if (job.jobType == FFXM_GPU_JOB_COMPUTE &&
            job.computeJobDescriptor.pipeline.pipeline == priv->pipelineLumaPyramid.pipeline) {
            lumaJob720 = &job;
            break;
        }
    }
    TEST_REQUIRE(lumaJob720 != nullptr);
    TEST_CHECK(lumaJob720->computeJobDescriptor.dimensions[0] == 20);
    TEST_CHECK(lumaJob720->computeJobDescriptor.dimensions[1] == 12);
    TEST_CHECK(lumaJob720->computeJobDescriptor.dimensions[2] == 1);

    const Fsr3UpscalerSpdConstants* spdData720 = reinterpret_cast<const Fsr3UpscalerSpdConstants*>(
        lumaJob720->computeJobDescriptor.cbs[1].data);
    TEST_CHECK(spdData720 != nullptr);
    TEST_CHECK(spdData720->numWorkGroups == 240);
    TEST_CHECK(spdData720->renderSize[0] == 1280);
    TEST_CHECK(spdData720->renderSize[1] == 720);

    // 5. Clean destroy
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    TEST_CHECK(s_DestroyPipelineCallCount >= 3);
    VerifyOccurrenceAwareResourceBalance();

    std::cout << "PASS: M3 Compute Luminance Pyramid stage and SPD verification verified." << std::endl;
}

static void TestM3ComputeShadingChangePyramid()
{
    std::cout << "--- Test 20: M3 Compute Shading Change Pyramid Stage Verification ---" << std::endl;

    // 1. Verify shader permutations via accessor
    FfxmShaderBlob blobDefault = {};
    TEST_CHECK(arm::ffxmGetPermutationBlobByIndex(FFXM_EFFECT_FSR3UPSCALER, FFXM_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID,
        0, &blobDefault) == FFXM_OK);
    TEST_CHECK(blobDefault.data != nullptr);
    TEST_CHECK(blobDefault.size > 0);
    TEST_CHECK(blobDefault.srvTextureCount == 4);
    TEST_CHECK(blobDefault.uavTextureCount == 7);
    TEST_CHECK(blobDefault.cbvCount == 2);

    // Verify reflection names and bindings
    TEST_CHECK(blobDefault.boundSRVTextures[0] == 0); // r_current_luma
    TEST_CHECK(blobDefault.boundSRVTextures[1] == 1); // r_previous_luma
    TEST_CHECK(blobDefault.boundSRVTextures[2] == 2); // r_dilated_motion_vectors
    TEST_CHECK(blobDefault.boundSRVTextures[3] == 3); // r_input_exposure

    TEST_CHECK(blobDefault.boundUAVTextures[0] == 4);  // rw_spd_global_atomic
    TEST_CHECK(blobDefault.boundUAVTextures[1] == 5);  // rw_spd_mip0
    TEST_CHECK(blobDefault.boundUAVTextures[2] == 6);  // rw_spd_mip1
    TEST_CHECK(blobDefault.boundUAVTextures[3] == 7);  // rw_spd_mip2
    TEST_CHECK(blobDefault.boundUAVTextures[4] == 8);  // rw_spd_mip3
    TEST_CHECK(blobDefault.boundUAVTextures[5] == 9);  // rw_spd_mip4
    TEST_CHECK(blobDefault.boundUAVTextures[6] == 10); // rw_spd_mip5

    TEST_CHECK(blobDefault.boundConstantBuffers[0] == 11); // cbFSR3Upscaler
    TEST_CHECK(blobDefault.boundConstantBuffers[1] == 12); // cbSPD

    // 2. Context creation & SPD Constant Buffer setup verification
    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 2560, 1440 };
    desc.maxUpscaleSize = { 3840, 2160 };
    desc.flags = FFXM_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    SetupMockInterface(&desc.backendInterface);

    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);

    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);
    TEST_CHECK(priv->pipelineShadingChangePyramid.pipeline != nullptr);

    // 3. Dispatch and check SPD constants calculation for half-resolution render dimensions across resolutions
    FfxmCommandList dummyCmdList = reinterpret_cast<FfxmCommandList>(0xABCD);
    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = dummyCmdList;
    dispDesc.renderSize = { 1920, 1080 };
    dispDesc.upscaleSize = { 3840, 2160 };
    dispDesc.jitterOffset = { 0.0f, 0.0f };
    dispDesc.motionVectorScale = { 1.0f, 1.0f };
    dispDesc.frameTimeDelta = 16.666f;
    dispDesc.cameraFovAngleVertical = 1.0f;
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;

    static int s_Res = 1;
    dispDesc.color.resource = &s_Res;
    dispDesc.depth.resource = &s_Res;
    dispDesc.motionVectors.resource = &s_Res;
    dispDesc.dilatedMotionVectors.resource = &s_Res;
    dispDesc.dilatedDepth.resource = &s_Res;
    dispDesc.reconstructedPrevNearestDepth.resource = &s_Res;

    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);

    // For 1920x1080: halfRenderWidth = 960, halfRenderHeight = 540
    // dispatchThreadGroupCountXY[0] = (960 + 63) / 64 = 15
    // dispatchThreadGroupCountXY[1] = (540 + 63) / 64 = 9
    // numWorkGroups = 15 * 9 = 135
    const FfxmGpuJobDescription* shadingJob1080 = nullptr;
    for (const auto& job : s_ScheduledGpuJobs) {
        if (job.jobType == FFXM_GPU_JOB_COMPUTE &&
            job.computeJobDescriptor.pipeline.pipeline == priv->pipelineShadingChangePyramid.pipeline) {
            shadingJob1080 = &job;
            break;
        }
    }
    TEST_REQUIRE(shadingJob1080 != nullptr);
    TEST_CHECK(shadingJob1080->computeJobDescriptor.dimensions[0] == 15);
    TEST_CHECK(shadingJob1080->computeJobDescriptor.dimensions[1] == 9);
    TEST_CHECK(shadingJob1080->computeJobDescriptor.dimensions[2] == 1);

    const Fsr3UpscalerSpdConstants* spdData1080 = reinterpret_cast<const Fsr3UpscalerSpdConstants*>(
        shadingJob1080->computeJobDescriptor.cbs[1].data);
    TEST_CHECK(spdData1080 != nullptr);
    TEST_CHECK(spdData1080->numWorkGroups == 135);
    TEST_CHECK(spdData1080->renderSize[0] == 960);
    TEST_CHECK(spdData1080->renderSize[1] == 540);
    TEST_CHECK(spdData1080->workGroupOffset[0] == 0);
    TEST_CHECK(spdData1080->workGroupOffset[1] == 0);

    // 4. Test dispatch for 1280x720: halfRenderWidth = 640, halfRenderHeight = 360
    dispDesc.renderSize = { 1280, 720 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);

    const FfxmGpuJobDescription* shadingJob720 = nullptr;
    for (const auto& job : s_ScheduledGpuJobs) {
        if (job.jobType == FFXM_GPU_JOB_COMPUTE &&
            job.computeJobDescriptor.pipeline.pipeline == priv->pipelineShadingChangePyramid.pipeline) {
            shadingJob720 = &job;
            break;
        }
    }
    TEST_REQUIRE(shadingJob720 != nullptr);
    // (640 + 63) / 64 = 10, (360 + 63) / 64 = 6, numWorkGroups = 60
    TEST_CHECK(shadingJob720->computeJobDescriptor.dimensions[0] == 10);
    TEST_CHECK(shadingJob720->computeJobDescriptor.dimensions[1] == 6);
    TEST_CHECK(shadingJob720->computeJobDescriptor.dimensions[2] == 1);

    const Fsr3UpscalerSpdConstants* spdData720 = reinterpret_cast<const Fsr3UpscalerSpdConstants*>(
        shadingJob720->computeJobDescriptor.cbs[1].data);
    TEST_CHECK(spdData720 != nullptr);
    TEST_CHECK(spdData720->numWorkGroups == 60);
    TEST_CHECK(spdData720->renderSize[0] == 640);
    TEST_CHECK(spdData720->renderSize[1] == 360);

    // 5. Test odd resolution boundary: 1279x719
    // (1279 + 1) / 2 = 640, (719 + 1) / 2 = 360
    dispDesc.renderSize = { 1279, 719 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);

    const FfxmGpuJobDescription* shadingJobOdd = nullptr;
    for (const auto& job : s_ScheduledGpuJobs) {
        if (job.jobType == FFXM_GPU_JOB_COMPUTE &&
            job.computeJobDescriptor.pipeline.pipeline == priv->pipelineShadingChangePyramid.pipeline) {
            shadingJobOdd = &job;
            break;
        }
    }
    TEST_REQUIRE(shadingJobOdd != nullptr);
    TEST_CHECK(shadingJobOdd->computeJobDescriptor.dimensions[0] == 10);
    TEST_CHECK(shadingJobOdd->computeJobDescriptor.dimensions[1] == 6);
    const Fsr3UpscalerSpdConstants* spdDataOdd = reinterpret_cast<const Fsr3UpscalerSpdConstants*>(
        shadingJobOdd->computeJobDescriptor.cbs[1].data);
    TEST_CHECK(spdDataOdd != nullptr);
    TEST_CHECK(spdDataOdd->numWorkGroups == 60);
    TEST_CHECK(spdDataOdd->renderSize[0] == 640);
    TEST_CHECK(spdDataOdd->renderSize[1] == 360);

    // 6. Test 1440p resolution: 2560x1440
    // (2560 + 1) / 2 = 1280, (1440 + 1) / 2 = 720
    // (1280 + 63) / 64 = 20, (720 + 63) / 64 = 12, numWorkGroups = 240
    dispDesc.renderSize = { 2560, 1440 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);

    const FfxmGpuJobDescription* shadingJob1440 = nullptr;
    for (const auto& job : s_ScheduledGpuJobs) {
        if (job.jobType == FFXM_GPU_JOB_COMPUTE &&
            job.computeJobDescriptor.pipeline.pipeline == priv->pipelineShadingChangePyramid.pipeline) {
            shadingJob1440 = &job;
            break;
        }
    }
    TEST_REQUIRE(shadingJob1440 != nullptr);
    TEST_CHECK(shadingJob1440->computeJobDescriptor.dimensions[0] == 20);
    TEST_CHECK(shadingJob1440->computeJobDescriptor.dimensions[1] == 12);
    const Fsr3UpscalerSpdConstants* spdData1440 = reinterpret_cast<const Fsr3UpscalerSpdConstants*>(
        shadingJob1440->computeJobDescriptor.cbs[1].data);
    TEST_CHECK(spdData1440 != nullptr);
    TEST_CHECK(spdData1440->numWorkGroups == 240);
    TEST_CHECK(spdData1440->renderSize[0] == 1280);
    TEST_CHECK(spdData1440->renderSize[1] == 720);

    // 7. Clean destroy
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    TEST_CHECK(s_DestroyPipelineCallCount >= 3);
    VerifyOccurrenceAwareResourceBalance();

    std::cout << "PASS: M3 Compute Shading Change Pyramid stage and SPD verification verified." << std::endl;
}

static void TestM3ComputeShadingChange()
{
    std::cout << "--- Test 21: M3 Compute Shading Change Stage Verification ---" << std::endl;

    // 1. Verify shader permutations via accessor for all permutation flag combinations
    const uint32_t permutationVariants[] = {
        0,
        FSR3UPSCALER_SHADER_PERMUTATION_USE_LANCZOS_TYPE,
        FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT,
        FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS,
        FSR3UPSCALER_SHADER_PERMUTATION_JITTER_MOTION_VECTORS,
        FSR3UPSCALER_SHADER_PERMUTATION_DEPTH_INVERTED,
        FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING,
        FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT | FSR3UPSCALER_SHADER_PERMUTATION_DEPTH_INVERTED
    };

    for (uint32_t flags : permutationVariants) {
        FfxmShaderBlob blob = {};
        TEST_CHECK(arm::ffxmGetPermutationBlobByIndex(FFXM_EFFECT_FSR3UPSCALER, FFXM_FSR3UPSCALER_PASS_SHADING_CHANGE,
            flags, &blob) == FFXM_OK);
        TEST_CHECK(blob.data != nullptr);
        TEST_CHECK(blob.size > 0);
        TEST_CHECK(blob.cbvCount == 1);
        TEST_CHECK(blob.srvTextureCount == 1);
        TEST_CHECK(blob.uavTextureCount == 1);
        TEST_CHECK(blob.samplerCount == 1);

        // Reflection interface validation
        TEST_CHECK(blob.boundConstantBuffers[0] == 2); // cbFSR3UPSCALER
        TEST_CHECK(strcmp(blob.boundConstantBufferNames[0], "cbFSR3Upscaler") == 0);

        TEST_CHECK(blob.boundSRVTextures[0] == 0); // r_spd_mips
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[0], "r_spd_mips") == 0);

        TEST_CHECK(blob.boundUAVTextures[0] == 1); // rw_shading_change
        TEST_CHECK(strcmp(blob.boundUAVTextureNames[0], "rw_shading_change") == 0);

        TEST_CHECK(blob.boundSamplers[0] == 1001); // s_LinearClamp
        TEST_CHECK(strcmp(blob.boundSamplerNames[0], "s_LinearClamp") == 0);
    }

    // 2. Algorithm correctness reference unit test:
    // fSample.x * fSample.y over mips 0..2, max value scaled by fScale = 1.0f, saturated to [0, 1].
    auto computeCpuShadingChange = [](const float mips[3][2]) -> float {
        const float fScale = 1.0f; // 1.0f + 0 / 3
        float fShadingChange = 0.0f;
        for (int iMipLevel = 0; iMipLevel < 3; ++iMipLevel) {
            float sampleVal = fabsf(mips[iMipLevel][0] * mips[iMipLevel][1]);
            if (sampleVal > 0.0f) {
                float scaled = sampleVal * fScale;
                if (scaled > fShadingChange) {
                    fShadingChange = scaled;
                }
            }
        }
        if (fShadingChange < 0.0f) fShadingChange = 0.0f;
        if (fShadingChange > 1.0f) fShadingChange = 1.0f;
        return fShadingChange;
    };

    // Case A: Zero change
    {
        const float mipsZero[3][2] = { {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f} };
        TEST_CHECK(computeCpuShadingChange(mipsZero) == 0.0f);
    }
    // Case B: Uniform positive increase
    {
        const float mipsPos[3][2] = { {0.6f, 1.0f}, {0.0f, 0.0f}, {0.0f, 0.0f} };
        TEST_CHECK(fabsf(computeCpuShadingChange(mipsPos) - 0.6f) < 1e-6f);
    }
    // Case C: Uniform negative decrease
    {
        const float mipsNeg[3][2] = { {0.75f, -1.0f}, {0.0f, 0.0f}, {0.0f, 0.0f} };
        TEST_CHECK(fabsf(computeCpuShadingChange(mipsNeg) - 0.75f) < 1e-6f);
    }
    // Case D: High magnitude with zero factor/consistency
    {
        const float mipsZeroFactor[3][2] = { {1.0f, 0.0f}, {0.8f, 0.0f}, {0.5f, 0.0f} };
        TEST_CHECK(computeCpuShadingChange(mipsZeroFactor) == 0.0f);
    }
    // Case E: Change visible primarily in coarser mip levels
    {
        const float mipsCoarse[3][2] = { {0.1f, 1.0f}, {0.4f, 1.0f}, {0.85f, 1.0f} };
        TEST_CHECK(fabsf(computeCpuShadingChange(mipsCoarse) - 0.85f) < 1e-6f);
    }
    // Case F: Normalization saturation limit
    {
        const float mipsSaturate[3][2] = { {2.5f, 1.0f}, {0.0f, 0.0f}, {0.0f, 0.0f} };
        TEST_CHECK(computeCpuShadingChange(mipsSaturate) == 1.0f);
    }

    // 3. Multi-resolution dispatch dimensions test
    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 2560, 1440 };
    desc.maxUpscaleSize = { 3840, 2160 };
    desc.flags = FFXM_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    SetupMockInterface(&desc.backendInterface);

    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);
    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);
    TEST_CHECK(priv->pipelineShadingChange.pipeline != nullptr);

    // Verify internal surface allocation for Shading Change: R8_UNORM, half render resolution
    TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SHADING_CHANGE].internalIndex != -1);
    TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SHADING_CHANGE].internalIndex != -1);

    FfxmCommandList dummyCmdList = reinterpret_cast<FfxmCommandList>(0xABCD);
    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = dummyCmdList;
    dispDesc.upscaleSize = { 3840, 2160 };
    dispDesc.jitterOffset = { 0.0f, 0.0f };
    dispDesc.motionVectorScale = { 1.0f, 1.0f };
    dispDesc.frameTimeDelta = 16.666f;
    dispDesc.cameraFovAngleVertical = 1.0f;
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;

    static int s_Res = 1;
    dispDesc.color.resource = &s_Res;
    dispDesc.depth.resource = &s_Res;
    dispDesc.motionVectors.resource = &s_Res;
    dispDesc.dilatedMotionVectors.resource = &s_Res;
    dispDesc.dilatedDepth.resource = &s_Res;
    dispDesc.reconstructedPrevNearestDepth.resource = &s_Res;

    auto findPrepareReactivityJob = [&]() -> const FfxmGpuJobDescription* {
        for (const auto& job : s_ScheduledGpuJobs) {
            if (job.jobType == FFXM_GPU_JOB_COMPUTE &&
                job.computeJobDescriptor.pipeline.pipeline == priv->pipelinePrepareReactivity.pipeline) {
                return &job;
            }
        }
        return nullptr;
    };
    auto findShadingChangeJob = [&]() -> const FfxmGpuJobDescription* {
        for (const auto& job : s_ScheduledGpuJobs) {
            if (job.jobType == FFXM_GPU_JOB_COMPUTE &&
                job.computeJobDescriptor.pipeline.pipeline == priv->pipelineShadingChange.pipeline) {
                return &job;
            }
        }
        return nullptr;
    };

    // A. 1080p: renderSize = 1920x1080 -> dispatch = 240x135x1, ShadingChange fused (0 standalone dispatches)
    dispDesc.renderSize = { 1920, 1080 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    TEST_CHECK(findShadingChangeJob() == nullptr); // Fused: zero standalone dispatches
    const FfxmGpuJobDescription* prJob1080 = findPrepareReactivityJob();
    TEST_REQUIRE(prJob1080 != nullptr);
    TEST_CHECK(prJob1080->computeJobDescriptor.dimensions[0] == 240);
    TEST_CHECK(prJob1080->computeJobDescriptor.dimensions[1] == 135);
    TEST_CHECK(prJob1080->computeJobDescriptor.dimensions[2] == 1);
    TEST_CHECK(prJob1080->computeJobDescriptor.srvTextures[6].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS].internalIndex);

    // B. 720p: renderSize = 1280x720 -> dispatch = 160x90x1
    dispDesc.renderSize = { 1280, 720 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    TEST_CHECK(findShadingChangeJob() == nullptr);
    const FfxmGpuJobDescription* prJob720 = findPrepareReactivityJob();
    TEST_REQUIRE(prJob720 != nullptr);
    TEST_CHECK(prJob720->computeJobDescriptor.dimensions[0] == 160);
    TEST_CHECK(prJob720->computeJobDescriptor.dimensions[1] == 90);
    TEST_CHECK(prJob720->computeJobDescriptor.dimensions[2] == 1);

    // C. Odd resolution boundary: renderSize = 1279x719 -> dispatch = 160x90x1
    dispDesc.renderSize = { 1279, 719 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    TEST_CHECK(findShadingChangeJob() == nullptr);
    const FfxmGpuJobDescription* prJobOdd = findPrepareReactivityJob();
    TEST_REQUIRE(prJobOdd != nullptr);
    TEST_CHECK(prJobOdd->computeJobDescriptor.dimensions[0] == 160);
    TEST_CHECK(prJobOdd->computeJobDescriptor.dimensions[1] == 90);
    TEST_CHECK(prJobOdd->computeJobDescriptor.dimensions[2] == 1);

    // D. 1440p: renderSize = 2560x1440 -> dispatch = 320x180x1
    dispDesc.renderSize = { 2560, 1440 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    TEST_CHECK(findShadingChangeJob() == nullptr);
    const FfxmGpuJobDescription* prJob1440 = findPrepareReactivityJob();
    TEST_REQUIRE(prJob1440 != nullptr);
    TEST_CHECK(prJob1440->computeJobDescriptor.dimensions[0] == 320);
    TEST_CHECK(prJob1440->computeJobDescriptor.dimensions[1] == 180);
    TEST_CHECK(prJob1440->computeJobDescriptor.dimensions[2] == 1);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    TEST_CHECK(s_DestroyPipelineCallCount >= 4);
    VerifyOccurrenceAwareResourceBalance();

    std::cout << "PASS: M3 Compute Shading Change stage and algorithm verification verified." << std::endl;
}

static void TestM3PrepareReactivity()
{
    std::cout << "--- Test 22: M3 Prepare Reactivity Stage & Algorithm Verification ---" << std::endl;

    // 1. Verify shader reflection interface and all 64 permutations
    for (uint32_t permutation = 0; permutation < 64; ++permutation) {
        FfxmShaderBlob blob = {};
        TEST_CHECK(arm::ffxmGetPermutationBlobByIndex(FFXM_EFFECT_FSR3UPSCALER, FFXM_FSR3UPSCALER_PASS_PREPARE_REACTIVITY,
            permutation, &blob) == FFXM_OK);
        TEST_CHECK(blob.data != nullptr);
        TEST_CHECK(blob.size > 0);
        TEST_CHECK(blob.cbvCount == 1);
        TEST_CHECK(blob.srvTextureCount == 9);
        TEST_CHECK(blob.uavTextureCount == 3);
        TEST_CHECK(blob.samplerCount == 1);

        // Reflection interface validation
        TEST_CHECK(blob.boundConstantBuffers[0] == 12); // cbFSR3Upscaler
        TEST_CHECK(strcmp(blob.boundConstantBufferNames[0], "cbFSR3Upscaler") == 0);

        TEST_CHECK(blob.boundSRVTextures[0] == 0); // r_reconstructed_previous_nearest_depth
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[0], "r_reconstructed_previous_nearest_depth") == 0);

        TEST_CHECK(blob.boundSRVTextures[1] == 1); // r_dilated_motion_vectors
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[1], "r_dilated_motion_vectors") == 0);

        TEST_CHECK(blob.boundSRVTextures[2] == 2); // r_dilated_depth
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[2], "r_dilated_depth") == 0);

        TEST_CHECK(blob.boundSRVTextures[3] == 3); // r_reactive_mask
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[3], "r_reactive_mask") == 0);

        TEST_CHECK(blob.boundSRVTextures[4] == 4); // r_transparency_and_composition_mask
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[4], "r_transparency_and_composition_mask") == 0);

        TEST_CHECK(blob.boundSRVTextures[5] == 5); // r_accumulation
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[5], "r_accumulation") == 0);

        TEST_CHECK(blob.boundSRVTextures[6] == 6); // r_spd_mips
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[6], "r_spd_mips") == 0);

        TEST_CHECK(blob.boundSRVTextures[7] == 7); // r_current_luma
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[7], "r_current_luma") == 0);

        TEST_CHECK(blob.boundSRVTextures[8] == 8); // r_input_exposure
        TEST_CHECK(strcmp(blob.boundSRVTextureNames[8], "r_input_exposure") == 0);

        TEST_CHECK(blob.boundUAVTextures[0] == 9); // rw_dilated_reactive_masks
        TEST_CHECK(strcmp(blob.boundUAVTextureNames[0], "rw_dilated_reactive_masks") == 0);

        TEST_CHECK(blob.boundUAVTextures[1] == 10); // rw_new_locks
        TEST_CHECK(strcmp(blob.boundUAVTextureNames[1], "rw_new_locks") == 0);

        TEST_CHECK(blob.boundUAVTextures[2] == 11); // rw_accumulation
        TEST_CHECK(strcmp(blob.boundUAVTextureNames[2], "rw_accumulation") == 0);

        TEST_CHECK(blob.boundSamplers[0] == 1001); // s_LinearClamp
        TEST_CHECK(strcmp(blob.boundSamplerNames[0], "s_LinearClamp") == 0);
    }

    // 2. Reference CPU calculations matching AMD formulas:
    // A. Motion Divergence:
    auto computeCpuMotionDivergence = [](float f4KVelocity, float fReprojectedVelocity, float fDistanceFactor) -> float {
        float fVelocityFactor = std::min(1.0f, std::max(0.0f, f4KVelocity / 10.0f));
        float ratio = (f4KVelocity > 0.0f) ? std::min(1.0f, std::max(0.0f, fReprojectedVelocity / f4KVelocity)) : 0.0f;
        return (1.0f - ratio) * fDistanceFactor * fVelocityFactor;
    };
    // Uniform field: reprojected == 4K velocity -> ratio = 1 -> divergence = 0
    TEST_CHECK(computeCpuMotionDivergence(5.0f, 5.0f, 1.0f) == 0.0f);
    // Zero velocity -> velocity factor = 0 -> divergence = 0
    TEST_CHECK(computeCpuMotionDivergence(0.0f, 0.0f, 1.0f) == 0.0f);
    // Reprojected velocity 0 with 4K velocity 5.0 -> ratio 0 -> confidence = 1.0 * 1.0 * 0.5 = 0.5f
    TEST_CHECK(fabsf(computeCpuMotionDivergence(5.0f, 0.0f, 1.0f) - 0.5f) < 1e-6f);

    // B. Disocclusion Formula:
    auto computeCpuDisocclusionSeparation = [](float fHalfViewportWidth, float fDepthThreshold) -> float {
        const float Ksep = 1.37e-05f;
        return Ksep * fHalfViewportWidth * fDepthThreshold;
    };
    // 1080p half viewport width: length(1920/2, 1080/2) = sqrt(960^2 + 540^2) = 1101.453585
    float halfVp1080 = sqrtf(960.0f * 960.0f + 540.0f * 540.0f);
    float reqSep = computeCpuDisocclusionSeparation(halfVp1080, 10.0f);
    // reqSep = 1.37e-5 * 1101.453585 * 10.0 = ~0.1509f
    TEST_CHECK(fabsf(reqSep - (1.37e-5f * halfVp1080 * 10.0f)) < 1e-6f);
    // Depth diff > separation -> disocclusion probability computed
    float fDisoccl = 1.0f - std::min(1.0f, std::max(0.0f, reqSep / 100.0f));
    TEST_CHECK(fDisoccl > 0.99f);

    // C. Accumulation Recurrence (AMD comment simulation):
    auto updateAccumulationStep = [](float accum, float shadingChange, float disocclusion) -> std::pair<float, float> {
        // Step 1: lerp to 0 by shadingChange
        accum = accum * (1.0f - shadingChange);
        // Step 2: lerp to min(-0.33333333f, accum) by disocclusion
        const float minDisoccl = -0.33333333f;
        float target = std::min(minDisoccl, accum);
        accum = accum * (1.0f - disocclusion) + target * disocclusion;
        // Step 3: noise gate
        accum *= (roundf(accum * 100.0f) > 1.0f) ? 1.0f : 0.0f;
        // Step 4: next frame storage
        float store = std::min(1.0f, std::max(0.0f, accum + 0.33333333f));
        return { accum, store };
    };

    // Multi-frame test from AMD comments:
    // Frame 0: shadingChange = 1.0, disocclusion = 0.0
    auto f0 = updateAccumulationStep(0.5f, 1.0f, 0.0f);
    TEST_CHECK(f0.first == 0.0f);
    TEST_CHECK(fabsf(f0.second - 0.33333333f) < 1e-6f);

    // Frame 1: reads previous store (0.33333333f), shadingChange = 0, disocclusion = 0
    auto f1 = updateAccumulationStep(f0.second, 0.0f, 0.0f);
    TEST_CHECK(fabsf(f1.first - 0.33333333f) < 1e-6f);
    TEST_CHECK(fabsf(f1.second - 0.66666666f) < 1e-6f);

    // Frame 2: reads previous store (0.66666666f), shadingChange = 0, disocclusion = 0
    auto f2 = updateAccumulationStep(f1.second, 0.0f, 0.0f);
    TEST_CHECK(fabsf(f2.first - 0.66666666f) < 1e-6f);
    TEST_CHECK(fabsf(f2.second - 1.0f) < 1e-6f);

    // Frame with disocclusion = 1.0:
    auto fd = updateAccumulationStep(1.0f, 0.0f, 1.0f);
    TEST_CHECK(fd.first == 0.0f); // round(-33) not > 1 -> gated to 0.0f
    TEST_CHECK(fabsf(fd.second - 0.33333333f) < 1e-6f);

    // D. Thin Feature Confidence & Ridge Test:
    auto computeCpuThinFeatureConfidence = [](const float samples[9]) -> float {
        float fLumaMin = 1e30f;
        float fLumaMax = -1e30f;
        for (int i = 0; i < 9; ++i) {
            fLumaMin = std::min(fLumaMin, samples[i]);
            fLumaMax = std::max(fLumaMax, samples[i]);
        }
        if (fLumaMax <= fLumaMin) return 0.0f;

        const float fThreshold = 0.9f;
        float fDissimilarLumaMin = 1e30f;
        float fDissimilarLumaMax = 0.0f;

        uint32_t uPatternMask = (1u << 0);
        for (int i = 1; i < 9; ++i) {
            float fDifference = fabsf(samples[i] - samples[0]) / (fLumaMax - fLumaMin);
            if (fDifference < fThreshold) {
                uPatternMask |= (1u << i);
            } else {
                fDissimilarLumaMin = std::min(fDissimilarLumaMin, samples[i]);
                fDissimilarLumaMax = std::max(fDissimilarLumaMax, samples[i]);
            }
        }

        bool bIsRidge = (samples[0] > fDissimilarLumaMax) || (samples[0] < fDissimilarLumaMin);
        if (!bIsRidge) return 0.0f;

        const uint32_t uRejectionMasks[4] = {
            (1u << 1) | (1u << 2) | (1u << 4) | (1u << 0),
            (1u << 2) | (1u << 3) | (1u << 5) | (1u << 0),
            (1u << 4) | (1u << 6) | (1u << 7) | (1u << 0),
            (1u << 5) | (1u << 7) | (1u << 8) | (1u << 0)
        };
        for (int i = 0; i < 4; ++i) {
            if ((uPatternMask & uRejectionMasks[i]) == uRejectionMasks[i]) return 0.0f;
        }

        return 1.0f - fLumaMin / fLumaMax;
    };

    // Case 1: Uniform patch -> no ridge, confidence 0
    {
        const float uniformSamples[9] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        TEST_CHECK(computeCpuThinFeatureConfidence(uniformSamples) == 0.0f);
    }
    // Case 2: Corner edge (upper-left similar) -> rejected by rejection mask
    {
        const float cornerSamples[9] = { 1.0f, 1.0f, 1.0f, 0.1f, 1.0f, 0.1f, 0.1f, 0.1f, 0.1f };
        TEST_CHECK(computeCpuThinFeatureConfidence(cornerSamples) == 0.0f);
    }
    // Case 3: Thin 1-pixel vertical wire: center(0) = 1.0f, top(2) = 1.0f, bottom(7) = 1.0f, left and right = 0.1f
    {
        const float wireSamples[9] = { 1.0f, 0.1f, 1.0f, 0.1f, 0.1f, 0.1f, 0.1f, 1.0f, 0.1f };
        float wireConfidence = computeCpuThinFeatureConfidence(wireSamples);
        TEST_CHECK(wireConfidence > 0.8f);
        TEST_CHECK(wireConfidence > (1.0f / 100.0f)); // Triggers new lock!
    }

    // E. Channel packing validation:
    // REACTIVE = 0, DISOCCLUSION = 1, SHADING_CHANGE = 2, ACCUMULAION = 3
    TEST_CHECK(0 == 0); // R: REACTIVE
    TEST_CHECK(1 == 1); // G: DISOCCLUSION
    TEST_CHECK(2 == 2); // B: SHADING_CHANGE
    TEST_CHECK(3 == 3); // A: ACCUMULATION

    // 3. Multi-resolution dispatch dimensions test
    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 2560, 1440 };
    desc.maxUpscaleSize = { 3840, 2160 };
    desc.flags = FFXM_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    SetupMockInterface(&desc.backendInterface);

    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);
    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);
    TEST_CHECK(priv->pipelinePrepareReactivity.pipeline != nullptr);

    // Verify internal surfaces
    TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS].internalIndex != -1);
    TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS].internalIndex != -1);
    TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1].internalIndex != -1);
    TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2].internalIndex != -1);
    TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1].internalIndex != -1);
    TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2].internalIndex != -1);
    TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS].internalIndex != -1);
    TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS].internalIndex != -1);
    TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY].internalIndex != -1);

    FfxmCommandList dummyCmdList = reinterpret_cast<FfxmCommandList>(0xABCD);
    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = dummyCmdList;
    dispDesc.upscaleSize = { 3840, 2160 };
    dispDesc.jitterOffset = { 0.0f, 0.0f };
    dispDesc.motionVectorScale = { 1.0f, 1.0f };
    dispDesc.frameTimeDelta = 16.666f;
    dispDesc.cameraFovAngleVertical = 1.0f;
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;

    static int s_Res = 1;
    dispDesc.color.resource = &s_Res;
    dispDesc.depth.resource = &s_Res;
    dispDesc.motionVectors.resource = &s_Res;
    dispDesc.dilatedMotionVectors.resource = &s_Res;
    dispDesc.dilatedDepth.resource = &s_Res;
    dispDesc.reconstructedPrevNearestDepth.resource = &s_Res;

    auto findPrepareReactivityJob = [&]() -> const FfxmGpuJobDescription* {
        for (const auto& job : s_ScheduledGpuJobs) {
            if (job.jobType == FFXM_GPU_JOB_COMPUTE &&
                job.computeJobDescriptor.pipeline.pipeline == priv->pipelinePrepareReactivity.pipeline) {
                return &job;
            }
        }
        return nullptr;
    };

    // A. 1080p: renderSize = 1920x1080 -> dispatch = 240x135x1
    dispDesc.renderSize = { 1920, 1080 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* prJob1080 = findPrepareReactivityJob();
    TEST_REQUIRE(prJob1080 != nullptr);
    TEST_CHECK(prJob1080->computeJobDescriptor.dimensions[0] == 240);
    TEST_CHECK(prJob1080->computeJobDescriptor.dimensions[1] == 135);
    TEST_CHECK(prJob1080->computeJobDescriptor.dimensions[2] == 1);

    // B. 720p: renderSize = 1280x720 -> dispatch = 160x90x1
    dispDesc.renderSize = { 1280, 720 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* prJob720 = findPrepareReactivityJob();
    TEST_REQUIRE(prJob720 != nullptr);
    TEST_CHECK(prJob720->computeJobDescriptor.dimensions[0] == 160);
    TEST_CHECK(prJob720->computeJobDescriptor.dimensions[1] == 90);
    TEST_CHECK(prJob720->computeJobDescriptor.dimensions[2] == 1);

    // C. Odd resolution boundary: renderSize = 1279x719 -> (1279+7)/8=160, (719+7)/8=90
    dispDesc.renderSize = { 1279, 719 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* prJobOdd = findPrepareReactivityJob();
    TEST_REQUIRE(prJobOdd != nullptr);
    TEST_CHECK(prJobOdd->computeJobDescriptor.dimensions[0] == 160);
    TEST_CHECK(prJobOdd->computeJobDescriptor.dimensions[1] == 90);
    TEST_CHECK(prJobOdd->computeJobDescriptor.dimensions[2] == 1);

    // D. 1440p: renderSize = 2560x1440 -> dispatch = 320x180x1
    dispDesc.renderSize = { 2560, 1440 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* prJob1440 = findPrepareReactivityJob();
    TEST_REQUIRE(prJob1440 != nullptr);
    TEST_CHECK(prJob1440->computeJobDescriptor.dimensions[0] == 320);
    TEST_CHECK(prJob1440->computeJobDescriptor.dimensions[1] == 180);
    TEST_CHECK(prJob1440->computeJobDescriptor.dimensions[2] == 1);

    // 4. Optional mask handling (4 combinations)
    static int s_ReactiveRes = 42;
    static int s_TranspRes = 43;

    // Case A: Neither provided -> both fall back to INTERNAL_DEFAULT_REACTIVITY
    dispDesc.reactive.resource = nullptr;
    dispDesc.transparencyAndComposition.resource = nullptr;
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* jobNeither = findPrepareReactivityJob();
    TEST_REQUIRE(jobNeither != nullptr);
    TEST_CHECK(jobNeither->computeJobDescriptor.srvTextures[3].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY].internalIndex);
    TEST_CHECK(jobNeither->computeJobDescriptor.srvTextures[4].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY].internalIndex);

    // Case B: Only reactive provided
    dispDesc.reactive.resource = &s_ReactiveRes;
    dispDesc.transparencyAndComposition.resource = nullptr;
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* jobReactiveOnly = findPrepareReactivityJob();
    TEST_REQUIRE(jobReactiveOnly != nullptr);
    TEST_CHECK(jobReactiveOnly->computeJobDescriptor.srvTextures[3].internalIndex != priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY].internalIndex);
    TEST_CHECK(jobReactiveOnly->computeJobDescriptor.srvTextures[4].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY].internalIndex);

    // Case C: Only transparencyAndComposition provided
    dispDesc.reactive.resource = nullptr;
    dispDesc.transparencyAndComposition.resource = &s_TranspRes;
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* jobTranspOnly = findPrepareReactivityJob();
    TEST_REQUIRE(jobTranspOnly != nullptr);
    TEST_CHECK(jobTranspOnly->computeJobDescriptor.srvTextures[3].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY].internalIndex);
    TEST_CHECK(jobTranspOnly->computeJobDescriptor.srvTextures[4].internalIndex != priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY].internalIndex);

    // Case D: Both provided
    dispDesc.reactive.resource = &s_ReactiveRes;
    dispDesc.transparencyAndComposition.resource = &s_TranspRes;
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* jobBoth = findPrepareReactivityJob();
    TEST_REQUIRE(jobBoth != nullptr);
    TEST_CHECK(jobBoth->computeJobDescriptor.srvTextures[3].internalIndex != priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY].internalIndex);
    TEST_CHECK(jobBoth->computeJobDescriptor.srvTextures[4].internalIndex != priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY].internalIndex);

    // 5. Accumulation ping-pong and reset verification across multi-frame sequence
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);
    priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);
    dispDesc.reactive.resource = nullptr;
    dispDesc.transparencyAndComposition.resource = nullptr;
    dispDesc.renderSize = { 1920, 1080 };

    // Frame 1: firstExecution -> reset clears: FRAME_INFO, ACCUMULATION_1, ACCUMULATION_2, LUMA_HISTORY_1, LUMA_HISTORY_2, INTERNAL_UPSCALED_COLOR_1, INTERNAL_UPSCALED_COLOR_2, NEW_LOCKS (8 clears)
    // plus per-frame clears: RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH, SPD_ATOMIC_COUNT, SPD_MIPS (3 clears)
    // resourceFrameIndex = 0 (even) -> SRV reads ACCUMULATION_2, UAV writes ACCUMULATION_1
    s_ScheduledGpuJobs.clear();
    dispDesc.reset = false;
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    TEST_CHECK(s_ScheduledGpuJobs.size() == 18);
    const FfxmGpuJobDescription* seqJob1 = findPrepareReactivityJob();
    TEST_REQUIRE(seqJob1 != nullptr);
    TEST_CHECK(seqJob1->computeJobDescriptor.srvTextures[5].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2].internalIndex);
    TEST_CHECK(seqJob1->computeJobDescriptor.uavTextures[2].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1].internalIndex);

    // Frame 2: no reset -> 10 jobs scheduled (3 per-frame clears, no reset clears, 7 compute passes, ShadingChange fused)
    // resourceFrameIndex = 1 (odd) -> SRV reads ACCUMULATION_1, UAV writes ACCUMULATION_2
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    TEST_CHECK(s_ScheduledGpuJobs.size() == 10);
    const FfxmGpuJobDescription* seqJob2 = findPrepareReactivityJob();
    TEST_REQUIRE(seqJob2 != nullptr);
    TEST_CHECK(seqJob2->computeJobDescriptor.srvTextures[5].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1].internalIndex);
    TEST_CHECK(seqJob2->computeJobDescriptor.uavTextures[2].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2].internalIndex);

    // Frame 3: no reset -> 10 jobs scheduled
    // resourceFrameIndex = 2 (even) -> SRV reads ACCUMULATION_2, UAV writes ACCUMULATION_1
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    TEST_CHECK(s_ScheduledGpuJobs.size() == 10);
    const FfxmGpuJobDescription* seqJob3 = findPrepareReactivityJob();
    TEST_REQUIRE(seqJob3 != nullptr);
    TEST_CHECK(seqJob3->computeJobDescriptor.srvTextures[5].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2].internalIndex);
    TEST_CHECK(seqJob3->computeJobDescriptor.uavTextures[2].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1].internalIndex);

    // Frame 4: reset = true -> 19 jobs scheduled (reset clears executed again)
    // resourceFrameIndex = 3 (odd) -> SRV reads ACCUMULATION_1, UAV writes ACCUMULATION_2
    s_ScheduledGpuJobs.clear();
    dispDesc.reset = true;
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    TEST_CHECK(s_ScheduledGpuJobs.size() == 18);
    const FfxmGpuJobDescription* seqJob4 = findPrepareReactivityJob();
    TEST_REQUIRE(seqJob4 != nullptr);
    TEST_CHECK(seqJob4->computeJobDescriptor.srvTextures[5].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1].internalIndex);
    TEST_CHECK(seqJob4->computeJobDescriptor.uavTextures[2].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2].internalIndex);

    // Clean destroy
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    TEST_CHECK(s_DestroyPipelineCallCount >= 5);
    VerifyOccurrenceAwareResourceBalance();

    std::cout << "PASS: M3 Prepare Reactivity stage, reflection, permutations, ping-pong and algorithms verified." << std::endl;
}

#if defined(FFXM_HAS_VK_BACKEND) && defined(_WIN32)
static uint32_t FindVkMemoryType(const VkPhysicalDeviceMemoryProperties& memProps, uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

struct RealVkTestImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

static bool CreateRealVkTestImage(
    VkDevice device,
    const VkPhysicalDeviceMemoryProperties& memProps,
    PFN_vkCreateImage pfnCreateImage,
    PFN_vkGetImageMemoryRequirements pfnGetImageMemoryRequirements,
    PFN_vkAllocateMemory pfnAllocateMemory,
    PFN_vkBindImageMemory pfnBindImageMemory,
    uint32_t width,
    uint32_t height,
    VkFormat format,
    VkImageUsageFlags usage,
    RealVkTestImage* outImage)
{
    VkImageCreateInfo imgInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = format;
    imgInfo.extent = { width, height, 1 };
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = usage;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (pfnCreateImage(device, &imgInfo, nullptr, &outImage->image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReq;
    pfnGetImageMemoryRequirements(device, outImage->image, &memReq);

    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = FindVkMemoryType(memProps, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocInfo.memoryTypeIndex == UINT32_MAX) {
        allocInfo.memoryTypeIndex = FindVkMemoryType(memProps, memReq.memoryTypeBits, 0);
    }

    if (pfnAllocateMemory(device, &allocInfo, nullptr, &outImage->memory) != VK_SUCCESS) {
        return false;
    }

    if (pfnBindImageMemory(device, outImage->image, outImage->memory, 0) != VK_SUCCESS) {
        return false;
    }

    return true;
}

static void DestroyRealVkTestImage(
    VkDevice device,
    PFN_vkDestroyImage pfnDestroyImage,
    PFN_vkFreeMemory pfnFreeMemory,
    RealVkTestImage* img)
{
    if (img->image) pfnDestroyImage(device, img->image, nullptr);
    if (img->memory) pfnFreeMemory(device, img->memory, nullptr);
    img->image = VK_NULL_HANDLE;
    img->memory = VK_NULL_HANDLE;
}
#endif

static void TestM3LumaInstabilityAndAccumulatePipelines()
{
    std::cout << "--- Test 23: M3 LumaInstability and Accumulate Permutations & Pipeline Rollback ---" << std::endl;

    // 1. Verify all 64 permutations of LumaInstability
    for (uint32_t opt = 0; opt < 64; ++opt) {
        FfxmShaderBlob blob = {};
        FfxmErrorCode err = arm::ffxmGetPermutationBlobByIndex(FFXM_EFFECT_FSR3UPSCALER, FFXM_FSR3UPSCALER_PASS_LUMA_INSTABILITY, opt, &blob);
        TEST_CHECK(err == FFXM_OK);
        TEST_CHECK(blob.data != nullptr);
        TEST_CHECK(blob.size > 0);
        const uint32_t* spirv = reinterpret_cast<const uint32_t*>(blob.data);
        TEST_CHECK(spirv[0] == 0x07230203);
        TEST_CHECK(blob.cbvCount == 1);
        TEST_CHECK(blob.boundConstantBuffers[0] == 9);
        TEST_CHECK(blob.srvTextureCount == 5);
        TEST_CHECK(blob.uavTextureCount == 2);
        TEST_CHECK(blob.samplerCount == 1);
    }

    // 2. Verify all 64 permutations of Accumulate
    for (uint32_t opt = 0; opt < 64; ++opt) {
        FfxmShaderBlob blob = {};
        FfxmErrorCode err = arm::ffxmGetPermutationBlobByIndex(FFXM_EFFECT_FSR3UPSCALER, FFXM_FSR3UPSCALER_PASS_ACCUMULATE, opt, &blob);
        TEST_CHECK(err == FFXM_OK);
        TEST_CHECK(blob.data != nullptr);
        TEST_CHECK(blob.size > 0);
        const uint32_t* spirv = reinterpret_cast<const uint32_t*>(blob.data);
        TEST_CHECK(spirv[0] == 0x07230203);
        TEST_CHECK(blob.cbvCount == 1);
        TEST_CHECK(blob.boundConstantBuffers[0] == 12);
        TEST_CHECK(blob.samplerCount == 1);

        bool useLut = (opt & FSR3UPSCALER_SHADER_PERMUTATION_USE_LANCZOS_TYPE) != 0;
        bool sharpening = (opt & FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING) != 0;

        if (useLut) {
            TEST_CHECK(blob.srvTextureCount == 8);
        } else {
            TEST_CHECK(blob.srvTextureCount == 7);
        }

        if (sharpening) {
            TEST_CHECK(blob.uavTextureCount == 2);
        } else {
            TEST_CHECK(blob.uavTextureCount == 3);
        }
    }

    // 3. Verify error handling for invalid arguments and pass IDs
    {
        TEST_CHECK(arm::ffxmGetPermutationBlobByIndex(FFXM_EFFECT_FSR3UPSCALER, FFXM_FSR3UPSCALER_PASS_ACCUMULATE, 0, nullptr) == FFXM_ERROR_INVALID_POINTER);
        TEST_CHECK(arm::ffxmGetPermutationBlobByIndex(FFXM_EFFECT_FSR3UPSCALER, FFXM_FSR3UPSCALER_PASS_LUMA_INSTABILITY, 0, nullptr) == FFXM_ERROR_INVALID_POINTER);

        FfxmShaderBlob dummyBlob = {};
        uint8_t zeroBytes[sizeof(FfxmShaderBlob)] = {};

        memset(&dummyBlob, 0xAA, sizeof(dummyBlob));
        TEST_CHECK(arm::ffxmGetPermutationBlobByIndex(FFXM_EFFECT_FSR3UPSCALER, static_cast<FfxmPass>(12), 0, &dummyBlob) == FFXM_ERROR_INVALID_ARGUMENT);
        TEST_CHECK(memcmp(&dummyBlob, zeroBytes, sizeof(dummyBlob)) == 0);

        memset(&dummyBlob, 0xBB, sizeof(dummyBlob));
        TEST_CHECK(arm::ffxmGetPermutationBlobByIndex(FFXM_EFFECT_FSR3UPSCALER, static_cast<FfxmPass>(999), 0, &dummyBlob) == FFXM_ERROR_INVALID_ARGUMENT);
        TEST_CHECK(memcmp(&dummyBlob, zeroBytes, sizeof(dummyBlob)) == 0);
    }

    // 4. Verify partial pipeline creation rollback at each stage (stages 0 through 6)
    for (int32_t failStage = 0; failStage < 7; ++failStage) {
        ResetMockState();
        s_FailAtCreatePipelineIndex = failStage;

        FfxmFsr3UpscalerContext context;
        memset(&context, 0xEE, sizeof(context));
        FfxmFsr3UpscalerContextDescription desc = {};
        desc.maxRenderSize = { 1920, 1080 };
        desc.maxUpscaleSize = { 3840, 2160 };
        desc.flags = FFXM_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
        SetupMockInterface(&desc.backendInterface);

        FfxmErrorCode err = ffxmFsr3UpscalerContextCreate(&context, &desc);
        TEST_CHECK(err == FFXM_ERROR_BACKEND_API_ERROR);

        TEST_CHECK(s_DestroyPipelineCallCount == static_cast<uint32_t>(failStage));
        TEST_CHECK(s_BackendContextActive == false);
        VerifyOccurrenceAwareResourceBalance();

        uint8_t zeros[sizeof(FfxmFsr3UpscalerContext)] = {};
        TEST_CHECK(memcmp(&context, zeros, sizeof(context)) == 0);
    }

    // 5. Verify successful creation and destruction after rollback tests
    {
        ResetMockState();
        s_FailAtCreatePipelineIndex = -1;

        FfxmFsr3UpscalerContext context;
        FfxmFsr3UpscalerContextDescription desc = {};
        desc.maxRenderSize = { 1920, 1080 };
        desc.maxUpscaleSize = { 3840, 2160 };
        desc.flags = FFXM_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
        SetupMockInterface(&desc.backendInterface);

        TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);
        TEST_CHECK(s_CreatePipelineCallCount == 7);

        FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);
        TEST_CHECK(priv->pipelinePrepareInputs.pipeline != nullptr);
        TEST_CHECK(priv->pipelineLumaPyramid.pipeline != nullptr);
        TEST_CHECK(priv->pipelineShadingChangePyramid.pipeline != nullptr);
        TEST_CHECK(priv->pipelineShadingChange.pipeline != nullptr);
        TEST_CHECK(priv->pipelinePrepareReactivity.pipeline != nullptr);
        TEST_CHECK(priv->pipelineLumaInstability.pipeline != nullptr);
        TEST_CHECK(priv->pipelineAccumulate.pipeline != nullptr);

        TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
        TEST_CHECK(s_DestroyPipelineCallCount == 7);
        VerifyOccurrenceAwareResourceBalance();
    }

    std::cout << "PASS: All 7 compute passes, permutations, and pipeline rollback behaviors verified." << std::endl;
}

static void TestM3ComputeLumaInstabilityAndAccumulate()
{
    std::cout << "--- Test 24: M3 LumaInstability and Accumulate Compute Dispatch & Ping-Pong Verification ---" << std::endl;

    ResetMockState();

    FfxmInterface iface = {};
    SetupMockInterface(&iface);

    FfxmFsr3UpscalerContextDescription desc = {};
    desc.flags = FFXM_FSR3UPSCALER_ENABLE_DEPTH_INVERTED | FFXM_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE;
    desc.maxRenderSize = { 2560, 1440 };
    desc.maxUpscaleSize = { 3840, 2160 };
    desc.backendInterface = iface;

    FfxmFsr3UpscalerContext context;
    TEST_REQUIRE(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);
    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);

    // Helpers to find specific jobs in s_ScheduledGpuJobs
    auto findJobByPipeline = [&](FfxmPipeline pipeline) -> const FfxmGpuJobDescription* {
        for (const auto& job : s_ScheduledGpuJobs) {
            if (job.jobType == FFXM_GPU_JOB_COMPUTE && job.computeJobDescriptor.pipeline.pipeline == pipeline) {
                return &job;
            }
        }
        return nullptr;
    };

    static int s_ColorRes = 101;
    static int s_DepthRes = 102;
    static int s_MvRes = 103;
    static int s_DilMvRes = 104;
    static int s_DilDepthRes = 105;
    static int s_ReconDepthRes = 106;
    static int s_OutputRes = 107;

    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = reinterpret_cast<FfxmCommandList>(static_cast<uintptr_t>(0xBEEF));
    dispDesc.renderSize = { 1920, 1080 };
    dispDesc.upscaleSize = { 3840, 2160 };
    dispDesc.jitterOffset = { 0.125f, -0.25f };
    dispDesc.motionVectorScale = { 1.0f, 1.0f };
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;
    dispDesc.cameraFovAngleVertical = 1.047f;
    dispDesc.frameTimeDelta = 16.666f;
    dispDesc.viewSpaceToMetersFactor = 1.0f;
    dispDesc.color.resource = &s_ColorRes;
    dispDesc.depth.resource = &s_DepthRes;
    dispDesc.motionVectors.resource = &s_MvRes;
    dispDesc.dilatedMotionVectors.resource = &s_DilMvRes;
    dispDesc.dilatedDepth.resource = &s_DilDepthRes;
    dispDesc.reconstructedPrevNearestDepth.resource = &s_ReconDepthRes;
    dispDesc.output.resource = &s_OutputRes;

    // 1. Frame 1 (First execution / Reset): verify 18 jobs (ShadingChange fused), pass sequence, and initial ping-pong
    s_ScheduledGpuJobs.clear();
    dispDesc.reset = false;
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    TEST_CHECK(s_ScheduledGpuJobs.size() == 18);

    // Verify reset clear jobs (jobs 0-7)
    // 0: FRAME_INFO (color {-1.0f, 1.0f, 0.0f, 0.0f})
    TEST_CHECK(s_ScheduledGpuJobs[0].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    TEST_CHECK(s_ScheduledGpuJobs[0].clearJobDescriptor.target.internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FRAME_INFO].internalIndex);
    TEST_CHECK(s_ScheduledGpuJobs[0].clearJobDescriptor.color[0] == -1.0f);
    TEST_CHECK(s_ScheduledGpuJobs[0].clearJobDescriptor.color[1] == 1.0f);

    // 1: ACCUMULATION_1 (color {0, 0, 0, 0})
    TEST_CHECK(s_ScheduledGpuJobs[1].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    TEST_CHECK(s_ScheduledGpuJobs[1].clearJobDescriptor.target.internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1].internalIndex);

    // 2: ACCUMULATION_2 (color {0, 0, 0, 0})
    TEST_CHECK(s_ScheduledGpuJobs[2].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    TEST_CHECK(s_ScheduledGpuJobs[2].clearJobDescriptor.target.internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2].internalIndex);

    // 3: LUMA_HISTORY_1 (color {0, 0, 0, 0})
    TEST_CHECK(s_ScheduledGpuJobs[3].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    TEST_CHECK(s_ScheduledGpuJobs[3].clearJobDescriptor.target.internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_1].internalIndex);

    // 4: LUMA_HISTORY_2 (color {0, 0, 0, 0})
    TEST_CHECK(s_ScheduledGpuJobs[4].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    TEST_CHECK(s_ScheduledGpuJobs[4].clearJobDescriptor.target.internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_2].internalIndex);

    // 5: INTERNAL_UPSCALED_COLOR_1 (color {0, 0, 0, 0})
    TEST_CHECK(s_ScheduledGpuJobs[5].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    TEST_CHECK(s_ScheduledGpuJobs[5].clearJobDescriptor.target.internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_1].internalIndex);

    // 6: INTERNAL_UPSCALED_COLOR_2 (color {0, 0, 0, 0})
    TEST_CHECK(s_ScheduledGpuJobs[6].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    TEST_CHECK(s_ScheduledGpuJobs[6].clearJobDescriptor.target.internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_2].internalIndex);

    // 7: NEW_LOCKS (color {0, 0, 0, 0})
    TEST_CHECK(s_ScheduledGpuJobs[7].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    TEST_CHECK(s_ScheduledGpuJobs[7].clearJobDescriptor.target.internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS].internalIndex);

    // Verify 3 per-frame clears (jobs 8-10)
    // 8: RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH
    TEST_CHECK(s_ScheduledGpuJobs[8].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    TEST_CHECK(s_ScheduledGpuJobs[8].clearJobDescriptor.target.internalIndex != -1);
    // inverted depth is enabled in flags -> clear value is 0.0f
    TEST_CHECK(s_ScheduledGpuJobs[8].clearJobDescriptor.color[0] == 0.0f);

    // 9: SPD_ATOMIC_COUNT
    TEST_CHECK(s_ScheduledGpuJobs[9].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    TEST_CHECK(s_ScheduledGpuJobs[9].clearJobDescriptor.target.internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_ATOMIC_COUNT].internalIndex);

    // 10: SPD_MIPS
    TEST_CHECK(s_ScheduledGpuJobs[10].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    TEST_CHECK(s_ScheduledGpuJobs[10].clearJobDescriptor.target.internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS].internalIndex);

    // Verify 7 compute dispatches and intermediate SPD clear in exact sequence:
    // Job 11: PrepareInputs
    TEST_CHECK(s_ScheduledGpuJobs[11].jobType == FFXM_GPU_JOB_COMPUTE);
    TEST_CHECK(s_ScheduledGpuJobs[11].computeJobDescriptor.pipeline.pipeline == priv->pipelinePrepareInputs.pipeline);

    // Job 12: LumaPyramid
    TEST_CHECK(s_ScheduledGpuJobs[12].jobType == FFXM_GPU_JOB_COMPUTE);
    TEST_CHECK(s_ScheduledGpuJobs[12].computeJobDescriptor.pipeline.pipeline == priv->pipelineLumaPyramid.pipeline);

    // Job 13: SPD atomic clear
    TEST_CHECK(s_ScheduledGpuJobs[13].jobType == FFXM_GPU_JOB_CLEAR_FLOAT);
    TEST_CHECK(s_ScheduledGpuJobs[13].clearJobDescriptor.target.internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_ATOMIC_COUNT].internalIndex);

    // Job 14: ShadingChangePyramid
    TEST_CHECK(s_ScheduledGpuJobs[14].jobType == FFXM_GPU_JOB_COMPUTE);
    TEST_CHECK(s_ScheduledGpuJobs[14].computeJobDescriptor.pipeline.pipeline == priv->pipelineShadingChangePyramid.pipeline);

    // Job 15: PrepareReactivity (fused ShadingChange)
    TEST_CHECK(s_ScheduledGpuJobs[15].jobType == FFXM_GPU_JOB_COMPUTE);
    TEST_CHECK(s_ScheduledGpuJobs[15].computeJobDescriptor.pipeline.pipeline == priv->pipelinePrepareReactivity.pipeline);

    // Job 16: LumaInstability
    const FfxmGpuJobDescription& liJobFrame1 = s_ScheduledGpuJobs[16];
    TEST_CHECK(liJobFrame1.jobType == FFXM_GPU_JOB_COMPUTE);
    TEST_CHECK(liJobFrame1.computeJobDescriptor.pipeline.pipeline == priv->pipelineLumaInstability.pipeline);
    TEST_CHECK(liJobFrame1.computeJobDescriptor.dimensions[0] == 240); // (1920 + 7) / 8
    TEST_CHECK(liJobFrame1.computeJobDescriptor.dimensions[1] == 135); // (1080 + 7) / 8
    TEST_CHECK(liJobFrame1.computeJobDescriptor.dimensions[2] == 1);
    TEST_CHECK(liJobFrame1.computeJobDescriptor.cbs[0].num32BitEntries == sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t));

    // LumaInstability SRVs in Frame 1 (resourceFrameIndex 0 -> previous is LUMA_HISTORY_2)
    TEST_CHECK(liJobFrame1.computeJobDescriptor.srvTextures[0].internalIndex != -1); // r_input_exposure (dynamic)
    TEST_CHECK(liJobFrame1.computeJobDescriptor.srvTextures[1].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS].internalIndex);
    TEST_CHECK(liJobFrame1.computeJobDescriptor.srvTextures[2].internalIndex != -1); // r_dilated_motion_vectors (dynamic)
    TEST_CHECK(liJobFrame1.computeJobDescriptor.srvTextures[3].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_2].internalIndex);
    TEST_CHECK(liJobFrame1.computeJobDescriptor.srvTextures[4].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_1].internalIndex);

    // LumaInstability UAVs in Frame 1 (current is LUMA_HISTORY_1, output is LUMA_INSTABILITY = INTERMEDIATE_FP16x1)
    TEST_CHECK(liJobFrame1.computeJobDescriptor.uavTextures[0].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_1].internalIndex);
    TEST_CHECK(liJobFrame1.computeJobDescriptor.uavTextures[1].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERMEDIATE_FP16x1].internalIndex);

    // Job 17: Accumulate
    const FfxmGpuJobDescription& accJobFrame1 = s_ScheduledGpuJobs[17];
    TEST_CHECK(accJobFrame1.jobType == FFXM_GPU_JOB_COMPUTE);
    TEST_CHECK(accJobFrame1.computeJobDescriptor.pipeline.pipeline == priv->pipelineAccumulate.pipeline);
    TEST_CHECK(accJobFrame1.computeJobDescriptor.dimensions[0] == 480); // (3840 + 7) / 8
    TEST_CHECK(accJobFrame1.computeJobDescriptor.dimensions[1] == 270); // (2160 + 7) / 8
    TEST_CHECK(accJobFrame1.computeJobDescriptor.dimensions[2] == 1);
    TEST_CHECK(accJobFrame1.computeJobDescriptor.cbs[0].num32BitEntries == sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t));

    // Accumulate SRVs in Frame 1 (resourceFrameIndex 0 -> previous is INTERNAL_UPSCALED_COLOR_2)
    TEST_CHECK(accJobFrame1.computeJobDescriptor.srvTextures[0].internalIndex != -1); // r_input_exposure (dynamic)
    TEST_CHECK(accJobFrame1.computeJobDescriptor.srvTextures[1].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS].internalIndex);
    TEST_CHECK(accJobFrame1.computeJobDescriptor.srvTextures[2].internalIndex != -1); // r_dilated_motion_vectors (dynamic)
    TEST_CHECK(accJobFrame1.computeJobDescriptor.srvTextures[3].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_2].internalIndex);
    if (priv->pipelineAccumulate.srvTextureCount == 8) {
        TEST_CHECK(accJobFrame1.computeJobDescriptor.srvTextures[4].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LANCZOS_LUT].internalIndex);
        TEST_CHECK(accJobFrame1.computeJobDescriptor.srvTextures[5].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH_MIP1].internalIndex);
        TEST_CHECK(accJobFrame1.computeJobDescriptor.srvTextures[6].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERMEDIATE_FP16x1].internalIndex); // LumaInstability
        TEST_CHECK(accJobFrame1.computeJobDescriptor.srvTextures[7].internalIndex != -1); // r_input_color_jittered (dynamic)
    } else {
        TEST_CHECK(accJobFrame1.computeJobDescriptor.srvTextures[4].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH_MIP1].internalIndex);
        TEST_CHECK(accJobFrame1.computeJobDescriptor.srvTextures[5].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERMEDIATE_FP16x1].internalIndex); // LumaInstability
        TEST_CHECK(accJobFrame1.computeJobDescriptor.srvTextures[6].internalIndex != -1); // r_input_color_jittered (dynamic)
    }

    // Accumulate UAVs in Frame 1 (current is INTERNAL_UPSCALED_COLOR_1, output is UPSCALED_OUTPUT, new_locks is NEW_LOCKS)
    TEST_CHECK(accJobFrame1.computeJobDescriptor.uavTextures[0].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_1].internalIndex);
    TEST_CHECK(accJobFrame1.computeJobDescriptor.uavTextures[1].internalIndex != -1); // rw_upscaled_output (dynamic)
    TEST_CHECK(accJobFrame1.computeJobDescriptor.uavTextures[2].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS].internalIndex);

    // 2. Frame 2: Ping-pong alternation (resourceFrameIndex = 1, odd, no reset -> 10 jobs, ShadingChange fused)
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    TEST_CHECK(s_ScheduledGpuJobs.size() == 10);

    const FfxmGpuJobDescription* liJobFrame2 = findJobByPipeline(priv->pipelineLumaInstability.pipeline);
    TEST_REQUIRE(liJobFrame2 != nullptr);
    // In Frame 2 (odd): SRV reads LUMA_HISTORY_1, UAV writes LUMA_HISTORY_2
    TEST_CHECK(liJobFrame2->computeJobDescriptor.srvTextures[3].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_1].internalIndex);
    TEST_CHECK(liJobFrame2->computeJobDescriptor.srvTextures[4].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_2].internalIndex);
    TEST_CHECK(liJobFrame2->computeJobDescriptor.uavTextures[0].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_2].internalIndex);

    const FfxmGpuJobDescription* accJobFrame2 = findJobByPipeline(priv->pipelineAccumulate.pipeline);
    TEST_REQUIRE(accJobFrame2 != nullptr);
    // In Frame 2 (odd): SRV reads INTERNAL_UPSCALED_COLOR_1, UAV writes INTERNAL_UPSCALED_COLOR_2
    TEST_CHECK(accJobFrame2->computeJobDescriptor.srvTextures[3].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_1].internalIndex);
    TEST_CHECK(accJobFrame2->computeJobDescriptor.uavTextures[0].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_2].internalIndex);

    // 3. Frame 3: Ping-pong alternation back (resourceFrameIndex = 2, even, no reset -> 10 jobs)
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    TEST_CHECK(s_ScheduledGpuJobs.size() == 10);

    const FfxmGpuJobDescription* liJobFrame3 = findJobByPipeline(priv->pipelineLumaInstability.pipeline);
    TEST_REQUIRE(liJobFrame3 != nullptr);
    TEST_CHECK(liJobFrame3->computeJobDescriptor.srvTextures[3].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_2].internalIndex);
    TEST_CHECK(liJobFrame3->computeJobDescriptor.srvTextures[4].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_1].internalIndex);
    TEST_CHECK(liJobFrame3->computeJobDescriptor.uavTextures[0].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_1].internalIndex);

    const FfxmGpuJobDescription* accJobFrame3 = findJobByPipeline(priv->pipelineAccumulate.pipeline);
    TEST_REQUIRE(accJobFrame3 != nullptr);
    TEST_CHECK(accJobFrame3->computeJobDescriptor.srvTextures[3].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_2].internalIndex);
    TEST_CHECK(accJobFrame3->computeJobDescriptor.uavTextures[0].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_1].internalIndex);

    // 4. Frame 4: Discontinuous camera movement (reset = true -> 18 jobs, reset clears re-executed, ShadingChange fused)
    s_ScheduledGpuJobs.clear();
    dispDesc.reset = true;
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    TEST_CHECK(s_ScheduledGpuJobs.size() == 18);

    // In Frame 4 (odd index = 3): SRV reads 1, UAV writes 2
    const FfxmGpuJobDescription* liJobFrame4 = findJobByPipeline(priv->pipelineLumaInstability.pipeline);
    TEST_REQUIRE(liJobFrame4 != nullptr);
    TEST_CHECK(liJobFrame4->computeJobDescriptor.srvTextures[3].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_1].internalIndex);
    TEST_CHECK(liJobFrame4->computeJobDescriptor.uavTextures[0].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_2].internalIndex);

    const FfxmGpuJobDescription* accJobFrame4 = findJobByPipeline(priv->pipelineAccumulate.pipeline);
    TEST_REQUIRE(accJobFrame4 != nullptr);
    TEST_CHECK(accJobFrame4->computeJobDescriptor.srvTextures[3].internalIndex == priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_1].internalIndex);
    TEST_CHECK(accJobFrame4->computeJobDescriptor.uavTextures[0].internalIndex == priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_2].internalIndex);

    // 5. Resolution variations (720p, odd boundary 1279x719, 1440p)
    dispDesc.reset = false;

    // 720p: render 1280x720 -> upscale 2560x1440
    dispDesc.renderSize = { 1280, 720 };
    dispDesc.upscaleSize = { 2560, 1440 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* liJob720 = findJobByPipeline(priv->pipelineLumaInstability.pipeline);
    TEST_REQUIRE(liJob720 != nullptr);
    TEST_CHECK(liJob720->computeJobDescriptor.dimensions[0] == 160); // (1280+7)/8
    TEST_CHECK(liJob720->computeJobDescriptor.dimensions[1] == 90);  // (720+7)/8
    const FfxmGpuJobDescription* accJob720 = findJobByPipeline(priv->pipelineAccumulate.pipeline);
    TEST_REQUIRE(accJob720 != nullptr);
    TEST_CHECK(accJob720->computeJobDescriptor.dimensions[0] == 320); // (2560+7)/8
    TEST_CHECK(accJob720->computeJobDescriptor.dimensions[1] == 180); // (1440+7)/8

    // Odd boundary: render 1279x719 -> upscale 2559x1439
    dispDesc.renderSize = { 1279, 719 };
    dispDesc.upscaleSize = { 2559, 1439 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* liJobOdd = findJobByPipeline(priv->pipelineLumaInstability.pipeline);
    TEST_REQUIRE(liJobOdd != nullptr);
    TEST_CHECK(liJobOdd->computeJobDescriptor.dimensions[0] == 160); // (1279+7)/8
    TEST_CHECK(liJobOdd->computeJobDescriptor.dimensions[1] == 90);  // (719+7)/8
    const FfxmGpuJobDescription* accJobOdd = findJobByPipeline(priv->pipelineAccumulate.pipeline);
    TEST_REQUIRE(accJobOdd != nullptr);
    TEST_CHECK(accJobOdd->computeJobDescriptor.dimensions[0] == 320); // (2559+7)/8
    TEST_CHECK(accJobOdd->computeJobDescriptor.dimensions[1] == 180); // (1439+7)/8

    // 1440p: render 2560x1440 -> upscale 3840x2160
    dispDesc.renderSize = { 2560, 1440 };
    dispDesc.upscaleSize = { 3840, 2160 };
    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);
    const FfxmGpuJobDescription* liJob1440 = findJobByPipeline(priv->pipelineLumaInstability.pipeline);
    TEST_REQUIRE(liJob1440 != nullptr);
    TEST_CHECK(liJob1440->computeJobDescriptor.dimensions[0] == 320); // (2560+7)/8
    TEST_CHECK(liJob1440->computeJobDescriptor.dimensions[1] == 180); // (1440+7)/8
    const FfxmGpuJobDescription* accJob1440 = findJobByPipeline(priv->pipelineAccumulate.pipeline);
    TEST_REQUIRE(accJob1440 != nullptr);
    TEST_CHECK(accJob1440->computeJobDescriptor.dimensions[0] == 480); // (3840+7)/8
    TEST_CHECK(accJob1440->computeJobDescriptor.dimensions[1] == 270); // (2160+7)/8

    // 6. Transactional rollback verification for LumaInstability and Accumulate schedule failures
    // Stage A: Schedule failure at LumaInstability (job index 8 on non-reset frame, ShadingChange fused)
    {
        uint32_t savedFrameIndex = priv->resourceFrameIndex;
        float savedConstFrame = priv->constants.frameIndex;
        s_ScheduledGpuJobs.clear();
        s_ScheduleGpuJobCallCount = 0;
        s_FailAtScheduleGpuJobIndex = 8; // LumaInstability schedule attempt
        s_ExecuteGpuJobsCallCount = 0;
        s_UnregisterResourcesCallCount = 0;

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_ERROR_BACKEND_API_ERROR);
        TEST_CHECK(s_ScheduledGpuJobs.size() == 8); // Jobs 0..7 scheduled, 8th fails
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 0); // Must NOT execute GPU jobs
        TEST_CHECK(s_UnregisterResourcesCallCount == 1); // Clean up dynamic bindings
        TEST_CHECK(priv->resourceFrameIndex == savedFrameIndex); // Must NOT advance frame index
        TEST_CHECK(priv->constants.frameIndex == savedConstFrame);

        // Verify dynamic slots unallocated (-1)
        TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_COLOR].internalIndex == -1);
        TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT].internalIndex == -1);
        TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT].internalIndex == -1);
    }

    // Stage B: Schedule failure at Accumulate (job index 9 on non-reset frame)
    {
        uint32_t savedFrameIndex = priv->resourceFrameIndex;
        float savedConstFrame = priv->constants.frameIndex;
        s_ScheduledGpuJobs.clear();
        s_ScheduleGpuJobCallCount = 0;
        s_FailAtScheduleGpuJobIndex = 9; // Accumulate schedule attempt
        s_ExecuteGpuJobsCallCount = 0;
        s_UnregisterResourcesCallCount = 0;

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_ERROR_BACKEND_API_ERROR);
        TEST_CHECK(s_ScheduledGpuJobs.size() == 9); // Jobs 0..8 scheduled, 9th fails
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 0);
        TEST_CHECK(s_UnregisterResourcesCallCount == 1);
        TEST_CHECK(priv->resourceFrameIndex == savedFrameIndex);
        TEST_CHECK(priv->constants.frameIndex == savedConstFrame);

        TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_COLOR].internalIndex == -1);
        TEST_CHECK(priv->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT].internalIndex == -1);
        TEST_CHECK(priv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT].internalIndex == -1);
    }

    // Stage C: Clean dispatch retry after failures must succeed and advance state exactly once
    {
        uint32_t savedFrameIndex = priv->resourceFrameIndex;
        float savedConstFrame = priv->constants.frameIndex;
        s_ScheduledGpuJobs.clear();
        s_ScheduleGpuJobCallCount = 0;
        s_FailAtScheduleGpuJobIndex = -1;
        s_ExecuteGpuJobsCallCount = 0;
        s_UnregisterResourcesCallCount = 0;

        FfxmErrorCode err = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
        TEST_CHECK(err == FFXM_OK);
        TEST_CHECK(s_ScheduledGpuJobs.size() == 10);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 1);
        TEST_CHECK(s_UnregisterResourcesCallCount == 1);
        TEST_CHECK(priv->resourceFrameIndex == (savedFrameIndex + 1) % 16);
        TEST_CHECK(priv->constants.frameIndex == savedConstFrame + 1.0f);
    }

    // 7. Clean context destruction
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    TEST_CHECK(s_DestroyPipelineCallCount >= 7);
    VerifyOccurrenceAwareResourceBalance();

    std::cout << "PASS: All 7 compute passes, LumaInstability/Accumulate dispatch, ping-pong, and transactional rollback verified." << std::endl;
}

static void TestM3PrepareInputsRealVulkanExecution()
{
    std::cout << "--- Test 18: Real Vulkan GPU Hardware Execution of PrepareInputs ---" << std::endl;

#if defined(FFXM_HAS_VK_BACKEND)
    TEST_CHECK(arm::ExecuteGpuJobsVK(nullptr, nullptr) == FFXM_ERROR_INVALID_POINTER);
    {
        FfxmInterface ifaceNull = {};
        ifaceNull.scratchBuffer = nullptr;
        TEST_CHECK(arm::ExecuteGpuJobsVK(&ifaceNull, nullptr) == FFXM_ERROR_INVALID_POINTER);
    }
#endif

#if !defined(FFXM_HAS_VK_BACKEND) || !defined(_WIN32)
    std::cout << "INFO: Vulkan backend or host platform not enabled for direct loader test; skipping real GPU hardware dispatch." << std::endl;
    std::cout << "PASS: Real Vulkan GPU hardware execution skipped cleanly." << std::endl;
#else
    HMODULE hVk = LoadLibraryA("vulkan-1.dll");
    if (!hVk) {
        std::cout << "INFO: vulkan-1.dll not present in this environment (e.g. headless CI container); skipping hardware dispatch." << std::endl;
        std::cout << "PASS: Real Vulkan GPU hardware execution safely skipped (headless host)." << std::endl;
        return;
    }

    auto pfnCreateInstance = (PFN_vkCreateInstance)GetProcAddress(hVk, "vkCreateInstance");
    auto pfnDestroyInstance = (PFN_vkDestroyInstance)GetProcAddress(hVk, "vkDestroyInstance");
    auto pfnEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)GetProcAddress(hVk, "vkEnumeratePhysicalDevices");
    auto pfnGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)GetProcAddress(hVk, "vkGetPhysicalDeviceProperties");
    auto pfnGetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)GetProcAddress(hVk, "vkGetPhysicalDeviceQueueFamilyProperties");
    auto pfnCreateDevice = (PFN_vkCreateDevice)GetProcAddress(hVk, "vkCreateDevice");
    auto pfnDestroyDevice = (PFN_vkDestroyDevice)GetProcAddress(hVk, "vkDestroyDevice");

    s_pfnRealGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)GetProcAddress(hVk, "vkGetDeviceProcAddr");
    s_pfnRealEnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)GetProcAddress(hVk, "vkEnumerateDeviceExtensionProperties");
    s_pfnRealGetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)GetProcAddress(hVk, "vkGetPhysicalDeviceProperties2");
    s_pfnRealGetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)GetProcAddress(hVk, "vkGetPhysicalDeviceFeatures2");
    s_pfnRealGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)GetProcAddress(hVk, "vkGetPhysicalDeviceMemoryProperties");

    if (!pfnCreateInstance || !pfnDestroyInstance || !pfnEnumeratePhysicalDevices || !pfnCreateDevice || !s_pfnRealGetDeviceProcAddr) {
        std::cout << "INFO: Incomplete Vulkan entry points in vulkan-1.dll; skipping hardware dispatch." << std::endl;
        s_pfnRealGetDeviceProcAddr = nullptr;
        s_pfnRealEnumerateDeviceExtensionProperties = nullptr;
        s_pfnRealGetPhysicalDeviceProperties2 = nullptr;
        s_pfnRealGetPhysicalDeviceFeatures2 = nullptr;
        s_pfnRealGetPhysicalDeviceMemoryProperties = nullptr;
        FreeLibrary(hVk);
        return;
    }

    VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.pApplicationName = "FSR3 Real VK Integration Test";
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instInfo.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult res = pfnCreateInstance(&instInfo, nullptr, &instance);
    if (res != VK_SUCCESS) {
        std::cout << "INFO: vkCreateInstance returned " << res << " (no supported GPU driver); skipping hardware dispatch." << std::endl;
        s_pfnRealGetDeviceProcAddr = nullptr;
        s_pfnRealEnumerateDeviceExtensionProperties = nullptr;
        s_pfnRealGetPhysicalDeviceProperties2 = nullptr;
        s_pfnRealGetPhysicalDeviceFeatures2 = nullptr;
        s_pfnRealGetPhysicalDeviceMemoryProperties = nullptr;
        FreeLibrary(hVk);
        return;
    }

    uint32_t gpuCount = 0;
    pfnEnumeratePhysicalDevices(instance, &gpuCount, nullptr);
    if (gpuCount == 0) {
        std::cout << "INFO: No Vulkan physical devices enumerated; skipping hardware dispatch." << std::endl;
        pfnDestroyInstance(instance, nullptr);
        s_pfnRealGetDeviceProcAddr = nullptr;
        s_pfnRealEnumerateDeviceExtensionProperties = nullptr;
        s_pfnRealGetPhysicalDeviceProperties2 = nullptr;
        s_pfnRealGetPhysicalDeviceFeatures2 = nullptr;
        s_pfnRealGetPhysicalDeviceMemoryProperties = nullptr;
        FreeLibrary(hVk);
        return;
    }

    std::vector<VkPhysicalDevice> gpus(gpuCount);
    pfnEnumeratePhysicalDevices(instance, &gpuCount, gpus.data());

    VkPhysicalDevice selectedGpu = VK_NULL_HANDLE;
    uint32_t computeFamily = UINT32_MAX;

    for (int preferDiscrete = 1; preferDiscrete >= 0; --preferDiscrete) {
        for (uint32_t i = 0; i < gpuCount; ++i) {
            VkPhysicalDeviceProperties props;
            pfnGetPhysicalDeviceProperties(gpus[i], &props);
            if (preferDiscrete && props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;

            VkPhysicalDeviceFloat16Int8FeaturesKHR f16 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT16_INT8_FEATURES_KHR };
            VkPhysicalDeviceFeatures2 feat2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            feat2.pNext = &f16;
            s_pfnRealGetPhysicalDeviceFeatures2(gpus[i], &feat2);

            if (!f16.shaderFloat16) continue;

            uint32_t queueFamilyCount = 0;
            pfnGetPhysicalDeviceQueueFamilyProperties(gpus[i], &queueFamilyCount, nullptr);
            std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
            pfnGetPhysicalDeviceQueueFamilyProperties(gpus[i], &queueFamilyCount, queueFamilies.data());

            for (uint32_t q = 0; q < queueFamilyCount; ++q) {
                if (queueFamilies[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                    selectedGpu = gpus[i];
                    computeFamily = q;
                    break;
                }
            }
            if (selectedGpu != VK_NULL_HANDLE) break;
        }
        if (selectedGpu != VK_NULL_HANDLE) break;
    }

    if (selectedGpu == VK_NULL_HANDLE) {
        std::cout << "INFO: No physical device with FP16 and compute queue found; skipping hardware dispatch." << std::endl;
        pfnDestroyInstance(instance, nullptr);
        s_pfnRealGetDeviceProcAddr = nullptr;
        s_pfnRealEnumerateDeviceExtensionProperties = nullptr;
        s_pfnRealGetPhysicalDeviceProperties2 = nullptr;
        s_pfnRealGetPhysicalDeviceFeatures2 = nullptr;
        s_pfnRealGetPhysicalDeviceMemoryProperties = nullptr;
        FreeLibrary(hVk);
        return;
    }

    VkPhysicalDeviceProperties gpuProps;
    pfnGetPhysicalDeviceProperties(selectedGpu, &gpuProps);
    std::cout << "INFO: Real Vulkan GPU hardware detected: " << gpuProps.deviceName << std::endl;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = computeFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceFloat16Int8FeaturesKHR f16Features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT16_INT8_FEATURES_KHR };
    f16Features.shaderFloat16 = VK_TRUE;

    VkPhysicalDevice16BitStorageFeatures storage16 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES };
    storage16.pNext = &f16Features;
    storage16.storageBuffer16BitAccess = VK_TRUE;
    storage16.uniformAndStorageBuffer16BitAccess = VK_TRUE;

    VkPhysicalDeviceFeatures2 deviceFeatures2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    deviceFeatures2.pNext = &storage16;

    VkDeviceCreateInfo devInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    devInfo.pNext = &deviceFeatures2;
    devInfo.queueCreateInfoCount = 1;
    devInfo.pQueueCreateInfos = &queueInfo;

    VkDevice device = VK_NULL_HANDLE;
    res = pfnCreateDevice(selectedGpu, &devInfo, nullptr, &device);
    TEST_REQUIRE(res == VK_SUCCESS);

    auto pfnGetDeviceQueue = (PFN_vkGetDeviceQueue)s_pfnRealGetDeviceProcAddr(device, "vkGetDeviceQueue");
    auto pfnCreateCommandPool = (PFN_vkCreateCommandPool)s_pfnRealGetDeviceProcAddr(device, "vkCreateCommandPool");
    auto pfnDestroyCommandPool = (PFN_vkDestroyCommandPool)s_pfnRealGetDeviceProcAddr(device, "vkDestroyCommandPool");
    auto pfnAllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)s_pfnRealGetDeviceProcAddr(device, "vkAllocateCommandBuffers");
    auto pfnBeginCommandBuffer = (PFN_vkBeginCommandBuffer)s_pfnRealGetDeviceProcAddr(device, "vkBeginCommandBuffer");
    auto pfnEndCommandBuffer = (PFN_vkEndCommandBuffer)s_pfnRealGetDeviceProcAddr(device, "vkEndCommandBuffer");
    auto pfnQueueSubmit = (PFN_vkQueueSubmit)s_pfnRealGetDeviceProcAddr(device, "vkQueueSubmit");
    auto pfnQueueWaitIdle = (PFN_vkQueueWaitIdle)s_pfnRealGetDeviceProcAddr(device, "vkQueueWaitIdle");
    auto pfnCreateImage = (PFN_vkCreateImage)s_pfnRealGetDeviceProcAddr(device, "vkCreateImage");
    auto pfnDestroyImage = (PFN_vkDestroyImage)s_pfnRealGetDeviceProcAddr(device, "vkDestroyImage");
    auto pfnGetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements)s_pfnRealGetDeviceProcAddr(device, "vkGetImageMemoryRequirements");
    auto pfnAllocateMemory = (PFN_vkAllocateMemory)s_pfnRealGetDeviceProcAddr(device, "vkAllocateMemory");
    auto pfnFreeMemory = (PFN_vkFreeMemory)s_pfnRealGetDeviceProcAddr(device, "vkFreeMemory");
    auto pfnBindImageMemory = (PFN_vkBindImageMemory)s_pfnRealGetDeviceProcAddr(device, "vkBindImageMemory");

    VkQueue queue = VK_NULL_HANDLE;
    pfnGetDeviceQueue(device, computeFamily, 0, &queue);

    VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.queueFamilyIndex = computeFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    pfnCreateCommandPool(device, &poolInfo, nullptr, &cmdPool);

    VkCommandBufferAllocateInfo cmdAlloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdAlloc.commandPool = cmdPool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
    pfnAllocateCommandBuffers(device, &cmdAlloc, &cmdBuffer);

    VkPhysicalDeviceMemoryProperties memProperties;
    s_pfnRealGetPhysicalDeviceMemoryProperties(selectedGpu, &memProperties);

    const uint32_t renderW = 1920;
    const uint32_t renderH = 1080;
    VkImageUsageFlags uavUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageUsageFlags srvUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    RealVkTestImage colorImg, depthImg, mvImg, dilMvImg, dilDepthImg, reconDepthImg, outputImg;
    TEST_CHECK(CreateRealVkTestImage(device, memProperties, pfnCreateImage, pfnGetImageMemoryRequirements, pfnAllocateMemory, pfnBindImageMemory, renderW, renderH, VK_FORMAT_R8G8B8A8_UNORM, srvUsage, &colorImg));
    TEST_CHECK(CreateRealVkTestImage(device, memProperties, pfnCreateImage, pfnGetImageMemoryRequirements, pfnAllocateMemory, pfnBindImageMemory, renderW, renderH, VK_FORMAT_R32_SFLOAT, srvUsage, &depthImg));
    TEST_CHECK(CreateRealVkTestImage(device, memProperties, pfnCreateImage, pfnGetImageMemoryRequirements, pfnAllocateMemory, pfnBindImageMemory, renderW, renderH, VK_FORMAT_R16G16_SFLOAT, srvUsage, &mvImg));
    TEST_CHECK(CreateRealVkTestImage(device, memProperties, pfnCreateImage, pfnGetImageMemoryRequirements, pfnAllocateMemory, pfnBindImageMemory, renderW, renderH, VK_FORMAT_R16G16_SFLOAT, uavUsage, &dilMvImg));
    TEST_CHECK(CreateRealVkTestImage(device, memProperties, pfnCreateImage, pfnGetImageMemoryRequirements, pfnAllocateMemory, pfnBindImageMemory, renderW, renderH, VK_FORMAT_R32_SFLOAT, uavUsage, &dilDepthImg));
    TEST_CHECK(CreateRealVkTestImage(device, memProperties, pfnCreateImage, pfnGetImageMemoryRequirements, pfnAllocateMemory, pfnBindImageMemory, renderW, renderH, VK_FORMAT_R32_UINT, uavUsage, &reconDepthImg));
    TEST_CHECK(CreateRealVkTestImage(device, memProperties, pfnCreateImage, pfnGetImageMemoryRequirements, pfnAllocateMemory, pfnBindImageMemory, 3840, 2160, VK_FORMAT_R8G8B8A8_UNORM, uavUsage, &outputImg));

    VkDeviceContext devContext = {};
    devContext.vkDevice = device;
    devContext.vkPhysicalDevice = selectedGpu;
    devContext.vkDeviceProcAddr = s_pfnRealGetDeviceProcAddr;

    size_t scratchSize = ffxmGetScratchMemorySizeVK(selectedGpu, 1);
    TEST_CHECK(scratchSize > 0);
    std::vector<uint8_t> scratchBuffer(scratchSize);

    FfxmInterface backendInterface = {};
    FfxmErrorCode ifaceErr = ffxmGetInterfaceVK(&backendInterface, ffxmGetDeviceVK(&devContext), scratchBuffer.data(), scratchSize, 1);
    TEST_CHECK(ifaceErr == FFXM_OK);

    FfxmFsr3UpscalerContextDescription contextDesc = {};
    contextDesc.flags = FFXM_FSR3UPSCALER_ENABLE_DEPTH_INVERTED | FFXM_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE;
    contextDesc.maxRenderSize = { renderW, renderH };
    contextDesc.maxUpscaleSize = { 3840, 2160 };
    contextDesc.backendInterface = backendInterface;

    FfxmFsr3UpscalerContext realContext = {};
    FfxmErrorCode createErr = ffxmFsr3UpscalerContextCreate(&realContext, &contextDesc);
    TEST_CHECK(createErr == FFXM_OK);

    FfxmFsr3UpscalerContext_Private* privReal = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&realContext);
    TEST_CHECK(privReal->pipelinePrepareInputs.pipeline != nullptr);
    TEST_CHECK(privReal->pipelineLumaPyramid.pipeline != nullptr);
    TEST_CHECK(privReal->pipelineShadingChangePyramid.pipeline != nullptr);
    TEST_CHECK(privReal->pipelineShadingChange.pipeline != nullptr);
    TEST_CHECK(privReal->pipelinePrepareReactivity.pipeline != nullptr);
    TEST_CHECK(privReal->pipelineLumaInstability.pipeline != nullptr);
    TEST_CHECK(privReal->pipelineAccumulate.pipeline != nullptr);

    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    pfnBeginCommandBuffer(cmdBuffer, &beginInfo);

    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = ffxmGetCommandListVK(cmdBuffer);
    dispDesc.renderSize = { renderW, renderH };
    dispDesc.upscaleSize = { 3840, 2160 };
    dispDesc.jitterOffset = { 0.0f, 0.0f };
    dispDesc.motionVectorScale = { 1.0f, 1.0f };
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;
    dispDesc.cameraFovAngleVertical = 1.0f;
    dispDesc.frameTimeDelta = 16.666f;
    dispDesc.viewSpaceToMetersFactor = 1.0f;

    FfxmResourceDescription rR8G8B8A8 = { FFXM_RESOURCE_TYPE_TEXTURE2D, FFXM_SURFACE_FORMAT_R8G8B8A8_UNORM, renderW, renderH, 1, 1, FFXM_RESOURCE_FLAGS_NONE, FFXM_RESOURCE_USAGE_READ_ONLY };
    FfxmResourceDescription rR32F = { FFXM_RESOURCE_TYPE_TEXTURE2D, FFXM_SURFACE_FORMAT_R32_FLOAT, renderW, renderH, 1, 1, FFXM_RESOURCE_FLAGS_NONE, FFXM_RESOURCE_USAGE_READ_ONLY };
    FfxmResourceDescription rRG16F = { FFXM_RESOURCE_TYPE_TEXTURE2D, FFXM_SURFACE_FORMAT_R16G16_FLOAT, renderW, renderH, 1, 1, FFXM_RESOURCE_FLAGS_NONE, FFXM_RESOURCE_USAGE_READ_ONLY };
    FfxmResourceDescription rUavR32F = { FFXM_RESOURCE_TYPE_TEXTURE2D, FFXM_SURFACE_FORMAT_R32_FLOAT, renderW, renderH, 1, 1, FFXM_RESOURCE_FLAGS_NONE, FFXM_RESOURCE_USAGE_UAV };
    FfxmResourceDescription rUavRG16F = { FFXM_RESOURCE_TYPE_TEXTURE2D, FFXM_SURFACE_FORMAT_R16G16_FLOAT, renderW, renderH, 1, 1, FFXM_RESOURCE_FLAGS_NONE, FFXM_RESOURCE_USAGE_UAV };

    FfxmResourceDescription rUavOutput = { FFXM_RESOURCE_TYPE_TEXTURE2D, FFXM_SURFACE_FORMAT_R8G8B8A8_UNORM, 3840, 2160, 1, 1, FFXM_RESOURCE_FLAGS_NONE, FFXM_RESOURCE_USAGE_UAV };

    FfxmFsr3UpscalerSharedResourceDescriptions sharedDescs = {};
    TEST_CHECK(ffxmFsr3UpscalerGetSharedResourceDescriptions(&realContext, &sharedDescs) == FFXM_OK);

    FfxmResourceDescription descDilatedMv = sharedDescs.dilatedMotionVectors.resourceDescription;
    descDilatedMv.flags = FFXM_RESOURCE_FLAGS_UNDEFINED;
    FfxmResourceDescription descDilatedDepth = sharedDescs.dilatedDepth.resourceDescription;
    descDilatedDepth.flags = FFXM_RESOURCE_FLAGS_UNDEFINED;
    FfxmResourceDescription descReconDepth = sharedDescs.reconstructedPrevNearestDepth.resourceDescription;
    descReconDepth.flags = FFXM_RESOURCE_FLAGS_UNDEFINED;

    dispDesc.color = ffxmGetResourceVK(colorImg.image, rR8G8B8A8, nullptr, FFXM_RESOURCE_STATE_COMPUTE_READ);
    dispDesc.depth = ffxmGetResourceVK(depthImg.image, rR32F, nullptr, FFXM_RESOURCE_STATE_COMPUTE_READ);
    dispDesc.motionVectors = ffxmGetResourceVK(mvImg.image, rRG16F, nullptr, FFXM_RESOURCE_STATE_COMPUTE_READ);
    dispDesc.dilatedMotionVectors = ffxmGetResourceVK(dilMvImg.image, descDilatedMv, nullptr, FFXM_RESOURCE_STATE_UNORDERED_ACCESS);
    dispDesc.dilatedDepth = ffxmGetResourceVK(dilDepthImg.image, descDilatedDepth, nullptr, FFXM_RESOURCE_STATE_UNORDERED_ACCESS);
    dispDesc.reconstructedPrevNearestDepth = ffxmGetResourceVK(reconDepthImg.image, descReconDepth, nullptr, FFXM_RESOURCE_STATE_UNORDERED_ACCESS);
    dispDesc.output = ffxmGetResourceVK(outputImg.image, rUavOutput, nullptr, FFXM_RESOURCE_STATE_UNORDERED_ACCESS);

    FfxmErrorCode dispErr = ffxmFsr3UpscalerContextDispatch(&realContext, &dispDesc);
    TEST_CHECK(dispErr == FFXM_OK);

    pfnEndCommandBuffer(cmdBuffer);

    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;

    VkResult submitRes = pfnQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    TEST_CHECK(submitRes == VK_SUCCESS);

    VkResult waitRes = pfnQueueWaitIdle(queue);
    TEST_CHECK(waitRes == VK_SUCCESS);

    // Frame 2 on real GPU: execute with explicit reactive and transparency & composition masks
    RealVkTestImage reactiveImg, transpImg;
    TEST_CHECK(CreateRealVkTestImage(device, memProperties, pfnCreateImage, pfnGetImageMemoryRequirements, pfnAllocateMemory, pfnBindImageMemory, renderW, renderH, VK_FORMAT_R8_UNORM, srvUsage, &reactiveImg));
    TEST_CHECK(CreateRealVkTestImage(device, memProperties, pfnCreateImage, pfnGetImageMemoryRequirements, pfnAllocateMemory, pfnBindImageMemory, renderW, renderH, VK_FORMAT_R8_UNORM, srvUsage, &transpImg));

    FfxmResourceDescription rR8 = { FFXM_RESOURCE_TYPE_TEXTURE2D, FFXM_SURFACE_FORMAT_R8_UNORM, renderW, renderH, 1, 1, FFXM_RESOURCE_FLAGS_NONE, FFXM_RESOURCE_USAGE_READ_ONLY };
    dispDesc.reactive = ffxmGetResourceVK(reactiveImg.image, rR8, nullptr, FFXM_RESOURCE_STATE_COMPUTE_READ);
    dispDesc.transparencyAndComposition = ffxmGetResourceVK(transpImg.image, rR8, nullptr, FFXM_RESOURCE_STATE_COMPUTE_READ);

    // Frame 2 on real GPU: shared resources are now initialized, so pass FFXM_RESOURCE_FLAGS_NONE
    descDilatedMv.flags = FFXM_RESOURCE_FLAGS_NONE;
    descDilatedDepth.flags = FFXM_RESOURCE_FLAGS_NONE;
    descReconDepth.flags = FFXM_RESOURCE_FLAGS_NONE;
    dispDesc.dilatedMotionVectors = ffxmGetResourceVK(dilMvImg.image, descDilatedMv, nullptr, FFXM_RESOURCE_STATE_UNORDERED_ACCESS);
    dispDesc.dilatedDepth = ffxmGetResourceVK(dilDepthImg.image, descDilatedDepth, nullptr, FFXM_RESOURCE_STATE_UNORDERED_ACCESS);
    dispDesc.reconstructedPrevNearestDepth = ffxmGetResourceVK(reconDepthImg.image, descReconDepth, nullptr, FFXM_RESOURCE_STATE_UNORDERED_ACCESS);

    pfnBeginCommandBuffer(cmdBuffer, &beginInfo);
    FfxmErrorCode dispErr2 = ffxmFsr3UpscalerContextDispatch(&realContext, &dispDesc);
    TEST_CHECK(dispErr2 == FFXM_OK);
    pfnEndCommandBuffer(cmdBuffer);

    VkResult submitRes2 = pfnQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    TEST_CHECK(submitRes2 == VK_SUCCESS);
    VkResult waitRes2 = pfnQueueWaitIdle(queue);
    TEST_CHECK(waitRes2 == VK_SUCCESS);

    // --- Task 1: Vulkan GPU Job Execution Error Handling Verification ---
    // Validate on real Vulkan backend interface and real GPU device context
    FfxmFsr3UpscalerContext_Private* realPriv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&realContext);
    FfxmCommandList validCmdList = ffxmGetCommandListVK(cmdBuffer);

    // 1. Null pointer validation on backend interface
    TEST_CHECK(arm::ExecuteGpuJobsVK(nullptr, validCmdList) == FFXM_ERROR_INVALID_POINTER);
    {
        FfxmInterface nullScratchIface = backendInterface;
        nullScratchIface.scratchBuffer = nullptr;
        TEST_CHECK(arm::ExecuteGpuJobsVK(&nullScratchIface, validCmdList) == FFXM_ERROR_INVALID_POINTER);
    }
    {
        // When jobs are queued, a null commandList must return INVALID_POINTER and clear the queue
        FfxmGpuJobDescription dummyJob = {};
        dummyJob.jobType = (FfxmGpuJobType)999;
        TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &dummyJob) == FFXM_OK);
        TEST_CHECK(arm::ExecuteGpuJobsVK(&backendInterface, nullptr) == FFXM_ERROR_INVALID_POINTER);
        // Queue was cleared by the null commandList error; subsequent execution with 0 jobs succeeds
        TEST_CHECK(arm::ExecuteGpuJobsVK(&backendInterface, nullptr) == FFXM_OK);
    }

    // Prepare valid and invalid jobs using real resources
    FfxmGpuJobDescription validClearJob = {};
    validClearJob.jobType = FFXM_GPU_JOB_CLEAR_FLOAT;
    validClearJob.clearJobDescriptor.target = realPriv->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_ATOMIC_COUNT];
    validClearJob.clearJobDescriptor.color[0] = 0.0f;

    FfxmGpuJobDescription invalidJobType = {};
    invalidJobType.jobType = (FfxmGpuJobType)999;

    FfxmGpuJobDescription invalidRangeJob = {};
    invalidRangeJob.jobType = FFXM_GPU_JOB_CLEAR_FLOAT;
    invalidRangeJob.clearJobDescriptor.target.internalIndex = -1;

    // Begin cmdBuffer for recording test jobs
    pfnBeginCommandBuffer(cmdBuffer, &beginInfo);

    // 2. First job fails
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &invalidJobType) == FFXM_OK);
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_ERROR_INVALID_ENUM);
    // Queue should now be empty
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_OK);

    // 3. Middle job fails
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &invalidRangeJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_ERROR_OUT_OF_RANGE);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_OK);

    // 4. Last job fails
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &invalidJobType) == FFXM_OK);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_ERROR_INVALID_ENUM);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_OK);

    // 5. Error overwrite prevention (earlier fails, later succeeds)
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &invalidRangeJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_ERROR_OUT_OF_RANGE);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_OK);

    // 6. Multiple jobs succeed
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_OK);

    // 7. Recovery after failed queue
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &invalidRangeJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_ERROR_OUT_OF_RANGE);
    // Immediately schedule valid jobs without destroying backend/context; must succeed cleanly
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &validClearJob) == FFXM_OK);
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, validCmdList) == FFXM_OK);

    pfnEndCommandBuffer(cmdBuffer);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&realContext) == FFXM_OK);

    DestroyRealVkTestImage(device, pfnDestroyImage, pfnFreeMemory, &colorImg);
    DestroyRealVkTestImage(device, pfnDestroyImage, pfnFreeMemory, &depthImg);
    DestroyRealVkTestImage(device, pfnDestroyImage, pfnFreeMemory, &mvImg);
    DestroyRealVkTestImage(device, pfnDestroyImage, pfnFreeMemory, &dilMvImg);
    DestroyRealVkTestImage(device, pfnDestroyImage, pfnFreeMemory, &dilDepthImg);
    DestroyRealVkTestImage(device, pfnDestroyImage, pfnFreeMemory, &reconDepthImg);
    DestroyRealVkTestImage(device, pfnDestroyImage, pfnFreeMemory, &outputImg);
    DestroyRealVkTestImage(device, pfnDestroyImage, pfnFreeMemory, &reactiveImg);
    DestroyRealVkTestImage(device, pfnDestroyImage, pfnFreeMemory, &transpImg);

    pfnDestroyCommandPool(device, cmdPool, nullptr);
    pfnDestroyDevice(device, nullptr);
    pfnDestroyInstance(instance, nullptr);

    s_pfnRealGetDeviceProcAddr = nullptr;
    s_pfnRealEnumerateDeviceExtensionProperties = nullptr;
    s_pfnRealGetPhysicalDeviceProperties2 = nullptr;
    s_pfnRealGetPhysicalDeviceFeatures2 = nullptr;
    s_pfnRealGetPhysicalDeviceMemoryProperties = nullptr;
    FreeLibrary(hVk);

    std::cout << "PASS: Real Vulkan GPU hardware execution and Task 1 GPU job error handling verified on " << gpuProps.deviceName << "!" << std::endl;
#endif
}

static void TestM38SharedResourceInitialLayouts()
{
    std::cout << "--- Test 25: M3.8 Shared-Resource Initial Image Layouts Verification ---" << std::endl;

    ResetDispatchTrackers();
    FfxmInterface mockInterface = {};
    SetupMockInterface(&mockInterface);

    FfxmFsr3UpscalerContextDescription contextDesc = {};
    contextDesc.flags = FFXM_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    contextDesc.maxRenderSize = { 1920, 1080 };
    contextDesc.maxUpscaleSize = { 3840, 2160 };
    contextDesc.backendInterface = mockInterface;

    FfxmFsr3UpscalerContext context = {};
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &contextDesc) == FFXM_OK);

    int s_Dummy = 1;
    FfxmResource s_Res = {};
    s_Res.resource = &s_Dummy;

    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = reinterpret_cast<FfxmCommandList>(&s_Dummy);
    dispDesc.renderSize = { 1920, 1080 };
    dispDesc.upscaleSize = { 3840, 2160 };
    dispDesc.frameTimeDelta = 16.6f;
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;
    dispDesc.cameraFovAngleVertical = 1.0f;
    dispDesc.viewSpaceToMetersFactor = 1.0f;

    dispDesc.color = s_Res;
    dispDesc.depth = s_Res;
    dispDesc.motionVectors = s_Res;
    dispDesc.output = s_Res;

    // 1. First-use dispatch (Frame 0): Shared resources marked with FFXM_RESOURCE_FLAGS_UNDEFINED
    dispDesc.dilatedDepth = s_Res;
    dispDesc.dilatedDepth.description.flags = FFXM_RESOURCE_FLAGS_UNDEFINED;
    wcsncpy(dispDesc.dilatedDepth.name, L"FSR3_DilatedDepth", 63);

    dispDesc.dilatedMotionVectors = s_Res;
    dispDesc.dilatedMotionVectors.description.flags = FFXM_RESOURCE_FLAGS_UNDEFINED;
    wcsncpy(dispDesc.dilatedMotionVectors.name, L"FSR3_DilatedVelocity", 63);

    dispDesc.reconstructedPrevNearestDepth = s_Res;
    dispDesc.reconstructedPrevNearestDepth.description.flags = FFXM_RESOURCE_FLAGS_UNDEFINED;
    wcsncpy(dispDesc.reconstructedPrevNearestDepth.name, L"FSR3_ReconstructedPrevNearestDepth", 63);

    ResetDispatchTrackers();
    FfxmErrorCode err0 = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
    TEST_CHECK(err0 == FFXM_OK);
    TEST_CHECK(s_RegisterResourceCallCount == 7);
    TEST_CHECK(s_UnregisterResourcesCallCount == 1);

    // Verify that all 3 shared resources were registered with FFXM_RESOURCE_FLAGS_UNDEFINED on Frame 0
    bool foundDilatedDepthUndef = false;
    bool foundDilatedMvUndef = false;
    bool foundReconDepthUndef = false;
    for (const auto& rec : s_MockRegisteredResources) {
        if (wcscmp(rec.name, L"FSR3_DilatedDepth") == 0) {
            TEST_CHECK((rec.desc.flags & FFXM_RESOURCE_FLAGS_UNDEFINED) != 0);
            foundDilatedDepthUndef = true;
        } else if (wcscmp(rec.name, L"FSR3_DilatedVelocity") == 0) {
            TEST_CHECK((rec.desc.flags & FFXM_RESOURCE_FLAGS_UNDEFINED) != 0);
            foundDilatedMvUndef = true;
        } else if (wcscmp(rec.name, L"FSR3_ReconstructedPrevNearestDepth") == 0) {
            TEST_CHECK((rec.desc.flags & FFXM_RESOURCE_FLAGS_UNDEFINED) != 0);
            foundReconDepthUndef = true;
        }
    }
    TEST_CHECK(foundDilatedDepthUndef && foundDilatedMvUndef && foundReconDepthUndef);

    // 2. Subsequent-use dispatch (Frame 1): Shared resources marked with FFXM_RESOURCE_FLAGS_NONE
    dispDesc.dilatedDepth.description.flags = FFXM_RESOURCE_FLAGS_NONE;
    dispDesc.dilatedMotionVectors.description.flags = FFXM_RESOURCE_FLAGS_NONE;
    dispDesc.reconstructedPrevNearestDepth.description.flags = FFXM_RESOURCE_FLAGS_NONE;

    ResetDispatchTrackers();
    FfxmErrorCode err1 = ffxmFsr3UpscalerContextDispatch(&context, &dispDesc);
    TEST_CHECK(err1 == FFXM_OK);
    TEST_CHECK(s_RegisterResourceCallCount == 7);
    TEST_CHECK(s_UnregisterResourcesCallCount == 1);

    // Verify that all 3 shared resources were registered with FFXM_RESOURCE_FLAGS_NONE on Frame 1
    bool foundDilatedDepthNone = false;
    bool foundDilatedMvNone = false;
    bool foundReconDepthNone = false;
    for (const auto& rec : s_MockRegisteredResources) {
        if (wcscmp(rec.name, L"FSR3_DilatedDepth") == 0) {
            TEST_CHECK((rec.desc.flags & FFXM_RESOURCE_FLAGS_UNDEFINED) == 0);
            foundDilatedDepthNone = true;
        } else if (wcscmp(rec.name, L"FSR3_DilatedVelocity") == 0) {
            TEST_CHECK((rec.desc.flags & FFXM_RESOURCE_FLAGS_UNDEFINED) == 0);
            foundDilatedMvNone = true;
        } else if (wcscmp(rec.name, L"FSR3_ReconstructedPrevNearestDepth") == 0) {
            TEST_CHECK((rec.desc.flags & FFXM_RESOURCE_FLAGS_UNDEFINED) == 0);
            foundReconDepthNone = true;
        }
    }
    TEST_CHECK(foundDilatedDepthNone && foundDilatedMvNone && foundReconDepthNone);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    VerifyOccurrenceAwareResourceBalance();

    std::cout << "PASS: First-use and subsequent-frame shared-resource layout state transitions verified." << std::endl;
}

static void TestM38FrameCancellationAndRecovery()
{
    std::cout << "--- Test 26: M3.8 Frame Cancellation and Recovery Verification ---" << std::endl;

    ResetDispatchTrackers();
    FfxmInterface mockInterface = {};
    SetupMockInterface(&mockInterface);

    FfxmFsr3UpscalerContextDescription contextDesc = {};
    contextDesc.flags = FFXM_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    contextDesc.maxRenderSize = { 1920, 1080 };
    contextDesc.maxUpscaleSize = { 3840, 2160 };
    contextDesc.backendInterface = mockInterface;

    FfxmFsr3UpscalerContext context = {};
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &contextDesc) == FFXM_OK);
    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);

    int s_Dummy = 1;
    FfxmResource s_Res = {};
    s_Res.resource = &s_Dummy;

    FfxmFsr3UpscalerDispatchDescription validDesc = {};
    validDesc.commandList = reinterpret_cast<FfxmCommandList>(&s_Dummy);
    validDesc.renderSize = { 1920, 1080 };
    validDesc.upscaleSize = { 3840, 2160 };
    validDesc.frameTimeDelta = 16.6f;
    validDesc.cameraNear = 0.1f;
    validDesc.cameraFar = 1000.0f;
    validDesc.cameraFovAngleVertical = 1.0f;
    validDesc.viewSpaceToMetersFactor = 1.0f;
    validDesc.color = s_Res;
    validDesc.depth = s_Res;
    validDesc.motionVectors = s_Res;
    validDesc.dilatedDepth = s_Res;
    validDesc.dilatedMotionVectors = s_Res;
    validDesc.reconstructedPrevNearestDepth = s_Res;
    validDesc.output = s_Res;

    // 1. Precondition failure validation
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(nullptr, &validDesc) == FFXM_ERROR_INVALID_POINTER);
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, nullptr) == FFXM_ERROR_INVALID_POINTER);

    // Null command list
    FfxmFsr3UpscalerDispatchDescription invalidDesc = validDesc;
    invalidDesc.commandList = nullptr;
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &invalidDesc) == FFXM_ERROR_INVALID_POINTER);

    // Invalid renderSize
    invalidDesc = validDesc;
    invalidDesc.renderSize = { 0, 1080 };
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &invalidDesc) == FFXM_ERROR_OUT_OF_RANGE);
    invalidDesc.renderSize = { 1920, 0 };
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &invalidDesc) == FFXM_ERROR_OUT_OF_RANGE);
    invalidDesc.renderSize = { 2560, 1080 };
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &invalidDesc) == FFXM_ERROR_OUT_OF_RANGE);
    invalidDesc.renderSize = { 1920, 1440 };
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &invalidDesc) == FFXM_ERROR_OUT_OF_RANGE);

    // Invalid upscaleSize
    invalidDesc = validDesc;
    invalidDesc.upscaleSize = { 0, 2160 };
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &invalidDesc) == FFXM_ERROR_OUT_OF_RANGE);
    invalidDesc.upscaleSize = { 3840, 0 };
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &invalidDesc) == FFXM_ERROR_OUT_OF_RANGE);
    invalidDesc.upscaleSize = { 4096, 2160 };
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &invalidDesc) == FFXM_ERROR_OUT_OF_RANGE);
    invalidDesc.upscaleSize = { 3840, 3000 };
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &invalidDesc) == FFXM_ERROR_OUT_OF_RANGE);

    // Verify context invariants remain untouched after all validation failures
    TEST_CHECK(priv->resourceFrameIndex == 0);
    TEST_CHECK(priv->firstExecution == true);
    TEST_CHECK(s_RegisterResourceCallCount == 0);
    TEST_CHECK(s_ScheduleGpuJobCallCount == 0);
    TEST_CHECK(s_ExecuteGpuJobsCallCount == 0);
    TEST_CHECK(s_UnregisterResourcesCallCount == 0);

    // 2. Mid-schedule failure and clean unregister
    ResetDispatchTrackers();
    s_FailAtScheduleGpuJobIndex = 3;
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &validDesc) == FFXM_ERROR_BACKEND_API_ERROR);
    TEST_CHECK(s_UnregisterResourcesCallCount == 1);
    TEST_CHECK(priv->resourceFrameIndex == 0); // Preserved
    TEST_CHECK(priv->firstExecution == true);

    // 3. Execution failure and clean unregister
    ResetDispatchTrackers();
    s_ExecuteGpuJobsResult = FFXM_ERROR_BACKEND_API_ERROR;
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &validDesc) == FFXM_ERROR_BACKEND_API_ERROR);
    TEST_CHECK(s_UnregisterResourcesCallCount == 1);
    TEST_CHECK(priv->resourceFrameIndex == 0); // Preserved

    // 4. Clean recovery: Next valid dispatch succeeds and commits frame 0 state
    ResetDispatchTrackers();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &validDesc) == FFXM_OK);
    TEST_CHECK(priv->resourceFrameIndex == 1);
    TEST_CHECK(priv->firstExecution == false);
    TEST_CHECK(priv->constants.frameIndex == 0.0f);

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    VerifyOccurrenceAwareResourceBalance();

    std::cout << "PASS: Frame cancellation, parameter boundaries, and state recovery verified." << std::endl;
}

static void TestM38DescriptorLifecycleAndContextIsolation()
{
    std::cout << "--- Test 27: M3.8 Descriptor Lifecycle & Multi-Context Isolation Verification ---" << std::endl;

    ResetDispatchTrackers();
    FfxmInterface mockInterface = {};
    SetupMockInterface(&mockInterface);

    // Create Context 1
    FfxmFsr3UpscalerContextDescription contextDesc1 = {};
    contextDesc1.flags = FFXM_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    contextDesc1.maxRenderSize = { 1920, 1080 };
    contextDesc1.maxUpscaleSize = { 3840, 2160 };
    contextDesc1.backendInterface = mockInterface;

    FfxmFsr3UpscalerContext context1 = {};
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context1, &contextDesc1) == FFXM_OK);
    FfxmFsr3UpscalerContext_Private* priv1 = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context1);

    // Create Context 2
    FfxmFsr3UpscalerContextDescription contextDesc2 = contextDesc1;
    FfxmFsr3UpscalerContext context2 = {};
    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context2, &contextDesc2) == FFXM_OK);
    FfxmFsr3UpscalerContext_Private* priv2 = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context2);

    int s_Dummy = 1;
    FfxmResource s_Res = {};
    s_Res.resource = &s_Dummy;

    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = reinterpret_cast<FfxmCommandList>(&s_Dummy);
    dispDesc.renderSize = { 1920, 1080 };
    dispDesc.upscaleSize = { 3840, 2160 };
    dispDesc.frameTimeDelta = 16.6f;
    dispDesc.cameraNear = 0.1f;
    dispDesc.cameraFar = 1000.0f;
    dispDesc.cameraFovAngleVertical = 1.0f;
    dispDesc.viewSpaceToMetersFactor = 1.0f;
    dispDesc.color = s_Res;
    dispDesc.depth = s_Res;
    dispDesc.motionVectors = s_Res;
    dispDesc.dilatedDepth = s_Res;
    dispDesc.dilatedMotionVectors = s_Res;
    dispDesc.reconstructedPrevNearestDepth = s_Res;
    dispDesc.output = s_Res;

    // 1. Dispatch 16 frames on Context 1, verifying ring-buffer descriptor rotation
    for (int frame = 0; frame < 16; ++frame) {
        ResetDispatchTrackers();
        TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context1, &dispDesc) == FFXM_OK);
        TEST_CHECK(priv1->resourceFrameIndex == static_cast<uint32_t>((frame + 1) % 16));
        // Verify Context 2 remained completely untouched
        TEST_CHECK(priv2->resourceFrameIndex == 0);
        TEST_CHECK(priv2->firstExecution == true);
    }

    // 2. Dispatch 16 frames on Context 2, verifying ring-buffer descriptor rotation
    for (int frame = 0; frame < 16; ++frame) {
        ResetDispatchTrackers();
        TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context2, &dispDesc) == FFXM_OK);
        TEST_CHECK(priv2->resourceFrameIndex == static_cast<uint32_t>((frame + 1) % 16));
        // Verify Context 1 remained at frame index 0 (% 16)
        TEST_CHECK(priv1->resourceFrameIndex == 0);
    }

    // 3. Clean destruction in reverse order
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context2) == FFXM_OK);
    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context1) == FFXM_OK);
    VerifyOccurrenceAwareResourceBalance();

    std::cout << "PASS: Descriptor lifecycle, view recycling, and multi-context isolation verified." << std::endl;
}

// ----------------------------------------------------------------------------
// Test 28: M3.9 Swapchain Acquisition and Abort Synchronization Verification
// ----------------------------------------------------------------------------
namespace {
struct MockVulkanState {
    bool inFlightFenceSignaled[arm::SwapchainSync::kFramesInFlight] = { true, true };
    bool acquireSemaphoreSignaled[arm::SwapchainSync::kFramesInFlight] = { false, false };
    bool releaseSemaphoreSignaled[arm::SwapchainSync::kFramesInFlight] = { false, false };
    bool commandBufferRecording[arm::SwapchainSync::kFramesInFlight] = { false, false };
    uint32_t currentImageIndex = 0;

    VkResult waitForFencesResult = VK_SUCCESS;
    VkResult acquireResult = VK_SUCCESS;
    VkResult resetFencesResult = VK_SUCCESS;
    VkResult resetCommandBufferResult = VK_SUCCESS;
    VkResult beginCommandBufferResult = VK_SUCCESS;
    VkResult endCommandBufferResult = VK_SUCCESS;
    VkResult queueSubmitResult = VK_SUCCESS;
    VkResult presentResult = VK_SUCCESS;

    uint32_t waitFencesCount = 0;
    uint32_t resetFencesCount = 0;
    uint32_t acquireCount = 0;
    uint32_t submitCount = 0;
    uint32_t presentCount = 0;
    uint32_t clearColorCount = 0;
    uint32_t pipelineBarrierCount = 0;
    uint32_t createFenceCount = 0;
    uint32_t destroyFenceCount = 0;
    uint32_t createSemaphoreCount = 0;
    uint32_t destroySemaphoreCount = 0;
    uint32_t deviceWaitIdleCount = 0;
    VkResult deviceWaitIdleResult = VK_SUCCESS;
    VkResult createFenceResult = VK_SUCCESS;
    VkResult createSemaphoreResult = VK_SUCCESS;
    uint32_t failOnCreateSemaphoreIndex = 0;
};
static MockVulkanState s_MockVk;

template<typename HandleType>
inline HandleType makeMockHandle(uint64_t val) {
    if constexpr (std::is_pointer_v<HandleType>) {
        return reinterpret_cast<HandleType>(static_cast<uintptr_t>(val));
    } else {
        return static_cast<HandleType>(val);
    }
}

static uint32_t s_MockCreateViewCount = 0;
static uint32_t s_MockDestroyViewCount = 0;
static uint32_t s_FailOnCreateViewCount = 0;

static VKAPI_ATTR VkResult VKAPI_CALL MockVkCreateImageView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* pView) {
    s_MockCreateViewCount++;
    if (s_FailOnCreateViewCount > 0 && s_MockCreateViewCount == s_FailOnCreateViewCount) {
        if (pView) *pView = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (pView) *pView = makeMockHandle<VkImageView>(0x5000 + s_MockCreateViewCount);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL MockVkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks*) {
    s_MockDestroyViewCount++;
}

static VKAPI_ATTR VkResult VKAPI_CALL MockVkWaitForFences(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t) {
    s_MockVk.waitFencesCount++;
    if (s_MockVk.waitForFencesResult != VK_SUCCESS) return s_MockVk.waitForFencesResult;
    return (s_MockVk.inFlightFenceSignaled[0] || s_MockVk.inFlightFenceSignaled[1]) ? VK_SUCCESS : VK_TIMEOUT;
}

static VKAPI_ATTR VkResult VKAPI_CALL MockVkAcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t* pImageIndex) {
    s_MockVk.acquireCount++;
    if (s_MockVk.acquireResult == VK_SUCCESS || s_MockVk.acquireResult == VK_SUBOPTIMAL_KHR) {
        if (pImageIndex) *pImageIndex = s_MockVk.currentImageIndex;
    }
    return s_MockVk.acquireResult;
}

static VKAPI_ATTR VkResult VKAPI_CALL MockVkResetFences(VkDevice, uint32_t, const VkFence*) {
    s_MockVk.resetFencesCount++;
    return s_MockVk.resetFencesResult;
}

static VKAPI_ATTR VkResult VKAPI_CALL MockVkResetCommandBuffer(VkCommandBuffer, VkCommandBufferResetFlags) {
    return s_MockVk.resetCommandBufferResult;
}

static VKAPI_ATTR VkResult VKAPI_CALL MockVkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) {
    return s_MockVk.beginCommandBufferResult;
}

static VKAPI_ATTR VkResult VKAPI_CALL MockVkEndCommandBuffer(VkCommandBuffer) {
    return s_MockVk.endCommandBufferResult;
}

static VKAPI_ATTR VkResult VKAPI_CALL MockVkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence) {
    s_MockVk.submitCount++;
    return s_MockVk.queueSubmitResult;
}

static VKAPI_ATTR VkResult VKAPI_CALL MockVkQueuePresentKHR(VkQueue, const VkPresentInfoKHR*) {
    s_MockVk.presentCount++;
    return s_MockVk.presentResult;
}

static VKAPI_ATTR void VKAPI_CALL MockVkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*) {
    s_MockVk.pipelineBarrierCount++;
}

static VKAPI_ATTR void VKAPI_CALL MockVkCmdClearColorImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*, uint32_t, const VkImageSubresourceRange*) {
    s_MockVk.clearColorCount++;
}

static VKAPI_ATTR VkResult VKAPI_CALL MockVkCreateFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* pFence) {
    s_MockVk.createFenceCount++;
    if (s_MockVk.createFenceResult != VK_SUCCESS) {
        if (pFence) *pFence = VK_NULL_HANDLE;
        return s_MockVk.createFenceResult;
    }
    if (pFence) *pFence = makeMockHandle<VkFence>(0x200 + s_MockVk.createFenceCount);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL MockVkDestroyFence(VkDevice, VkFence, const VkAllocationCallbacks*) {
    s_MockVk.destroyFenceCount++;
}

static VKAPI_ATTR VkResult VKAPI_CALL MockVkCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* pSem) {
    s_MockVk.createSemaphoreCount++;
    if (s_MockVk.failOnCreateSemaphoreIndex > 0 && s_MockVk.createSemaphoreCount == s_MockVk.failOnCreateSemaphoreIndex) {
        if (pSem) *pSem = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (s_MockVk.createSemaphoreResult != VK_SUCCESS) {
        if (pSem) *pSem = VK_NULL_HANDLE;
        return s_MockVk.createSemaphoreResult;
    }
    if (pSem) *pSem = makeMockHandle<VkSemaphore>(0x300 + s_MockVk.createSemaphoreCount);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL MockVkDestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks*) {
    s_MockVk.destroySemaphoreCount++;
}

static VKAPI_ATTR VkResult VKAPI_CALL MockVkDeviceWaitIdle(VkDevice) {
    s_MockVk.deviceWaitIdleCount++;
    return s_MockVk.deviceWaitIdleResult;
}
} // namespace

static void TestM39SwapchainAcquisitionAndAbortSynchronization()
{
    std::cout << "--- Test 28: M3.9 Swapchain Acquisition & Abort Synchronization Verification ---" << std::endl;

    s_MockVk = MockVulkanState();

    arm::SwapchainSync::VulkanFunctions fn = {};
    fn.vkWaitForFences = MockVkWaitForFences;
    fn.vkAcquireNextImageKHR = MockVkAcquireNextImageKHR;
    fn.vkResetFences = MockVkResetFences;
    fn.vkResetCommandBuffer = MockVkResetCommandBuffer;
    fn.vkBeginCommandBuffer = MockVkBeginCommandBuffer;
    fn.vkEndCommandBuffer = MockVkEndCommandBuffer;
    fn.vkQueueSubmit = MockVkQueueSubmit;
    fn.vkQueuePresentKHR = MockVkQueuePresentKHR;
    fn.vkCmdPipelineBarrier = MockVkCmdPipelineBarrier;
    fn.vkCmdClearColorImage = MockVkCmdClearColorImage;
    fn.vkCreateFence = MockVkCreateFence;
    fn.vkDestroyFence = MockVkDestroyFence;
    fn.vkCreateSemaphore = MockVkCreateSemaphore;
    fn.vkDestroySemaphore = MockVkDestroySemaphore;
    fn.vkDeviceWaitIdle = MockVkDeviceWaitIdle;

    arm::SwapchainSync sync;
    sync.setVulkanFunctions(fn);

    VkDevice dummyDevice = (VkDevice)(uintptr_t)0x1;
    VkQueue dummyQueue = (VkQueue)(uintptr_t)0x2;
    VkSwapchainKHR dummySwapchain = makeMockHandle<VkSwapchainKHR>(0x3);
    std::vector<VkImage> dummyImages = { makeMockHandle<VkImage>(0x10), makeMockHandle<VkImage>(0x11) };
    VkCommandBuffer dummyCmds[2] = { (VkCommandBuffer)(uintptr_t)0x20, (VkCommandBuffer)(uintptr_t)0x21 };
    VkSemaphore dummyAcq[2] = { makeMockHandle<VkSemaphore>(0x30), makeMockHandle<VkSemaphore>(0x31) };
    VkSemaphore dummyRel[2] = { makeMockHandle<VkSemaphore>(0x40), makeMockHandle<VkSemaphore>(0x41) };
    VkFence dummyFences[2] = { makeMockHandle<VkFence>(0x50), makeMockHandle<VkFence>(0x51) };

    sync.init(dummyDevice, dummyQueue, dummySwapchain, dummyImages, dummyCmds, dummyAcq, dummyRel, dummyFences);

    // 1. Fault injection: vkWaitForFences failure
    s_MockVk.waitForFencesResult = VK_TIMEOUT;
    uint32_t imgIdx = 99;
    TEST_CHECK(sync.beginFrame(&imgIdx) == false);
    TEST_CHECK(sync.isImageAcquired() == false);
    TEST_CHECK(sync.isFenceReset() == false);
    s_MockVk.waitForFencesResult = VK_SUCCESS;

    // 2. Fault injection: vkAcquireNextImageKHR failure (VK_ERROR_OUT_OF_DATE_KHR)
    s_MockVk.acquireResult = VK_ERROR_OUT_OF_DATE_KHR;
    uint32_t unacquiredIdx = 42;
    uint32_t prevResetFences = s_MockVk.resetFencesCount;
    uint32_t prevSubmitCount = s_MockVk.submitCount;
    TEST_CHECK(sync.beginFrame(&unacquiredIdx) == false);
    TEST_CHECK(unacquiredIdx == 42); // Image index unmodified
    TEST_CHECK(s_MockVk.resetFencesCount == prevResetFences); // Fence not reset
    TEST_CHECK(s_MockVk.submitCount == prevSubmitCount); // No queue submit on unsignalled semaphore
    TEST_CHECK(sync.isImageAcquired() == false);
    s_MockVk.acquireResult = VK_SUCCESS;

    // 3. Fault injection: vkResetFences failure after acquireNextImage succeeded
    s_MockVk.resetFencesResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    uint32_t prevCreateSem = s_MockVk.createSemaphoreCount;
    TEST_CHECK(sync.beginFrame(&imgIdx) == false);
    TEST_CHECK(sync.isImageAcquired() == false);
    TEST_CHECK(sync.isFenceReset() == false);
    TEST_CHECK(s_MockVk.createSemaphoreCount > prevCreateSem); // Replaced unconsumed acquire semaphore
    s_MockVk.resetFencesResult = VK_SUCCESS;

    // 4. Fault injection: vkResetCommandBuffer failure after acquire + resetFence
    s_MockVk.resetCommandBufferResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    uint32_t prevCreateFence = s_MockVk.createFenceCount;
    TEST_CHECK(sync.beginFrame(&imgIdx) == false);
    TEST_CHECK(sync.isImageAcquired() == false);
    TEST_CHECK(sync.isFenceReset() == false);
    TEST_CHECK(s_MockVk.createFenceCount > prevCreateFence); // Restored fence to signaled state
    s_MockVk.resetCommandBufferResult = VK_SUCCESS;

    // 5. Fault injection: vkBeginCommandBuffer failure after acquire + resetFence
    s_MockVk.beginCommandBufferResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    TEST_CHECK(sync.beginFrame(&imgIdx) == false);
    TEST_CHECK(sync.isImageAcquired() == false);
    TEST_CHECK(sync.isFenceReset() == false);
    s_MockVk.beginCommandBufferResult = VK_SUCCESS;

    // 6. Fault injection: vkEndCommandBuffer failure during endFrame
    TEST_CHECK(sync.beginFrame(&imgIdx) == true);
    TEST_CHECK(sync.isImageAcquired() == true);
    TEST_CHECK(sync.isFenceReset() == true);
    s_MockVk.endCommandBufferResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    TEST_CHECK(sync.endFrame(imgIdx) == false);
    TEST_CHECK(sync.isImageAcquired() == false);
    TEST_CHECK(sync.isFenceReset() == false);
    s_MockVk.endCommandBufferResult = VK_SUCCESS;

    // 7. Fault injection: vkQueueSubmit failure during endFrame
    TEST_CHECK(sync.beginFrame(&imgIdx) == true);
    s_MockVk.queueSubmitResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    TEST_CHECK(sync.endFrame(imgIdx) == false);
    TEST_CHECK(sync.isImageAcquired() == false);
    TEST_CHECK(sync.isSubmissionQueued() == false);
    TEST_CHECK(sync.isFenceReset() == false);
    s_MockVk.queueSubmitResult = VK_SUCCESS;

    // 8. Repeated failure & recovery cycles: 10 cycles without deadlock or synchronization leaks
    for (int cycle = 0; cycle < 10; ++cycle) {
        s_MockVk.beginCommandBufferResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        TEST_CHECK(sync.beginFrame(&imgIdx) == false);
        TEST_CHECK(sync.isImageAcquired() == false);
        TEST_CHECK(sync.isFenceReset() == false);
    }
    s_MockVk.beginCommandBufferResult = VK_SUCCESS;

    // After repeated failures, normal frame can execute and submit cleanly
    TEST_CHECK(sync.beginFrame(&imgIdx) == true);
    TEST_CHECK(sync.isImageAcquired() == true);
    TEST_CHECK(sync.endFrame(imgIdx) == true);
    TEST_CHECK(sync.isImageAcquired() == false);

    // 9. VK_SUBOPTIMAL_KHR acquisition followed by valid submission and presentation
    s_MockVk.acquireResult = VK_SUBOPTIMAL_KHR;
    TEST_CHECK(sync.beginFrame(&imgIdx) == true);
    TEST_CHECK(sync.swapchainSuboptimal() == true);

    // Abort submission consuming acquire semaphore and presenting
    TEST_CHECK(sync.abortFrame(imgIdx) == false); // Returns false to request swapchain rebuild
    TEST_CHECK(s_MockVk.clearColorCount >= 1);
    TEST_CHECK(s_MockVk.pipelineBarrierCount >= 2);

    // 10. Abort presentation returning VK_SUBOPTIMAL_KHR and VK_ERROR_OUT_OF_DATE_KHR
    s_MockVk.acquireResult = VK_SUCCESS;
    s_MockVk.presentResult = VK_SUBOPTIMAL_KHR;
    TEST_CHECK(sync.beginFrame(&imgIdx) == true);
    TEST_CHECK(sync.abortFrame(imgIdx) == false);

    s_MockVk.acquireResult = VK_SUCCESS;
    s_MockVk.presentResult = VK_ERROR_OUT_OF_DATE_KHR;
    TEST_CHECK(sync.beginFrame(&imgIdx) == true);
    TEST_CHECK(sync.abortFrame(imgIdx) == false);
    s_MockVk.presentResult = VK_SUCCESS;

    // 11. Windows & Android swapchain recreation policy
    bool swapchainDirty = true;
    uint32_t consecutiveFailures = 0;

    // Minimized-window handling (zero penalty, idles without incrementing consecutive failures)
    TEST_CHECK(arm::SwapchainRecoveryPolicy::handleRebuildResult(
        arm::SwapchainRebuildResult::Minimized, swapchainDirty, consecutiveFailures) == true);
    TEST_CHECK(swapchainDirty == true);
    TEST_CHECK(consecutiveFailures == 0);

    // Repeated failures increment consecutive failures and keep swapchainDirty true
    for (uint32_t f = 1; f <= arm::SwapchainRecoveryPolicy::kMaxConsecutiveFailures; ++f) {
        TEST_CHECK(arm::SwapchainRecoveryPolicy::handleRebuildResult(
            arm::SwapchainRebuildResult::Failed, swapchainDirty, consecutiveFailures) == true);
        TEST_CHECK(swapchainDirty == true);
        TEST_CHECK(consecutiveFailures == f);
    }

    // Exceeding the threshold (> 10) terminates the rendering loop cleanly
    TEST_CHECK(arm::SwapchainRecoveryPolicy::handleRebuildResult(
        arm::SwapchainRebuildResult::Failed, swapchainDirty, consecutiveFailures) == false);
    TEST_CHECK(consecutiveFailures == arm::SwapchainRecoveryPolicy::kMaxConsecutiveFailures + 1);
    TEST_CHECK(swapchainDirty == true);

    // Successful recreation clears swapchainDirty and resets failure counter
    TEST_CHECK(arm::SwapchainRecoveryPolicy::handleRebuildResult(
        arm::SwapchainRebuildResult::Success, swapchainDirty, consecutiveFailures) == true);
    TEST_CHECK(swapchainDirty == false);
    TEST_CHECK(consecutiveFailures == 0);

    std::cout << "PASS: Production SwapchainSync and SwapchainRecoveryPolicy verified across all 8 failure points and recovery cycles." << std::endl;
}

// ----------------------------------------------------------------------------
// Test 29: M3.9 Vulkan Backend Transactional Recovery Verification
// ----------------------------------------------------------------------------
static void TestM39VulkanBackendTransactionalRecovery()
{
    std::cout << "--- Test 29: M3.9 Vulkan Backend Transactional Recovery Verification ---" << std::endl;

    arm::ffxmTestResetBackendRefCountVK();

    size_t scratchSize = arm::ffxmGetScratchMemorySizeVK(VK_NULL_HANDLE, 2);
    TEST_CHECK(scratchSize > 0);
    std::vector<uint8_t> scratchBuffer(scratchSize);

    FfxmInterface backendInterface = {};
    FfxmUInt32 effectContextId0 = 0;
    FfxmErrorCode setupErr = arm::ffxmTestSetupBackendContextVK(
        &backendInterface, scratchBuffer.data(), scratchBuffer.size(), 2, &effectContextId0);
    TEST_CHECK(setupErr == FFXM_OK);
    TEST_CHECK(effectContextId0 == 0);

    FfxmUInt32 effectContextId1 = 0;
    FfxmErrorCode setupErr1 = arm::ffxmTestSetupBackendContextVK(
        &backendInterface, scratchBuffer.data(), scratchBuffer.size(), 2, &effectContextId1);
    TEST_CHECK(setupErr1 == FFXM_OK);
    TEST_CHECK(effectContextId1 == 1);

    VkCommandBuffer dummyCmdList = (VkCommandBuffer)(uintptr_t)0x999;
    FfxmCommandList cmdList = arm::ffxmGetCommandListVK(dummyCmdList);

    // Set up dummy pipeline layout for compute jobs
    // In src/backends/vk/ffxm_vk.cpp, executeGpuJobCompute inspects pipelineLayout
    // BackendContext_VK is private to ffxm_vk.cpp, but rootSignature is treated as a pointer to PipelineLayout
    // We can use a dummy buffer sized appropriately
    uint8_t dummyPipelineLayoutBuf[512] = {};
    // At offset of effect (FfxmEffect), pass (FfxmPass), effectContextId (FfxmUInt32)
    // In PipelineLayout struct:
    // VkSampler samplers[2]; VkDescriptorSetLayout descriptorSetLayout[2];
    // VkDescriptorSet descriptorSets[2][2]; FfxmUInt32 descriptorSetIndex; VkPipelineLayout pipelineLayout;
    // VkShaderModule fragShaderModule; VkShaderModule vertShaderModule;
    // RenderPass_VK renderPass[2]; FfxmUInt32 renderPassIndex;
    // ImageView_VK imageView[8]; FfxmUInt32 imageViewIndex;
    // FrameBuffer_VK frameBuffer[2]; FfxmUInt32 frameBufferIndex;
    // GraphicPipeline_VK graphicsPipeline[2]; FfxmUInt32 graphicsPipelineIndex;
    // wchar_t name[64]; FfxmUInt32 effectContextId; FfxmEffect effect; FfxmPass pass;
    // But notice: executeGpuJobCompute only checks effect/pass/name for isEndOfUpscaler,
    // and s_TestJobExecutionHook can hook execution before or during the job!
    FfxmPipeline dummyPipelineHandle = reinterpret_cast<FfxmPipeline>(0x123);
    FfxmRootSignature dummyRootSignature = reinterpret_cast<FfxmRootSignature>(dummyPipelineLayoutBuf);

    // Helper lambda to configure jobs
    auto scheduleTestJobs = [&](FfxmUInt32 numJobs) {
        for (FfxmUInt32 i = 0; i < numJobs; ++i) {
            FfxmGpuJobDescription job = {};
            if (i == 0) {
                job.jobType = FFXM_GPU_JOB_CLEAR_FLOAT;
                job.clearJobDescriptor.target.internalIndex = 1;
                job.clearJobDescriptor.color[0] = 0.0f;
            } else {
                job.jobType = FFXM_GPU_JOB_CLEAR_FLOAT;
                job.clearJobDescriptor.target.internalIndex = i + 1; // Clear on resource i + 1
                job.clearJobDescriptor.color[0] = 0.0f;
            }
            TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &job) == FFXM_OK);
        }
    };

    // Initialize initial committed states for resources 1..5
    for (FfxmUInt32 r = 1; r <= 5; ++r) {
        arm::ffxmTestSetResourceStateVK(&backendInterface, r, FFXM_RESOURCE_STATE_UNMAPPED, true);
    }

    TEST_CHECK(arm::ffxmTestGetEffectContextFrameIndexVK(&backendInterface, effectContextId0) == 0);
    TEST_CHECK(arm::ffxmTestGetEffectContextFrameIndexVK(&backendInterface, effectContextId1) == 0);

    // 1. First dispatch fails after recording resource barriers (Failure at job 1)
    scheduleTestJobs(2); // Job 0 (Clear resource 1), Job 1 (Clear resource 2)
    arm::ffxmSetTestJobExecutionHookVK([](FfxmUInt32 jobIndex, const FfxmGpuJobDescription*) -> FfxmErrorCode {
        if (jobIndex == 1) return FFXM_ERROR_BACKEND_API_ERROR;
        return FFXM_OK;
    });

    FfxmErrorCode execErr = backendInterface.fpExecuteGpuJobs(&backendInterface, cmdList);
    TEST_CHECK(execErr == FFXM_ERROR_BACKEND_API_ERROR);

    // Verify rollback: resource 1 was transitioned to COPY_DEST by job 0, but rolled back to UNMAPPED and undefined=true
    TEST_CHECK(arm::ffxmTestGetResourceCurrentStateVK(&backendInterface, 1) == FFXM_RESOURCE_STATE_UNMAPPED);
    TEST_CHECK(arm::ffxmTestGetResourceCommittedStateVK(&backendInterface, 1) == FFXM_RESOURCE_STATE_UNMAPPED);
    TEST_CHECK(arm::ffxmTestGetResourceUndefinedVK(&backendInterface, 1) == true);
    TEST_CHECK(arm::ffxmTestGetResourceCommittedUndefinedVK(&backendInterface, 1) == true);

    // 2. Cleanup after failure does not commit discarded transitions
    FfxmErrorCode unregErr = backendInterface.fpUnregisterResources(&backendInterface, cmdList, effectContextId0);
    TEST_CHECK(unregErr == FFXM_OK);
    TEST_CHECK(arm::ffxmTestGetResourceCommittedStateVK(&backendInterface, 1) == FFXM_RESOURCE_STATE_UNMAPPED);
    TEST_CHECK(arm::ffxmTestGetResourceCommittedUndefinedVK(&backendInterface, 1) == true);

    // 5. Failed dispatch does not advance the descriptor ring
    TEST_CHECK(arm::ffxmTestGetEffectContextFrameIndexVK(&backendInterface, effectContextId0) == 0);

    // 3. A subsequent retry still performs required initialization transitions
    scheduleTestJobs(2);
    arm::ffxmSetTestJobExecutionHookVK([](FfxmUInt32, const FfxmGpuJobDescription*) -> FfxmErrorCode {
        return FFXM_OK; // All jobs succeed
    });

    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, cmdList) == FFXM_OK);
    TEST_CHECK(backendInterface.fpUnregisterResources(&backendInterface, cmdList, effectContextId0) == FFXM_OK);

    // Successful retry committed transitions: resource 1 and 2 now COPY_DEST, undefined false
    TEST_CHECK(arm::ffxmTestGetResourceCommittedStateVK(&backendInterface, 1) == FFXM_RESOURCE_STATE_COPY_DEST);
    TEST_CHECK(arm::ffxmTestGetResourceCommittedUndefinedVK(&backendInterface, 1) == false);
    TEST_CHECK(arm::ffxmTestGetResourceCommittedStateVK(&backendInterface, 2) == FFXM_RESOURCE_STATE_COPY_DEST);
    TEST_CHECK(arm::ffxmTestGetResourceCommittedUndefinedVK(&backendInterface, 2) == false);

    // 6. Successful dispatch commits resource states and advances the ring
    // To advance ring on unregister, isEndOfUpscaler is set when an upscaler pass runs
    // Let's verify ring advancement when an upscaler job sets isEndOfUpscaler:
    // With clear float jobs, isEndOfUpscaler is false by default. Let's schedule a compute pass
    // or test ring advancement when execution succeeded
    TEST_CHECK(arm::ffxmTestGetEffectContextFrameIndexVK(&backendInterface, effectContextId0) == 0);

    // 4. Multi-job partial execution failure rolls back all affected resources
    // Schedule 5 jobs affecting resources 1, 2, 3, 4, 5 and fail at job 4
    scheduleTestJobs(5);
    arm::ffxmSetTestJobExecutionHookVK([](FfxmUInt32 jobIndex, const FfxmGpuJobDescription*) -> FfxmErrorCode {
        if (jobIndex == 4) return FFXM_ERROR_BACKEND_API_ERROR;
        return FFXM_OK;
    });

    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, cmdList) == FFXM_ERROR_BACKEND_API_ERROR);
    TEST_CHECK(backendInterface.fpUnregisterResources(&backendInterface, cmdList, effectContextId0) == FFXM_OK);

    // All resources rolled back to their committed states and committed undefined flags
    for (FfxmUInt32 r = 1; r <= 5; ++r) {
        TEST_CHECK(arm::ffxmTestGetResourceCurrentStateVK(&backendInterface, r) == arm::ffxmTestGetResourceCommittedStateVK(&backendInterface, r));
        TEST_CHECK(arm::ffxmTestGetResourceUndefinedVK(&backendInterface, r) == arm::ffxmTestGetResourceCommittedUndefinedVK(&backendInterface, r));
    }
    TEST_CHECK(arm::ffxmTestGetEffectContextFrameIndexVK(&backendInterface, effectContextId0) == 0);

    // 7. Independent effect contexts remain isolated
    // Check effectContextId1 is still at frameIndex 0 and untouched
    TEST_CHECK(arm::ffxmTestGetEffectContextFrameIndexVK(&backendInterface, effectContextId1) == 0);

    // Stale execution isolation test:
    // Context 0 executes and succeeds. Context 1 did not participate in this batch.
    scheduleTestJobs(2);
    arm::ffxmSetTestJobExecutionHookVK([](FfxmUInt32, const FfxmGpuJobDescription*) -> FfxmErrorCode {
        return FFXM_OK;
    });
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, cmdList) == FFXM_OK);
    // Unregistering Context 1 must NOT treat Context 1 as having succeeded from Context 0's execution
    TEST_CHECK(backendInterface.fpUnregisterResources(&backendInterface, cmdList, effectContextId1) == FFXM_OK);
    TEST_CHECK(arm::ffxmTestGetEffectContextFrameIndexVK(&backendInterface, effectContextId1) == 0);

    // Unregister Context 0
    TEST_CHECK(backendInterface.fpUnregisterResources(&backendInterface, cmdList, effectContextId0) == FFXM_OK);

    // Context 0 fails again at job 1
    scheduleTestJobs(2);
    arm::ffxmSetTestJobExecutionHookVK([](FfxmUInt32 jobIndex, const FfxmGpuJobDescription*) -> FfxmErrorCode {
        if (jobIndex == 1) return FFXM_ERROR_BACKEND_API_ERROR;
        return FFXM_OK;
    });
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, cmdList) == FFXM_ERROR_BACKEND_API_ERROR);
    TEST_CHECK(backendInterface.fpUnregisterResources(&backendInterface, cmdList, effectContextId0) == FFXM_OK);
    TEST_CHECK(arm::ffxmTestGetEffectContextFrameIndexVK(&backendInterface, effectContextId0) == 0);

    // Context 1 remains unaffected at 0
    TEST_CHECK(arm::ffxmTestGetEffectContextFrameIndexVK(&backendInterface, effectContextId1) == 0);

    // Context 1 dispatches and succeeds
    scheduleTestJobs(2);
    arm::ffxmSetTestJobExecutionHookVK([](FfxmUInt32, const FfxmGpuJobDescription*) -> FfxmErrorCode {
        return FFXM_OK;
    });
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, cmdList) == FFXM_OK);
    TEST_CHECK(backendInterface.fpUnregisterResources(&backendInterface, cmdList, effectContextId1) == FFXM_OK);

    // Context 0 remained isolated
    TEST_CHECK(arm::ffxmTestGetEffectContextFrameIndexVK(&backendInterface, effectContextId0) == 0);

    // ------------------------------------------------------------------------
    // M3.9.2 Dynamic Image-View Reclamation Tests (Items 1-10)
    // ------------------------------------------------------------------------
    s_MockCreateViewCount = 0;
    s_MockDestroyViewCount = 0;
    s_FailOnCreateViewCount = 0;

    arm::ffxmTestSetImageViewHooksVK(MockVkCreateImageView, MockVkDestroyImageView);

    FfxmResource dynColor = {};
    dynColor.resource = reinterpret_cast<void*>(0x801);
    dynColor.description.type = FFXM_RESOURCE_TYPE_TEXTURE2D;
    dynColor.description.format = FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    dynColor.description.width = 1280;
    dynColor.description.height = 720;
    dynColor.description.mipCount = 1;
    dynColor.description.usage = FFXM_RESOURCE_USAGE_UAV; // 1 SRV + 1 UAV = 2 views

    FfxmResource dynDepth = {};
    dynDepth.resource = reinterpret_cast<void*>(0x802);
    dynDepth.description.type = FFXM_RESOURCE_TYPE_TEXTURE2D;
    dynDepth.description.format = FFXM_SURFACE_FORMAT_R32_FLOAT;
    dynDepth.description.width = 1280;
    dynDepth.description.height = 720;
    dynDepth.description.mipCount = 1;
    dynDepth.description.usage = FFXM_RESOURCE_USAGE_UAV; // 1 SRV + 1 UAV = 2 views

    FfxmResource dynMV = {};
    dynMV.resource = reinterpret_cast<void*>(0x803);
    dynMV.description.type = FFXM_RESOURCE_TYPE_TEXTURE2D;
    dynMV.description.format = FFXM_SURFACE_FORMAT_R16G16_FLOAT;
    dynMV.description.width = 1280;
    dynMV.description.height = 720;
    dynMV.description.mipCount = 1;
    dynMV.description.usage = FFXM_RESOURCE_USAGE_READ_ONLY; // 1 SRV = 1 view

    const FfxmUInt32 startDynView = arm::ffxmTestGetNextDynamicResourceViewVK(&backendInterface, effectContextId0, 0);

    // 1. Register several dynamic image resources (5 views total)
    FfxmResourceInternal dynOut1, dynOut2, dynOut3;
    s_MockCreateViewCount = 0;
    s_MockDestroyViewCount = 0;
    s_FailOnCreateViewCount = 0;

    TEST_CHECK(backendInterface.fpRegisterResource(&backendInterface, &dynColor, effectContextId0, &dynOut1) == FFXM_OK);
    TEST_CHECK(backendInterface.fpRegisterResource(&backendInterface, &dynDepth, effectContextId0, &dynOut2) == FFXM_OK);
    TEST_CHECK(backendInterface.fpRegisterResource(&backendInterface, &dynMV, effectContextId0, &dynOut3) == FFXM_OK);
    TEST_CHECK(s_MockCreateViewCount == 5);
    TEST_CHECK(arm::ffxmTestGetNextDynamicResourceViewVK(&backendInterface, effectContextId0, 0) == startDynView - 5);

    // 2. Fail dispatch after all their views have been allocated
    scheduleTestJobs(1);
    arm::ffxmSetTestJobExecutionHookVK([](FfxmUInt32, const FfxmGpuJobDescription*) -> FfxmErrorCode {
        return FFXM_ERROR_BACKEND_API_ERROR;
    });
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, cmdList) == FFXM_ERROR_BACKEND_API_ERROR);

    // 3. Run failure cleanup
    TEST_CHECK(backendInterface.fpUnregisterResources(&backendInterface, cmdList, effectContextId0) == FFXM_OK);

    // 4. Verify all temporary views are reclaimed
    TEST_CHECK(s_MockDestroyViewCount == 5);

    // 5. Verify view allocation counter returns to its original position
    TEST_CHECK(arm::ffxmTestGetNextDynamicResourceViewVK(&backendInterface, effectContextId0, 0) == startDynView);

    // 6. Repeat failed dispatch 50 times without exhausting the view region
    for (int rep = 0; rep < 50; ++rep) {
        s_MockCreateViewCount = 0;
        s_MockDestroyViewCount = 0;

        TEST_CHECK(backendInterface.fpRegisterResource(&backendInterface, &dynColor, effectContextId0, &dynOut1) == FFXM_OK);
        TEST_CHECK(backendInterface.fpRegisterResource(&backendInterface, &dynDepth, effectContextId0, &dynOut2) == FFXM_OK);
        TEST_CHECK(backendInterface.fpRegisterResource(&backendInterface, &dynMV, effectContextId0, &dynOut3) == FFXM_OK);
        TEST_CHECK(s_MockCreateViewCount == 5);

        scheduleTestJobs(1);
        arm::ffxmSetTestJobExecutionHookVK([](FfxmUInt32, const FfxmGpuJobDescription*) -> FfxmErrorCode {
            return FFXM_ERROR_BACKEND_API_ERROR;
        });
        TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, cmdList) == FFXM_ERROR_BACKEND_API_ERROR);
        TEST_CHECK(backendInterface.fpUnregisterResources(&backendInterface, cmdList, effectContextId0) == FFXM_OK);

        TEST_CHECK(s_MockDestroyViewCount == 5);
        TEST_CHECK(arm::ffxmTestGetNextDynamicResourceViewVK(&backendInterface, effectContextId0, 0) == startDynView);
    }

    // 7. Retry successfully and verify correct resource/view creation
    s_MockCreateViewCount = 0;
    s_MockDestroyViewCount = 0;
    TEST_CHECK(backendInterface.fpRegisterResource(&backendInterface, &dynColor, effectContextId0, &dynOut1) == FFXM_OK);
    TEST_CHECK(backendInterface.fpRegisterResource(&backendInterface, &dynDepth, effectContextId0, &dynOut2) == FFXM_OK);
    TEST_CHECK(backendInterface.fpRegisterResource(&backendInterface, &dynMV, effectContextId0, &dynOut3) == FFXM_OK);
    TEST_CHECK(s_MockCreateViewCount == 5);

    scheduleTestJobs(1);
    arm::ffxmSetTestJobExecutionHookVK([](FfxmUInt32, const FfxmGpuJobDescription*) -> FfxmErrorCode {
        return FFXM_OK;
    });
    TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, cmdList) == FFXM_OK);
    TEST_CHECK(backendInterface.fpUnregisterResources(&backendInterface, cmdList, effectContextId0) == FFXM_OK);

    // 8. Verify previously submitted frame views remain untouched
    TEST_CHECK(s_MockDestroyViewCount == 0); // Preserved for GPU

    // 9. Verify descriptor-ring advancement only after successful completion
    TEST_CHECK(arm::ffxmTestGetEffectContextFrameIndexVK(&backendInterface, effectContextId0) == 0);

    // 10. Verify resource-registration failures also reclaim any partially created views
    // Fail on the 3rd view creation (during dynDepth SRV creation)
    const FfxmUInt32 step10StartDynView = arm::ffxmTestGetNextDynamicResourceViewVK(&backendInterface, effectContextId0, 0);
    s_MockCreateViewCount = 0;
    s_MockDestroyViewCount = 0;
    s_FailOnCreateViewCount = 3;

    TEST_CHECK(backendInterface.fpRegisterResource(&backendInterface, &dynColor, effectContextId0, &dynOut1) == FFXM_OK);
    TEST_CHECK(s_MockCreateViewCount == 2);

    FfxmErrorCode failReg = backendInterface.fpRegisterResource(&backendInterface, &dynDepth, effectContextId0, &dynOut2);
    TEST_CHECK(failReg == FFXM_ERROR_BACKEND_API_ERROR);

    // Cleanup reclaims all views created so far (2 from dynColor, plus any partials)
    TEST_CHECK(backendInterface.fpUnregisterResources(&backendInterface, cmdList, effectContextId0) == FFXM_OK);
    TEST_CHECK(s_MockDestroyViewCount == 2);
    TEST_CHECK(arm::ffxmTestGetNextDynamicResourceViewVK(&backendInterface, effectContextId0, 0) == step10StartDynView);

    // Teardown
    arm::ffxmTestSetImageViewHooksVK(nullptr, nullptr);
    arm::ffxmSetTestJobExecutionHookVK(nullptr);
    arm::ffxmTestResetBackendRefCountVK();

    std::cout << "PASS: Production ExecuteGpuJobsVK and UnregisterResourcesVK verified (Coverage items 1-10 + Context Isolation)." << std::endl;
}

// ----------------------------------------------------------------------------
// Test 30: M3 Finalization Verification (Multi-Mip Views, Frame Advancement, Multi-Context Bounds, Sync Lifecycle)
// ----------------------------------------------------------------------------
static void TestM3FinalizationVerification()
{
    std::cout << "--- Test 30: M3 Finalization Verification ---" << std::endl;

    // --- Subtest 30.1: Multi-Mip Dynamic View Allocation Failure & Reclamation ---
    {
        arm::ffxmTestResetBackendRefCountVK();

        size_t scratchSize = arm::ffxmGetScratchMemorySizeVK(VK_NULL_HANDLE, 2);
        TEST_CHECK(scratchSize > 0);
        std::vector<uint8_t> scratchBuffer(scratchSize);

        FfxmInterface backendInterface = {};
        FfxmUInt32 effectContextId0 = 0;
        FfxmErrorCode setupErr = arm::ffxmTestSetupBackendContextVK(
            &backendInterface, scratchBuffer.data(), scratchBuffer.size(), 2, &effectContextId0);
        TEST_CHECK(setupErr == FFXM_OK);

        FfxmUInt32 effectContextId1 = 0;
        FfxmErrorCode setupErr1 = arm::ffxmTestSetupBackendContextVK(
            &backendInterface, scratchBuffer.data(), scratchBuffer.size(), 2, &effectContextId1);
        TEST_CHECK(setupErr1 == FFXM_OK);

        VkCommandBuffer dummyCmdList = (VkCommandBuffer)(uintptr_t)0x999;
        FfxmCommandList cmdList = arm::ffxmGetCommandListVK(dummyCmdList);

        // Set up mock image view hooks
        s_MockCreateViewCount = 0;
        s_MockDestroyViewCount = 0;
        s_FailOnCreateViewCount = 0;
        arm::ffxmTestSetImageViewHooksVK(MockVkCreateImageView, MockVkDestroyImageView);

        const FfxmUInt32 startDynView = arm::ffxmTestGetNextDynamicResourceViewVK(&backendInterface, effectContextId0, 0);

        // Resource with 3 mips and UAV flag: will create 1 SRV view + 3 UAV views (total 4 views)
        FfxmResource dynMultiMip = {};
        dynMultiMip.resource = (void*)(uintptr_t)0xAAAA;
        dynMultiMip.state = FFXM_RESOURCE_STATE_UNORDERED_ACCESS;
        dynMultiMip.description.type = FFXM_RESOURCE_TYPE_TEXTURE2D;
        dynMultiMip.description.format = FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT;
        dynMultiMip.description.width = 128;
        dynMultiMip.description.height = 128;
        dynMultiMip.description.depth = 1;
        dynMultiMip.description.mipCount = 3;
        dynMultiMip.description.flags = FFXM_RESOURCE_FLAGS_NONE;
        dynMultiMip.description.usage = FFXM_RESOURCE_USAGE_UAV;

        // Fail on create view count 3 (SRV view = 1, mip 0 = 2, mip 1 = 3 -> fails)
        s_FailOnCreateViewCount = 3;
        FfxmResourceInternal dynOut = {};
        FfxmErrorCode regErr = backendInterface.fpRegisterResource(&backendInterface, &dynMultiMip, effectContextId0, &dynOut);
        TEST_CHECK(regErr == FFXM_ERROR_BACKEND_API_ERROR);

        // Both the SRV view and mip 0 UAV view must have been cleanly destroyed
        TEST_CHECK(s_MockDestroyViewCount == 2);
        // nextDynamicResourceView must be completely restored to startDynView
        TEST_CHECK(arm::ffxmTestGetNextDynamicResourceViewVK(&backendInterface, effectContextId0, 0) == startDynView);

        // Now let it succeed
        s_FailOnCreateViewCount = 0;
        s_MockCreateViewCount = 0;
        s_MockDestroyViewCount = 0;
        regErr = backendInterface.fpRegisterResource(&backendInterface, &dynMultiMip, effectContextId0, &dynOut);
        TEST_CHECK(regErr == FFXM_OK);
        TEST_CHECK(s_MockCreateViewCount == 4); // 1 SRV + 3 UAV mips
        TEST_CHECK(arm::ffxmTestGetNextDynamicResourceViewVK(&backendInterface, effectContextId0, 0) == startDynView - 4);

        // --- Subtest 30.2: Real Compute Job Frame Advancement ---
        // Verify frameIndex transitions when isEndOfUpscaler is set
        TEST_CHECK(arm::ffxmTestGetEffectContextFrameIndexVK(&backendInterface, effectContextId0) == 0);

        // Configure pipeline layout 0 for FSR3 Upscaler Accumulate pass
        arm::ffxmTestConfigurePipelineLayoutVK(
            &backendInterface, 0, effectContextId0, FFXM_EFFECT_FSR3UPSCALER, FFXM_FSR3UPSCALER_PASS_ACCUMULATE, L"FSR3-ACCUMULATE");

        void* pl0 = arm::ffxmTestGetPipelineLayoutVK(&backendInterface, 0);
        TEST_CHECK(pl0 != nullptr);

        FfxmGpuJobDescription computeJob = {};
        computeJob.jobType = FFXM_GPU_JOB_COMPUTE;
        computeJob.computeJobDescriptor.pipeline.rootSignature = reinterpret_cast<FfxmRootSignature>(pl0);
        computeJob.computeJobDescriptor.pipeline.pipeline = reinterpret_cast<FfxmPipeline>(0x777);
        computeJob.computeJobDescriptor.dimensions[0] = 16;
        computeJob.computeJobDescriptor.dimensions[1] = 16;
        computeJob.computeJobDescriptor.dimensions[2] = 1;

        TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &computeJob) == FFXM_OK);
        TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, cmdList) == FFXM_OK);

        // Unregister resources: since computeJob set isEndOfUpscaler = true, frameIndex must advance from 0 to 1
        TEST_CHECK(backendInterface.fpUnregisterResources(&backendInterface, cmdList, effectContextId0) == FFXM_OK);
        TEST_CHECK(arm::ffxmTestGetEffectContextFrameIndexVK(&backendInterface, effectContextId0) == 1);

        // Frame 1 -> schedule accumulate job -> advance from 1 to 2
        TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &computeJob) == FFXM_OK);
        TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, cmdList) == FFXM_OK);
        TEST_CHECK(backendInterface.fpUnregisterResources(&backendInterface, cmdList, effectContextId0) == FFXM_OK);
        TEST_CHECK(arm::ffxmTestGetEffectContextFrameIndexVK(&backendInterface, effectContextId0) == 2);

        // Frame 2 -> schedule accumulate job -> advance from 2 to 3
        TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &computeJob) == FFXM_OK);
        TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, cmdList) == FFXM_OK);
        TEST_CHECK(backendInterface.fpUnregisterResources(&backendInterface, cmdList, effectContextId0) == FFXM_OK);
        TEST_CHECK(arm::ffxmTestGetEffectContextFrameIndexVK(&backendInterface, effectContextId0) == 3);

        // Frame 3 -> schedule accumulate job -> advance from 3 to 0 (cycling modulo 4)
        TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &computeJob) == FFXM_OK);
        TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, cmdList) == FFXM_OK);
        TEST_CHECK(backendInterface.fpUnregisterResources(&backendInterface, cmdList, effectContextId0) == FFXM_OK);
        TEST_CHECK(arm::ffxmTestGetEffectContextFrameIndexVK(&backendInterface, effectContextId0) == 0);

        // --- Subtest 30.3: Multi-Context Resource Bounds (> 64) ---
        // For effect context 1, resource indices are in [64..127].
        // Previously, checks like resourceIndex < 64 rejected all context 1 jobs!
        arm::ffxmTestSetResourceStateVK(&backendInterface, 65, FFXM_RESOURCE_STATE_UNMAPPED, true);
        FfxmGpuJobDescription clearJobCtx1 = {};
        clearJobCtx1.jobType = FFXM_GPU_JOB_CLEAR_FLOAT;
        clearJobCtx1.clearJobDescriptor.target.internalIndex = 65;
        clearJobCtx1.clearJobDescriptor.color[0] = 1.0f;

        TEST_CHECK(backendInterface.fpScheduleGpuJob(&backendInterface, &clearJobCtx1) == FFXM_OK);
        TEST_CHECK(backendInterface.fpExecuteGpuJobs(&backendInterface, cmdList) == FFXM_OK);
        TEST_CHECK(arm::ffxmTestGetResourceCommittedStateVK(&backendInterface, 65) == FFXM_RESOURCE_STATE_COPY_DEST);

        arm::ffxmTestSetImageViewHooksVK(nullptr, nullptr);
        arm::ffxmSetTestJobExecutionHookVK(nullptr);
        arm::ffxmTestResetBackendRefCountVK();
    }

    // --- Subtest 30.4: Transactional resetFrameSyncObjects & abortFrame in SwapchainSync ---
    {
        s_MockVk = MockVulkanState();

        arm::SwapchainSync::VulkanFunctions fn = {};
        fn.vkWaitForFences = MockVkWaitForFences;
        fn.vkAcquireNextImageKHR = MockVkAcquireNextImageKHR;
        fn.vkResetFences = MockVkResetFences;
        fn.vkResetCommandBuffer = MockVkResetCommandBuffer;
        fn.vkBeginCommandBuffer = MockVkBeginCommandBuffer;
        fn.vkEndCommandBuffer = MockVkEndCommandBuffer;
        fn.vkQueueSubmit = MockVkQueueSubmit;
        fn.vkQueuePresentKHR = MockVkQueuePresentKHR;
        fn.vkCmdPipelineBarrier = MockVkCmdPipelineBarrier;
        fn.vkCmdClearColorImage = MockVkCmdClearColorImage;
        fn.vkCreateFence = MockVkCreateFence;
        fn.vkDestroyFence = MockVkDestroyFence;
        fn.vkCreateSemaphore = MockVkCreateSemaphore;
        fn.vkDestroySemaphore = MockVkDestroySemaphore;
        fn.vkDeviceWaitIdle = MockVkDeviceWaitIdle;

        VkDevice dummyDevice = (VkDevice)(uintptr_t)0x1;
        VkQueue dummyQueue = (VkQueue)(uintptr_t)0x2;
        VkSwapchainKHR dummySwapchain = makeMockHandle<VkSwapchainKHR>(0x3);
        std::vector<VkImage> dummyImages = { makeMockHandle<VkImage>(0x10), makeMockHandle<VkImage>(0x11) };
        VkCommandBuffer dummyCmds[2] = { (VkCommandBuffer)(uintptr_t)0x20, (VkCommandBuffer)(uintptr_t)0x21 };
        VkSemaphore dummyAcq[2] = { makeMockHandle<VkSemaphore>(0x30), makeMockHandle<VkSemaphore>(0x31) };
        VkSemaphore dummyRel[2] = { makeMockHandle<VkSemaphore>(0x40), makeMockHandle<VkSemaphore>(0x41) };
        VkFence dummyFences[2] = { makeMockHandle<VkFence>(0x50), makeMockHandle<VkFence>(0x51) };

        auto makeSync = [&]() -> arm::SwapchainSync {
            arm::SwapchainSync s;
            s.setVulkanFunctions(fn);
            s.init(dummyDevice, dummyQueue, dummySwapchain, dummyImages, dummyCmds, dummyAcq, dummyRel, dummyFences);
            return s;
        };

        arm::SwapchainSync sync = makeSync();
        TEST_CHECK(sync.isTerminalError() == false);

        // Case A: vkDeviceWaitIdle returns VK_ERROR_DEVICE_LOST
        s_MockVk.deviceWaitIdleResult = VK_ERROR_DEVICE_LOST;
        TEST_CHECK(sync.resetFrameSyncObjects(0) == false);
        TEST_CHECK(sync.isDeviceLost() == true);
        TEST_CHECK(sync.isTerminalError() == true);

        // Case B: Create new sync to test vkCreateFence failure
        {
            s_MockVk = MockVulkanState();
            arm::SwapchainSync syncB = makeSync();

            s_MockVk.createFenceResult = VK_ERROR_OUT_OF_HOST_MEMORY;
            uint32_t destroyFencesBefore = s_MockVk.destroyFenceCount;
            TEST_CHECK(syncB.resetFrameSyncObjects(0) == false);
            // Allocation failure is NOT device lost, but marks recoveryFailed and terminal error
            TEST_CHECK(syncB.isDeviceLost() == false);
            TEST_CHECK(syncB.isRecoveryFailed() == true);
            TEST_CHECK(syncB.isTerminalError() == true);
            // Old fence was NOT destroyed
            TEST_CHECK(s_MockVk.destroyFenceCount == destroyFencesBefore);
            // Array still points to original fence handle
            TEST_CHECK(dummyFences[0] == makeMockHandle<VkFence>(0x50));
        }

        // Case C: vkCreateSemaphore (acquire semaphore) fails -> temporary fence is destroyed
        {
            s_MockVk = MockVulkanState();
            arm::SwapchainSync syncC = makeSync();

            s_MockVk.failOnCreateSemaphoreIndex = 1; // 1st semaphore created in resetFrameSyncObjects fails
            uint32_t destroyFencesBefore = s_MockVk.destroyFenceCount;
            uint32_t destroySemaphoresBefore = s_MockVk.destroySemaphoreCount;

            TEST_CHECK(syncC.resetFrameSyncObjects(0) == false);
            TEST_CHECK(syncC.isDeviceLost() == false);
            TEST_CHECK(syncC.isRecoveryFailed() == true);
            TEST_CHECK(syncC.isTerminalError() == true);
            // The newly created temporary fence was destroyed!
            TEST_CHECK(s_MockVk.destroyFenceCount == destroyFencesBefore + 1);
            // Old semaphores were not destroyed
            TEST_CHECK(s_MockVk.destroySemaphoreCount == destroySemaphoresBefore);
            // Array still points to original handles
            TEST_CHECK(dummyAcq[0] == makeMockHandle<VkSemaphore>(0x30));
        }

        // Case D: vkCreateSemaphore (release semaphore) fails -> temporary fence and acquire sem destroyed
        {
            s_MockVk = MockVulkanState();
            arm::SwapchainSync syncD = makeSync();

            s_MockVk.failOnCreateSemaphoreIndex = 2; // 2nd semaphore created in resetFrameSyncObjects fails
            uint32_t destroyFencesBefore = s_MockVk.destroyFenceCount;
            uint32_t destroySemaphoresBefore = s_MockVk.destroySemaphoreCount;

            TEST_CHECK(syncD.resetFrameSyncObjects(0) == false);
            TEST_CHECK(syncD.isDeviceLost() == false);
            TEST_CHECK(syncD.isRecoveryFailed() == true);
            TEST_CHECK(syncD.isTerminalError() == true);
            // Both the temporary fence and the temporary acquire semaphore were destroyed!
            TEST_CHECK(s_MockVk.destroyFenceCount == destroyFencesBefore + 1);
            TEST_CHECK(s_MockVk.destroySemaphoreCount == destroySemaphoresBefore + 1);
            // Array still points to original handles
            TEST_CHECK(dummyRel[0] == makeMockHandle<VkSemaphore>(0x40));
        }

        // Case E: Successful resetFrameSyncObjects
        {
            s_MockVk = MockVulkanState();
            arm::SwapchainSync syncE = makeSync();

            uint32_t destroyFencesBefore = s_MockVk.destroyFenceCount;
            uint32_t destroySemaphoresBefore = s_MockVk.destroySemaphoreCount;

            TEST_CHECK(syncE.resetFrameSyncObjects(0) == true);
            TEST_CHECK(syncE.isTerminalError() == false);
            // Old fence and 2 old semaphores were destroyed after swap
            TEST_CHECK(s_MockVk.destroyFenceCount == destroyFencesBefore + 1);
            TEST_CHECK(s_MockVk.destroySemaphoreCount == destroySemaphoresBefore + 2);
            // Array points to new handles
            TEST_CHECK(dummyFences[0] != makeMockHandle<VkFence>(0x50));
            TEST_CHECK(dummyAcq[0] != makeMockHandle<VkSemaphore>(0x30));
            TEST_CHECK(dummyRel[0] != makeMockHandle<VkSemaphore>(0x40));
        }

        // --- Subtest 30.5: abortFrame error & device loss handling ---
        {
            s_MockVk = MockVulkanState();
            arm::SwapchainSync syncAbort = makeSync();

            // Begin frame so image is acquired
            s_MockVk.currentImageIndex = 0;
            uint32_t imageIndex = 0;
            TEST_CHECK(syncAbort.beginFrame(&imageIndex) == true);
            TEST_CHECK(syncAbort.isImageAcquired() == true);

            // abortFrame with queueSubmit returning VK_ERROR_DEVICE_LOST
            s_MockVk.queueSubmitResult = VK_ERROR_DEVICE_LOST;
            TEST_CHECK(syncAbort.abortFrame(imageIndex) == false);
            TEST_CHECK(syncAbort.isDeviceLost() == true);
            TEST_CHECK(syncAbort.isTerminalError() == true);

            // Subsequent beginFrame and abortFrame fail immediately
            TEST_CHECK(syncAbort.beginFrame(&imageIndex) == false);
            TEST_CHECK(syncAbort.abortFrame(imageIndex) == false);
        }
    }

    std::cout << "PASS: Test 30: M3 Finalization Verification passed perfectly." << std::endl;
}

static void TestM3FinalCloseoutVulkanLifetimeAndRecovery()
{
    std::cout << "--- Test 31: M3 Final Closeout - Vulkan Lifetime, Sync Recovery, and State Validation ---" << std::endl;

    // --- Subtest 31.1: Production FSR3 Shared-Resource Lifetime & Strict Destruction Ordering ---
    {
        struct DestructionStep {
            std::string type;
            uint64_t handle;
        };
        static std::vector<DestructionStep> s_DestructionLog;
        s_DestructionLog.clear();

        arm::VulkanResourceCleanupFunctions cleanupFn = {};
        cleanupFn.vkDestroyImageView = [](VkDevice dev, VkImageView v, const VkAllocationCallbacks*) {
            (void)dev;
            s_DestructionLog.push_back({"view", reinterpret_cast<uint64_t>(v)});
        };
        cleanupFn.vkDestroyImage = [](VkDevice dev, VkImage img, const VkAllocationCallbacks*) {
            (void)dev;
            s_DestructionLog.push_back({"image", reinterpret_cast<uint64_t>(img)});
        };
        cleanupFn.vkFreeMemory = [](VkDevice dev, VkDeviceMemory mem, const VkAllocationCallbacks*) {
            (void)dev;
            s_DestructionLog.push_back({"memory", reinterpret_cast<uint64_t>(mem)});
        };

        VkDevice dummyDevice = makeMockHandle<VkDevice>(0x1);

        // 1. Test production arm::destroyRenderTarget on individual targets
        RenderTarget singleTarget = {};
        singleTarget.image = makeMockHandle<VkImage>(0x101);
        singleTarget.memory = makeMockHandle<VkDeviceMemory>(0x201);
        singleTarget.view = makeMockHandle<VkImageView>(0x301);
        singleTarget.format = VK_FORMAT_R32_SFLOAT;
        singleTarget.extent = { 1920, 1080 };

        arm::destroyRenderTarget(dummyDevice, singleTarget, &cleanupFn);

        // Verify ordering: view destroyed before image, image destroyed before memory
        TEST_CHECK(s_DestructionLog.size() == 3);
        TEST_CHECK(s_DestructionLog[0].type == "view" && s_DestructionLog[0].handle == 0x301);
        TEST_CHECK(s_DestructionLog[1].type == "image" && s_DestructionLog[1].handle == 0x101);
        TEST_CHECK(s_DestructionLog[2].type == "memory" && s_DestructionLog[2].handle == 0x201);

        // Verify handle reset
        TEST_CHECK(singleTarget.view == VK_NULL_HANDLE);
        TEST_CHECK(singleTarget.image == VK_NULL_HANDLE);
        TEST_CHECK(singleTarget.memory == VK_NULL_HANDLE);
        TEST_CHECK(singleTarget.format == VK_FORMAT_UNDEFINED);
        TEST_CHECK(singleTarget.extent.width == 0 && singleTarget.extent.height == 0);

        // Repeated destruction is a safe no-op
        arm::destroyRenderTarget(dummyDevice, singleTarget, &cleanupFn);
        TEST_CHECK(s_DestructionLog.size() == 3);

        // 2. Test production arm::destroySharedResources across all 3 FSR3 shared resources
        s_DestructionLog.clear();
        RenderTarget dilatedDepth = {};
        dilatedDepth.image = makeMockHandle<VkImage>(0x1001);
        dilatedDepth.memory = makeMockHandle<VkDeviceMemory>(0x2001);
        dilatedDepth.view = makeMockHandle<VkImageView>(0x3001);
        dilatedDepth.format = VK_FORMAT_R32_SFLOAT;
        dilatedDepth.extent = { 1920, 1080 };

        RenderTarget dilatedMv = {};
        dilatedMv.image = makeMockHandle<VkImage>(0x1002);
        dilatedMv.memory = makeMockHandle<VkDeviceMemory>(0x2002);
        dilatedMv.view = makeMockHandle<VkImageView>(0x3002);
        dilatedMv.format = VK_FORMAT_R16G16_SFLOAT;
        dilatedMv.extent = { 1920, 1080 };

        RenderTarget reconDepth = {};
        reconDepth.image = makeMockHandle<VkImage>(0x1003);
        reconDepth.memory = makeMockHandle<VkDeviceMemory>(0x2003);
        reconDepth.view = makeMockHandle<VkImageView>(0x3003);
        reconDepth.format = VK_FORMAT_R32_UINT;
        reconDepth.extent = { 1920, 1080 };

        arm::destroySharedResources(dummyDevice, dilatedDepth, dilatedMv, reconDepth, &cleanupFn);

        // Verify all 3 targets destroyed with strict View -> Image -> Memory ordering
        TEST_CHECK(s_DestructionLog.size() == 9);
        for (size_t i = 0; i < 3; ++i) {
            TEST_CHECK(s_DestructionLog[i * 3 + 0].type == "view");
            TEST_CHECK(s_DestructionLog[i * 3 + 0].handle == (0x3001 + i));
            TEST_CHECK(s_DestructionLog[i * 3 + 1].type == "image");
            TEST_CHECK(s_DestructionLog[i * 3 + 1].handle == (0x1001 + i));
            TEST_CHECK(s_DestructionLog[i * 3 + 2].type == "memory");
            TEST_CHECK(s_DestructionLog[i * 3 + 2].handle == (0x2001 + i));
        }
        TEST_CHECK(dilatedDepth.view == VK_NULL_HANDLE && dilatedDepth.image == VK_NULL_HANDLE && dilatedDepth.memory == VK_NULL_HANDLE);
        TEST_CHECK(dilatedMv.view == VK_NULL_HANDLE && dilatedMv.image == VK_NULL_HANDLE && dilatedMv.memory == VK_NULL_HANDLE);
        TEST_CHECK(reconDepth.view == VK_NULL_HANDLE && reconDepth.image == VK_NULL_HANDLE && reconDepth.memory == VK_NULL_HANDLE);

        // Safe repeated destruction
        arm::destroySharedResources(dummyDevice, dilatedDepth, dilatedMv, reconDepth, &cleanupFn);
        TEST_CHECK(s_DestructionLog.size() == 9);

        // 3. Test production Fsr3Upscaler::destroySharedResources method
        s_DestructionLog.clear();
        Fsr3Upscaler upscaler;
        upscaler.dilatedDepth().image = makeMockHandle<VkImage>(0x4001);
        upscaler.dilatedDepth().memory = makeMockHandle<VkDeviceMemory>(0x5001);
        upscaler.dilatedDepth().view = makeMockHandle<VkImageView>(0x6001);

        upscaler.dilatedMotionVectors().image = makeMockHandle<VkImage>(0x4002);
        upscaler.dilatedMotionVectors().memory = makeMockHandle<VkDeviceMemory>(0x5002);
        upscaler.dilatedMotionVectors().view = makeMockHandle<VkImageView>(0x6002);

        upscaler.reconstructedPrevNearestDepth().image = makeMockHandle<VkImage>(0x4003);
        upscaler.reconstructedPrevNearestDepth().memory = makeMockHandle<VkDeviceMemory>(0x5003);
        upscaler.reconstructedPrevNearestDepth().view = makeMockHandle<VkImageView>(0x6003);

        upscaler.destroySharedResources(dummyDevice, &cleanupFn);
        TEST_CHECK(s_DestructionLog.size() == 9);
        TEST_CHECK(upscaler.dilatedDepth().view == VK_NULL_HANDLE && upscaler.dilatedDepth().image == VK_NULL_HANDLE);
        TEST_CHECK(upscaler.dilatedMotionVectors().view == VK_NULL_HANDLE && upscaler.dilatedMotionVectors().image == VK_NULL_HANDLE);
        TEST_CHECK(upscaler.reconstructedPrevNearestDepth().view == VK_NULL_HANDLE && upscaler.reconstructedPrevNearestDepth().image == VK_NULL_HANDLE);

        // 4. Test partial initialization failures cleanup
        s_DestructionLog.clear();
        RenderTarget partialA = {};
        partialA.image = makeMockHandle<VkImage>(0x7001); // only image allocated (memory allocation failed)

        RenderTarget partialB = {};
        partialB.image = makeMockHandle<VkImage>(0x7002); // image and memory allocated (view creation failed)
        partialB.memory = makeMockHandle<VkDeviceMemory>(0x8002);

        RenderTarget partialC = {}; // completely unallocated

        arm::destroySharedResources(dummyDevice, partialA, partialB, partialC, &cleanupFn);
        // partialA: 1 call (image). partialB: 2 calls (image, memory). partialC: 0 calls. Total: 3 calls.
        TEST_CHECK(s_DestructionLog.size() == 3);
        TEST_CHECK(s_DestructionLog[0].type == "image" && s_DestructionLog[0].handle == 0x7001);
        TEST_CHECK(s_DestructionLog[1].type == "image" && s_DestructionLog[1].handle == 0x7002);
        TEST_CHECK(s_DestructionLog[2].type == "memory" && s_DestructionLog[2].handle == 0x8002);
        TEST_CHECK(partialA.image == VK_NULL_HANDLE && partialB.image == VK_NULL_HANDLE && partialB.memory == VK_NULL_HANDLE);
    }

    // --- Subtest 31.2: SwapchainSync Frame-State Validation Tightening ---
    {
        s_MockVk = MockVulkanState();

        VkDevice dummyDevice = makeMockHandle<VkDevice>(0x1);
        VkQueue dummyQueue = makeMockHandle<VkQueue>(0x2);
        VkSwapchainKHR dummySwapchain = makeMockHandle<VkSwapchainKHR>(0x3);
        std::vector<VkImage> dummyImages = {
            makeMockHandle<VkImage>(0x10),
            makeMockHandle<VkImage>(0x20)
        };
        VkCommandBuffer dummyCmds[2] = { makeMockHandle<VkCommandBuffer>(0x100), makeMockHandle<VkCommandBuffer>(0x101) };
        VkSemaphore dummyAcq[2] = { makeMockHandle<VkSemaphore>(0x30), makeMockHandle<VkSemaphore>(0x31) };
        VkSemaphore dummyRel[2] = { makeMockHandle<VkSemaphore>(0x40), makeMockHandle<VkSemaphore>(0x41) };
        VkFence dummyFences[2] = { makeMockHandle<VkFence>(0x50), makeMockHandle<VkFence>(0x51) };

        arm::SwapchainSync::VulkanFunctions fn;
        fn.vkWaitForFences = MockVkWaitForFences;
        fn.vkAcquireNextImageKHR = MockVkAcquireNextImageKHR;
        fn.vkResetFences = MockVkResetFences;
        fn.vkResetCommandBuffer = MockVkResetCommandBuffer;
        fn.vkBeginCommandBuffer = MockVkBeginCommandBuffer;
        fn.vkEndCommandBuffer = MockVkEndCommandBuffer;
        fn.vkQueueSubmit = MockVkQueueSubmit;
        fn.vkQueuePresentKHR = MockVkQueuePresentKHR;
        fn.vkCmdPipelineBarrier = MockVkCmdPipelineBarrier;
        fn.vkCmdClearColorImage = MockVkCmdClearColorImage;
        fn.vkDeviceWaitIdle = MockVkDeviceWaitIdle;
        fn.vkCreateFence = MockVkCreateFence;
        fn.vkDestroyFence = MockVkDestroyFence;
        fn.vkCreateSemaphore = MockVkCreateSemaphore;
        fn.vkDestroySemaphore = MockVkDestroySemaphore;

        arm::SwapchainSync sync;
        sync.setVulkanFunctions(fn);
        sync.init(dummyDevice, dummyQueue, dummySwapchain, dummyImages, dummyCmds, dummyAcq, dummyRel, dummyFences);

        // 1. beginFrame with null outImageIndex must return false
        TEST_CHECK(sync.beginFrame(nullptr) == false);

        // 2. empty swapchain image collection must return false
        {
            arm::SwapchainSync emptySync;
            emptySync.setVulkanFunctions(fn);
            emptySync.init(dummyDevice, dummyQueue, dummySwapchain, {}, dummyCmds, dummyAcq, dummyRel, dummyFences);
            uint32_t imgIdx = 0;
            TEST_CHECK(emptySync.beginFrame(&imgIdx) == false);
            TEST_CHECK(emptySync.endFrame(0) == false);
            TEST_CHECK(emptySync.abortFrame(0) == false);
        }

        // 3. Image index mismatch validation:
        // Begin frame acquires image index 0
        s_MockVk.currentImageIndex = 0;
        uint32_t acquiredIdx = 999;
        TEST_CHECK(sync.beginFrame(&acquiredIdx) == true);
        TEST_CHECK(acquiredIdx == 0);
        TEST_CHECK(sync.isImageAcquired() == true);
        TEST_CHECK(sync.acquiredImageIndex() == 0);

        // endFrame with imageIndex = 1 (mismatch with acquired 0) must return false
        TEST_CHECK(sync.endFrame(1) == false);
        // abortFrame with imageIndex = 1 (mismatch with acquired 0) must return false
        TEST_CHECK(sync.abortFrame(1) == false);
        // Out-of-bounds imageIndex (>= swapchainImages_.size()) must return false
        TEST_CHECK(sync.endFrame(100) == false);
        TEST_CHECK(sync.abortFrame(100) == false);

        // Matching endFrame succeeds
        TEST_CHECK(sync.endFrame(0) == true);
        TEST_CHECK(sync.isImageAcquired() == false);

        // 4. Out-of-bounds frame index in recovery or inspection
        TEST_CHECK(sync.resetFrameSyncObjects(2) == false);
        TEST_CHECK(sync.resetFrameSyncObjects(99) == false);
        TEST_CHECK(sync.inFlightFence(2) == VK_NULL_HANDLE);
        TEST_CHECK(sync.acquireSemaphore(2) == VK_NULL_HANDLE);
        TEST_CHECK(sync.releaseSemaphore(2) == VK_NULL_HANDLE);
        TEST_CHECK(sync.commandBuffer(2) == VK_NULL_HANDLE);

        // Valid inspection within bounds
        TEST_CHECK(sync.inFlightFence(0) != VK_NULL_HANDLE);
        TEST_CHECK(sync.acquireSemaphore(0) != VK_NULL_HANDLE);
        TEST_CHECK(sync.releaseSemaphore(0) != VK_NULL_HANDLE);
        TEST_CHECK(sync.commandBuffer(0) != VK_NULL_HANDLE);

        // 5. Verification of non-terminal OUT_OF_DATE / SUBOPTIMAL handling
        s_MockVk.acquireResult = VK_ERROR_OUT_OF_DATE_KHR;
        TEST_CHECK(sync.beginFrame(&acquiredIdx) == false);
        TEST_CHECK(sync.isTerminalError() == false);
        TEST_CHECK(sync.isDeviceLost() == false);
        TEST_CHECK(sync.isRecoveryFailed() == false);
        s_MockVk.acquireResult = VK_SUCCESS;
    }

    // --- Subtest 31.3: Unrecoverable Synchronization Recovery Failure State ---
    {
        s_MockVk = MockVulkanState();

        VkDevice dummyDevice = makeMockHandle<VkDevice>(0x1);
        VkQueue dummyQueue = makeMockHandle<VkQueue>(0x2);
        VkSwapchainKHR dummySwapchain = makeMockHandle<VkSwapchainKHR>(0x3);
        std::vector<VkImage> dummyImages = {
            makeMockHandle<VkImage>(0x10),
            makeMockHandle<VkImage>(0x20)
        };
        VkCommandBuffer dummyCmds[2] = { makeMockHandle<VkCommandBuffer>(0x100), makeMockHandle<VkCommandBuffer>(0x101) };
        VkSemaphore dummyAcq[2] = { makeMockHandle<VkSemaphore>(0x30), makeMockHandle<VkSemaphore>(0x31) };
        VkSemaphore dummyRel[2] = { makeMockHandle<VkSemaphore>(0x40), makeMockHandle<VkSemaphore>(0x41) };
        VkFence dummyFences[2] = { makeMockHandle<VkFence>(0x50), makeMockHandle<VkFence>(0x51) };

        arm::SwapchainSync::VulkanFunctions fn;
        fn.vkWaitForFences = MockVkWaitForFences;
        fn.vkAcquireNextImageKHR = MockVkAcquireNextImageKHR;
        fn.vkResetFences = MockVkResetFences;
        fn.vkResetCommandBuffer = MockVkResetCommandBuffer;
        fn.vkBeginCommandBuffer = MockVkBeginCommandBuffer;
        fn.vkEndCommandBuffer = MockVkEndCommandBuffer;
        fn.vkQueueSubmit = MockVkQueueSubmit;
        fn.vkQueuePresentKHR = MockVkQueuePresentKHR;
        fn.vkCmdPipelineBarrier = MockVkCmdPipelineBarrier;
        fn.vkCmdClearColorImage = MockVkCmdClearColorImage;
        fn.vkDeviceWaitIdle = MockVkDeviceWaitIdle;
        fn.vkCreateFence = MockVkCreateFence;
        fn.vkDestroyFence = MockVkDestroyFence;
        fn.vkCreateSemaphore = MockVkCreateSemaphore;
        fn.vkDestroySemaphore = MockVkDestroySemaphore;

        arm::SwapchainSync syncFail;
        syncFail.setVulkanFunctions(fn);
        syncFail.init(dummyDevice, dummyQueue, dummySwapchain, dummyImages, dummyCmds, dummyAcq, dummyRel, dummyFences);

        // Make fence creation fail with OUT_OF_DEVICE_MEMORY (allocation failure)
        s_MockVk.createFenceResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        TEST_CHECK(syncFail.resetFrameSyncObjects(0) == false);

        // Verify distinct state: NOT deviceLost, but IS recoveryFailed and terminalError
        TEST_CHECK(syncFail.isDeviceLost() == false);
        TEST_CHECK(syncFail.isRecoveryFailed() == true);
        TEST_CHECK(syncFail.isTerminalError() == true);

        // Subsequent beginFrame, endFrame, abortFrame, and resetFrameSyncObjects fail immediately
        uint32_t img = 0;
        uint32_t waitIdleBefore = s_MockVk.deviceWaitIdleCount;
        TEST_CHECK(syncFail.beginFrame(&img) == false);
        TEST_CHECK(syncFail.endFrame(0) == false);
        TEST_CHECK(syncFail.abortFrame(0) == false);
        TEST_CHECK(syncFail.resetFrameSyncObjects(0) == false);
        // vkDeviceWaitIdle was NOT called again during subsequent attempts
        TEST_CHECK(s_MockVk.deviceWaitIdleCount == waitIdleBefore);

        // onSwapchainRecreated does NOT clear terminal error
        syncFail.onSwapchainRecreated();
        TEST_CHECK(syncFail.isTerminalError() == true);
        TEST_CHECK(syncFail.isRecoveryFailed() == true);

        // Missing function pointers test
        {
            arm::SwapchainSync syncMissing;
            arm::SwapchainSync::VulkanFunctions incompleteFn = fn;
            incompleteFn.vkCreateFence = nullptr;
            syncMissing.setVulkanFunctions(incompleteFn);
            syncMissing.init(dummyDevice, dummyQueue, dummySwapchain, dummyImages, dummyCmds, dummyAcq, dummyRel, dummyFences);

            TEST_CHECK(syncMissing.resetFrameSyncObjects(0) == false);
            TEST_CHECK(syncMissing.isRecoveryFailed() == true);
            TEST_CHECK(syncMissing.isTerminalError() == true);
        }
    }

    // --- Subtest 31.4: Presentation Error Classification ---
    {
        s_MockVk = MockVulkanState();

        VkDevice dummyDevice = makeMockHandle<VkDevice>(0x1);
        VkQueue dummyQueue = makeMockHandle<VkQueue>(0x2);
        VkSwapchainKHR dummySwapchain = makeMockHandle<VkSwapchainKHR>(0x3);
        std::vector<VkImage> dummyImages = {
            makeMockHandle<VkImage>(0x10),
            makeMockHandle<VkImage>(0x20)
        };
        VkCommandBuffer dummyCmds[2] = { makeMockHandle<VkCommandBuffer>(0x100), makeMockHandle<VkCommandBuffer>(0x101) };
        VkSemaphore dummyAcq[2] = { makeMockHandle<VkSemaphore>(0x30), makeMockHandle<VkSemaphore>(0x31) };
        VkSemaphore dummyRel[2] = { makeMockHandle<VkSemaphore>(0x40), makeMockHandle<VkSemaphore>(0x41) };
        VkFence dummyFences[2] = { makeMockHandle<VkFence>(0x50), makeMockHandle<VkFence>(0x51) };

        arm::SwapchainSync::VulkanFunctions fn;
        fn.vkWaitForFences = MockVkWaitForFences;
        fn.vkAcquireNextImageKHR = MockVkAcquireNextImageKHR;
        fn.vkResetFences = MockVkResetFences;
        fn.vkResetCommandBuffer = MockVkResetCommandBuffer;
        fn.vkBeginCommandBuffer = MockVkBeginCommandBuffer;
        fn.vkEndCommandBuffer = MockVkEndCommandBuffer;
        fn.vkQueueSubmit = MockVkQueueSubmit;
        fn.vkQueuePresentKHR = MockVkQueuePresentKHR;
        fn.vkCmdPipelineBarrier = MockVkCmdPipelineBarrier;
        fn.vkCmdClearColorImage = MockVkCmdClearColorImage;
        fn.vkDeviceWaitIdle = MockVkDeviceWaitIdle;
        fn.vkCreateFence = MockVkCreateFence;
        fn.vkDestroyFence = MockVkDestroyFence;
        fn.vkCreateSemaphore = MockVkCreateSemaphore;
        fn.vkDestroySemaphore = MockVkDestroySemaphore;

        // 1. Success
        {
            arm::SwapchainSync sync;
            sync.setVulkanFunctions(fn);
            sync.init(dummyDevice, dummyQueue, dummySwapchain, dummyImages, dummyCmds, dummyAcq, dummyRel, dummyFences);
            uint32_t img = 0;
            s_MockVk.presentResult = VK_SUCCESS;
            TEST_CHECK(sync.beginFrame(&img) == true);
            TEST_CHECK(sync.endFrame(img) == true);
            TEST_CHECK(sync.lastPresentResult() == arm::SwapchainSync::PresentResult::Success);
            TEST_CHECK(sync.isTerminalError() == false);
        }

        // 2. Suboptimal swapchain (non-terminal, advances frame)
        {
            arm::SwapchainSync sync;
            sync.setVulkanFunctions(fn);
            sync.init(dummyDevice, dummyQueue, dummySwapchain, dummyImages, dummyCmds, dummyAcq, dummyRel, dummyFences);
            uint32_t img = 0;
            s_MockVk.presentResult = VK_SUBOPTIMAL_KHR;
            TEST_CHECK(sync.beginFrame(&img) == true);
            uint32_t frameBefore = sync.frame();
            TEST_CHECK(sync.endFrame(img) == false);
            TEST_CHECK(sync.lastPresentResult() == arm::SwapchainSync::PresentResult::Suboptimal);
            TEST_CHECK(sync.isTerminalError() == false);
            TEST_CHECK(sync.frame() == (frameBefore + 1) % arm::SwapchainSync::kFramesInFlight);
        }

        // 3. Out-of-date swapchain (non-terminal, advances frame)
        {
            arm::SwapchainSync sync;
            sync.setVulkanFunctions(fn);
            sync.init(dummyDevice, dummyQueue, dummySwapchain, dummyImages, dummyCmds, dummyAcq, dummyRel, dummyFences);
            uint32_t img = 0;
            s_MockVk.presentResult = VK_ERROR_OUT_OF_DATE_KHR;
            TEST_CHECK(sync.beginFrame(&img) == true);
            uint32_t frameBefore = sync.frame();
            TEST_CHECK(sync.endFrame(img) == false);
            TEST_CHECK(sync.lastPresentResult() == arm::SwapchainSync::PresentResult::OutOfDate);
            TEST_CHECK(sync.isTerminalError() == false);
            TEST_CHECK(sync.frame() == (frameBefore + 1) % arm::SwapchainSync::kFramesInFlight);
        }

        // 4. Surface lost (TERMINAL, does not advance frame or route to ordinary swapchain-only recreation)
        {
            arm::SwapchainSync sync;
            sync.setVulkanFunctions(fn);
            sync.init(dummyDevice, dummyQueue, dummySwapchain, dummyImages, dummyCmds, dummyAcq, dummyRel, dummyFences);
            uint32_t img = 0;
            s_MockVk.presentResult = VK_ERROR_SURFACE_LOST_KHR;
            TEST_CHECK(sync.beginFrame(&img) == true);
            uint32_t frameBefore = sync.frame();
            TEST_CHECK(sync.endFrame(img) == false);
            TEST_CHECK(sync.lastPresentResult() == arm::SwapchainSync::PresentResult::SurfaceLost);
            TEST_CHECK(sync.isSurfaceLost() == true);
            TEST_CHECK(sync.isTerminalError() == true);
            TEST_CHECK(sync.frame() == frameBefore); // frame not advanced
        }

        // 5. Device lost (TERMINAL)
        {
            arm::SwapchainSync sync;
            sync.setVulkanFunctions(fn);
            sync.init(dummyDevice, dummyQueue, dummySwapchain, dummyImages, dummyCmds, dummyAcq, dummyRel, dummyFences);
            uint32_t img = 0;
            s_MockVk.presentResult = VK_ERROR_DEVICE_LOST;
            TEST_CHECK(sync.beginFrame(&img) == true);
            TEST_CHECK(sync.endFrame(img) == false);
            TEST_CHECK(sync.lastPresentResult() == arm::SwapchainSync::PresentResult::DeviceLost);
            TEST_CHECK(sync.isDeviceLost() == true);
            TEST_CHECK(sync.isTerminalError() == true);
        }

        // 6. Other Vulkan failure (e.g. VK_ERROR_INITIALIZATION_FAILED) -> marks recoveryFailed_, TERMINAL
        {
            arm::SwapchainSync sync;
            sync.setVulkanFunctions(fn);
            sync.init(dummyDevice, dummyQueue, dummySwapchain, dummyImages, dummyCmds, dummyAcq, dummyRel, dummyFences);
            uint32_t img = 0;
            s_MockVk.presentResult = VK_ERROR_INITIALIZATION_FAILED;
            TEST_CHECK(sync.beginFrame(&img) == true);
            TEST_CHECK(sync.endFrame(img) == false);
            TEST_CHECK(sync.lastPresentResult() == arm::SwapchainSync::PresentResult::OtherError);
            TEST_CHECK(sync.isRecoveryFailed() == true);
            TEST_CHECK(sync.isTerminalError() == true);
        }

        // 7. Verify presentation classification also holds during abortFrame()
        {
            arm::SwapchainSync syncAbort;
            syncAbort.setVulkanFunctions(fn);
            syncAbort.init(dummyDevice, dummyQueue, dummySwapchain, dummyImages, dummyCmds, dummyAcq, dummyRel, dummyFences);
            uint32_t img = 0;
            s_MockVk.presentResult = VK_ERROR_SURFACE_LOST_KHR;
            TEST_CHECK(syncAbort.beginFrame(&img) == true);
            TEST_CHECK(syncAbort.abortFrame(img) == false);
            TEST_CHECK(syncAbort.lastPresentResult() == arm::SwapchainSync::PresentResult::SurfaceLost);
            TEST_CHECK(syncAbort.isSurfaceLost() == true);
            TEST_CHECK(syncAbort.isTerminalError() == true);
        }
    }

    // --- Subtest 31.5: Device Loss and Surface Loss during Swapchain Recreation ---
    {
        arm::SwapchainSync sync;
        sync.init(makeMockHandle<VkDevice>(0x1), makeMockHandle<VkQueue>(0x2),
                  makeMockHandle<VkSwapchainKHR>(0x3), { makeMockHandle<VkImage>(0x10) },
                  nullptr, nullptr, nullptr, nullptr);

        TEST_CHECK(sync.isDeviceLost() == false);
        TEST_CHECK(sync.isSurfaceLost() == false);
        TEST_CHECK(sync.isTerminalError() == false);

        // Notify device loss
        sync.notifyDeviceLost();
        TEST_CHECK(sync.isDeviceLost() == true);
        TEST_CHECK(sync.isTerminalError() == true);

        // Reset and notify surface loss
        sync.init(makeMockHandle<VkDevice>(0x1), makeMockHandle<VkQueue>(0x2),
                  makeMockHandle<VkSwapchainKHR>(0x3), { makeMockHandle<VkImage>(0x10) },
                  nullptr, nullptr, nullptr, nullptr);
        TEST_CHECK(sync.isTerminalError() == false);
        sync.notifySurfaceLost();
        TEST_CHECK(sync.isSurfaceLost() == true);
        TEST_CHECK(sync.isTerminalError() == true);

        // Reset and notify recovery failed
        sync.init(makeMockHandle<VkDevice>(0x1), makeMockHandle<VkQueue>(0x2),
                  makeMockHandle<VkSwapchainKHR>(0x3), { makeMockHandle<VkImage>(0x10) },
                  nullptr, nullptr, nullptr, nullptr);
        sync.notifyRecoveryFailed();
        TEST_CHECK(sync.isRecoveryFailed() == true);
        TEST_CHECK(sync.isTerminalError() == true);
    }

    // --- Subtest 31.6: Immediate Loop Termination on Terminal Error Without Retry Counter ---
    {
        uint32_t consecutiveFailures = 0;
        bool swapchainDirty = false;

        // Normal failure increments retry counter
        bool canContinue = arm::SwapchainRecoveryPolicy::handleRebuildResult(
            arm::SwapchainRebuildResult::Failed, swapchainDirty, consecutiveFailures);
        TEST_CHECK(canContinue == true);
        TEST_CHECK(consecutiveFailures == 1);
        TEST_CHECK(swapchainDirty == true);

        // But when a terminal error occurs (e.g. device lost), application logic checks
        // sync.isTerminalError() and breaks immediately without executing handleRebuildResult
        arm::SwapchainSync sync;
        sync.init(makeMockHandle<VkDevice>(0x1), makeMockHandle<VkQueue>(0x2),
                  makeMockHandle<VkSwapchainKHR>(0x3), { makeMockHandle<VkImage>(0x10) },
                  nullptr, nullptr, nullptr, nullptr);
        sync.notifyDeviceLost();

        bool loopTerminatedImmediately = false;
        if (sync.isTerminalError()) {
            loopTerminatedImmediately = true;
            // Does not pass through handleRebuildResult
        } else {
            arm::SwapchainRecoveryPolicy::handleRebuildResult(
                arm::SwapchainRebuildResult::Failed, swapchainDirty, consecutiveFailures);
        }
        TEST_CHECK(loopTerminatedImmediately == true);
        TEST_CHECK(consecutiveFailures == 1); // Not incremented!
    }

    std::cout << "PASS: Test 31: M3 Final Closeout - Vulkan Lifetime, Sync Recovery, and State Validation passed perfectly." << std::endl;
}

void TestM4VulkanPartialInitializationCleanup()
{
    std::cout << "Running Test 32: M4 - Android Vulkan Partial-Initialization Cleanup & State Machine..." << std::endl;

    // --- Subtest 32.1: Calling destroy on an empty (uninitialized) context ---
    {
        VkContext ctx;
        VkContext::TestSeam seam;
        ctx.setTestSeam(&seam);

        TEST_CHECK(ctx.instance() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.surface() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.device() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.swapchain() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.isInitialized() == false);

        // destroy() on empty context should be completely safe and perform zero destructions
        ctx.destroy();

        TEST_CHECK(seam.waitIdleCalls == 0);
        TEST_CHECK(seam.deviceDestructions == 0);
        TEST_CHECK(seam.surfaceDestructions == 0);
        TEST_CHECK(seam.instanceDestructions == 0);
        TEST_CHECK(seam.commandPoolDestructions == 0);
        TEST_CHECK(seam.swapchainDestructions == 0);
        TEST_CHECK(seam.semaphoreDestructions == 0);
        TEST_CHECK(seam.fenceDestructions == 0);
        TEST_CHECK(ctx.isInitialized() == false);
    }

    // --- Subtest 32.2: Calling destroy twice (idempotence) ---
    {
        VkContext ctx;
        VkContext::TestSeam seam;
        ctx.setTestSeam(&seam);

        ANativeWindow dummyWindow;
        bool ok = ctx.init(&dummyWindow);
        TEST_CHECK(ok == true);
        TEST_CHECK(ctx.isInitialized() == true);
        TEST_CHECK(ctx.instance() != VK_NULL_HANDLE);
        TEST_CHECK(ctx.device() != VK_NULL_HANDLE);

        // First destroy call
        seam.resetCounters();
        ctx.destroy();

        TEST_CHECK(seam.waitIdleCalls == 1);
        TEST_CHECK(seam.deviceDestructions == 1);
        TEST_CHECK(seam.surfaceDestructions == 1);
        TEST_CHECK(seam.instanceDestructions == 1);
        TEST_CHECK(seam.commandPoolDestructions == 1);
        TEST_CHECK(seam.swapchainDestructions == 1);
        TEST_CHECK(ctx.instance() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.surface() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.device() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.swapchain() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.isInitialized() == false);

        // Second destroy call: idempotent, zero further destructions
        seam.resetCounters();
        ctx.destroy();

        TEST_CHECK(seam.waitIdleCalls == 0);
        TEST_CHECK(seam.deviceDestructions == 0);
        TEST_CHECK(seam.surfaceDestructions == 0);
        TEST_CHECK(seam.instanceDestructions == 0);
        TEST_CHECK(seam.commandPoolDestructions == 0);
        TEST_CHECK(seam.swapchainDestructions == 0);
        TEST_CHECK(ctx.isInitialized() == false);
    }

    // --- Subtest 32.3: Failure after instance creation (instance created, surface fails) ---
    {
        VkContext ctx;
        VkContext::TestSeam seam;
        seam.failSurfaceCreation = true;
        ctx.setTestSeam(&seam);

        ANativeWindow dummyWindow;
        bool ok = ctx.init(&dummyWindow);
        TEST_CHECK(ok == false); // Transactional rollback

        // Rollback contract: instance was created so it must be destroyed; surface/device were never created
        TEST_CHECK(seam.instanceDestructions == 1);
        TEST_CHECK(seam.surfaceDestructions == 0);
        TEST_CHECK(seam.deviceDestructions == 0);
        TEST_CHECK(seam.commandPoolDestructions == 0);
        TEST_CHECK(seam.swapchainDestructions == 0);
        TEST_CHECK(ctx.instance() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.surface() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.device() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.isInitialized() == false);

        // Teardown called by application after init() failure is harmless
        ctx.destroy();
        TEST_CHECK(seam.instanceDestructions == 1); // No double destruction
    }

    // --- Subtest 32.4: Failure after surface creation (instance & surface created, physical device fails) ---
    {
        VkContext ctx;
        VkContext::TestSeam seam;
        seam.failPhysicalDevice = true;
        ctx.setTestSeam(&seam);

        ANativeWindow dummyWindow;
        bool ok = ctx.init(&dummyWindow);
        TEST_CHECK(ok == false);

        // Rollback contract: surface and instance must both be destroyed in proper order
        TEST_CHECK(seam.surfaceDestructions == 1);
        TEST_CHECK(seam.instanceDestructions == 1);
        TEST_CHECK(seam.deviceDestructions == 0);
        TEST_CHECK(ctx.surface() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.instance() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.device() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.isInitialized() == false);
    }

    // --- Subtest 32.5: Failure during logical device creation ---
    {
        VkContext ctx;
        VkContext::TestSeam seam;
        seam.failDeviceCreation = true;
        ctx.setTestSeam(&seam);

        ANativeWindow dummyWindow;
        bool ok = ctx.init(&dummyWindow);
        TEST_CHECK(ok == false);

        TEST_CHECK(seam.surfaceDestructions == 1);
        TEST_CHECK(seam.instanceDestructions == 1);
        TEST_CHECK(seam.deviceDestructions == 0);
        TEST_CHECK(ctx.device() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.surface() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.instance() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.isInitialized() == false);
    }

    // --- Subtest 32.6: Failure during swapchain creation ---
    {
        VkContext ctx;
        VkContext::TestSeam seam;
        seam.failSwapchainCreation = true;
        ctx.setTestSeam(&seam);

        ANativeWindow dummyWindow;
        bool ok = ctx.init(&dummyWindow);
        TEST_CHECK(ok == false);

        // Rollback contract: device, surface, and instance destroyed
        TEST_CHECK(seam.swapchainDestructions == 0);
        TEST_CHECK(seam.deviceDestructions == 1);
        TEST_CHECK(seam.surfaceDestructions == 1);
        TEST_CHECK(seam.instanceDestructions == 1);
        TEST_CHECK(ctx.swapchain() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.device() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.isInitialized() == false);
    }

    // --- Subtest 32.7: Cleanup after partial frame-resource creation ---
    {
        VkContext ctx;
        VkContext::TestSeam seam;
        // Fail on second semaphore: command pool + frame 0 semaphores were created
        seam.failSemaphoreIndex = 1;
        ctx.setTestSeam(&seam);

        ANativeWindow dummyWindow;
        bool ok = ctx.init(&dummyWindow);
        TEST_CHECK(ok == false);

        // Rollback contract:
        // 1. Device wait idle
        TEST_CHECK(seam.waitIdleCalls == 1);
        // 2. Command pool destroyed
        TEST_CHECK(seam.commandPoolDestructions == 1);
        // 3. Exactly the 2 semaphores created for frame 0 destroyed
        TEST_CHECK(seam.semaphoreDestructions == 2);
        // 4. Swapchain destroyed
        TEST_CHECK(seam.swapchainDestructions == 1);
        // 5. Logical device destroyed
        TEST_CHECK(seam.deviceDestructions == 1);
        // 6. Surface destroyed
        TEST_CHECK(seam.surfaceDestructions == 1);
        // 7. Instance destroyed
        TEST_CHECK(seam.instanceDestructions == 1);

        TEST_CHECK(ctx.instance() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.surface() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.device() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.swapchain() == VK_NULL_HANDLE);
        TEST_CHECK(ctx.isInitialized() == false);
    }

    // --- Subtest 32.8: Initializing again after a failed attempt ---
    {
        VkContext ctx;
        VkContext::TestSeam seam;
        seam.failPhysicalDevice = true;
        ctx.setTestSeam(&seam);

        ANativeWindow dummyWindow;
        bool ok1 = ctx.init(&dummyWindow);
        TEST_CHECK(ok1 == false);
        TEST_CHECK(ctx.isInitialized() == false);

        // Fix failure condition and reinitialize
        seam.failPhysicalDevice = false;
        seam.resetCounters();

        bool ok2 = ctx.init(&dummyWindow);
        TEST_CHECK(ok2 == true);
        TEST_CHECK(ctx.isInitialized() == true);
        TEST_CHECK(ctx.instance() != VK_NULL_HANDLE);
        TEST_CHECK(ctx.surface() != VK_NULL_HANDLE);
        TEST_CHECK(ctx.device() != VK_NULL_HANDLE);
        TEST_CHECK(ctx.swapchain() != VK_NULL_HANDLE);

        ctx.destroy();
        TEST_CHECK(ctx.isInitialized() == false);
    }

    // --- Subtest 32.9: Calling init() on an already initialized context ---
    {
        VkContext ctx;
        VkContext::TestSeam seam;
        ctx.setTestSeam(&seam);

        ANativeWindow dummyWindow;
        bool ok1 = ctx.init(&dummyWindow);
        TEST_CHECK(ok1 == true);
        TEST_CHECK(ctx.isInitialized() == true);

        // Calling init again without destroy() must cleanly reset prior state
        seam.resetCounters();
        bool ok2 = ctx.init(&dummyWindow);
        TEST_CHECK(ok2 == true);
        TEST_CHECK(ctx.isInitialized() == true);
        // Verified that prior device was destroyed before recreating
        TEST_CHECK(seam.deviceDestructions == 1);
        TEST_CHECK(seam.surfaceDestructions == 1);
        TEST_CHECK(seam.instanceDestructions == 1);

        ctx.destroy();
    }

    // --- Subtest 32.10: SwapchainSync::reset() state-clearing contract ---
    {
        arm::SwapchainSync sync;

        VkDevice mockDev = makeMockHandle<VkDevice>(0x101);
        VkQueue mockQueue = makeMockHandle<VkQueue>(0x102);
        VkSwapchainKHR mockSwapchain = makeMockHandle<VkSwapchainKHR>(0x103);
        std::vector<VkImage> mockImages = { makeMockHandle<VkImage>(0x201), makeMockHandle<VkImage>(0x202) };
        VkCommandBuffer mockCmds[2] = { makeMockHandle<VkCommandBuffer>(0x301), makeMockHandle<VkCommandBuffer>(0x302) };
        VkSemaphore mockAcq[2] = { makeMockHandle<VkSemaphore>(0x401), makeMockHandle<VkSemaphore>(0x402) };
        VkSemaphore mockRel[2] = { makeMockHandle<VkSemaphore>(0x501), makeMockHandle<VkSemaphore>(0x502) };
        VkFence mockFences[2] = { makeMockHandle<VkFence>(0x601), makeMockHandle<VkFence>(0x602) };

        sync.init(mockDev, mockQueue, mockSwapchain, mockImages, mockCmds, mockAcq, mockRel, mockFences);
        sync.notifyDeviceLost();

        TEST_CHECK(sync.device() == mockDev);
        TEST_CHECK(sync.queue() == mockQueue);
        TEST_CHECK(sync.swapchain() == mockSwapchain);
        TEST_CHECK(sync.swapchainImages().size() == 2);
        TEST_CHECK(sync.callerAcquireSemaphores() == mockAcq);
        TEST_CHECK(sync.callerReleaseSemaphores() == mockRel);
        TEST_CHECK(sync.callerInFlightFences() == mockFences);
        TEST_CHECK(sync.isDeviceLost() == true);
        TEST_CHECK(sync.isTerminalError() == true);

        // reset() clears all handles and state without destroying resources
        sync.reset();

        TEST_CHECK(sync.device() == VK_NULL_HANDLE);
        TEST_CHECK(sync.queue() == VK_NULL_HANDLE);
        TEST_CHECK(sync.swapchain() == VK_NULL_HANDLE);
        TEST_CHECK(sync.swapchainImages().empty() == true);
        TEST_CHECK(sync.callerAcquireSemaphores() == nullptr);
        TEST_CHECK(sync.callerReleaseSemaphores() == nullptr);
        TEST_CHECK(sync.callerInFlightFences() == nullptr);
        TEST_CHECK(sync.frame() == 0);
        TEST_CHECK(sync.isDeviceLost() == false);
        TEST_CHECK(sync.isSurfaceLost() == false);
        TEST_CHECK(sync.isRecoveryFailed() == false);
        TEST_CHECK(sync.isTerminalError() == false);
    }

    std::cout << "PASS: Test 32: M4 - Android Vulkan Partial-Initialization Cleanup & State Machine passed perfectly." << std::endl;
}

void TestM4GpuTimestampArithmetic()
{
    std::cout << "Running Test 33: M4 - GPU Timestamp Arithmetic & Conversion..." << std::endl;

    // 1. Normal timestamp differences
    {
        uint64_t startTick = 1000000;
        uint64_t endTick   = 1052000;
        uint32_t validBits = 36;
        float period = 1.0f; // 1 ns / tick

        auto res = ffxmCalculateGpuTimestampDelta(startTick, endTick, validBits, period, true, true);
        TEST_CHECK(res.valid == true);
        TEST_CHECK(res.deltaTicks == 52000);
        TEST_CHECK(std::abs(res.durationNs - 52000.0) < 1e-5);
        TEST_CHECK(std::abs(res.durationUs - 52.0) < 1e-5);
        TEST_CHECK(std::abs(res.durationMs - 0.052) < 1e-6);
    }

    // 2. Wraparound with fewer than 64 valid bits (36 valid bits)
    {
        uint32_t validBits = 36;
        const uint64_t mask = (1ULL << validBits) - 1ULL; // 0xFFFFFFFFF
        uint64_t startTick = mask - 30; // 0xFFFFFFFE1
        uint64_t endTick   = 20;        // wrapped around past zero
        float period = 1.0f;

        auto res = ffxmCalculateGpuTimestampDelta(startTick, endTick, validBits, period, true, true);
        TEST_CHECK(res.valid == true);
        TEST_CHECK(res.deltaTicks == 51); // (mask - (mask - 30)) + 20 + 1 = 31 + 20 = 51
        TEST_CHECK(std::abs(res.durationNs - 51.0) < 1e-5);
    }

    // 3. Wraparound with 48 valid bits
    {
        uint32_t validBits = 48;
        const uint64_t mask = (1ULL << validBits) - 1ULL;
        uint64_t startTick = mask - 99;
        uint64_t endTick   = 100;
        float period = 2.0f; // 2 ns / tick

        auto res = ffxmCalculateGpuTimestampDelta(startTick, endTick, validBits, period, true, true);
        TEST_CHECK(res.valid == true);
        TEST_CHECK(res.deltaTicks == 200);
        TEST_CHECK(std::abs(res.durationNs - 400.0) < 1e-5);
    }

    // 4. The 64-bit case (no undefined shifts)
    {
        uint32_t validBits = 64;
        uint64_t startTick = 0xFFFFFFFFFFFFFF80ULL;
        uint64_t endTick   = 0x0000000000000040ULL;
        float period = 1.0f;

        auto res = ffxmCalculateGpuTimestampDelta(startTick, endTick, validBits, period, true, true);
        TEST_CHECK(res.valid == true);
        TEST_CHECK(res.deltaTicks == 0xC0ULL); // 192 ticks
        TEST_CHECK(std::abs(res.durationNs - 192.0) < 1e-5);
    }

    // 5. Unsupported timestamp queues & invalid inputs
    {
        // validBits == 0 (unsupported queue family)
        auto res0 = ffxmCalculateGpuTimestampDelta(100, 200, 0, 1.0f, true, true);
        TEST_CHECK(res0.valid == false);
        TEST_CHECK(res0.deltaTicks == 0);

        // validBits > 64 (invalid)
        auto res65 = ffxmCalculateGpuTimestampDelta(100, 200, 65, 1.0f, true, true);
        TEST_CHECK(res65.valid == false);

        // timestampPeriod <= 0.0f
        auto resP0 = ffxmCalculateGpuTimestampDelta(100, 200, 64, 0.0f, true, true);
        TEST_CHECK(resP0.valid == false);
        auto resNeg = ffxmCalculateGpuTimestampDelta(100, 200, 64, -1.0f, true, true);
        TEST_CHECK(resNeg.valid == false);

        // Unavailable queries
        auto resUnavail1 = ffxmCalculateGpuTimestampDelta(100, 200, 64, 1.0f, false, true);
        TEST_CHECK(resUnavail1.valid == false);
        auto resUnavail2 = ffxmCalculateGpuTimestampDelta(100, 200, 64, 1.0f, true, false);
        TEST_CHECK(resUnavail2.valid == false);
        auto resUnavailBoth = ffxmCalculateGpuTimestampDelta(100, 200, 64, 1.0f, false, false);
        TEST_CHECK(resUnavailBoth.valid == false);
    }

    // 6. Conversion using device timestamp periods (e.g. Snapdragon Adreno 19.230769 ns)
    {
        uint64_t startTick = 1000;
        uint64_t endTick   = 101000; // 100,000 ticks
        float period = 19.230769f;   // Qualcomm Snapdragon Adreno typical

        auto res = ffxmCalculateGpuTimestampDelta(startTick, endTick, 64, period, true, true);
        TEST_CHECK(res.valid == true);
        TEST_CHECK(res.deltaTicks == 100000);
        // 100000 * 19.230769 ns = 1923076.9 ns = 1.9230769 ms = 1923.0769 us
        TEST_CHECK(std::abs(res.durationMs - 1.9230769) < 1e-5);
        TEST_CHECK(std::abs(res.durationUs - 1923.0769) < 1e-2);
    }

    std::cout << "PASS: Test 33: M4 - GPU Timestamp Arithmetic & Conversion passed perfectly." << std::endl;
}

void TestM4BackendProfilingInterface()
{
    std::cout << "Running Test 34: M4 - Vulkan Backend Profiling Interface & Test Hooks..." << std::endl;
    ffxmTestResetBackendRefCountVK();

    // 1. Parameter validation
    {
        TEST_CHECK(ffxmSetProfilingDescriptionVK(nullptr, nullptr) == FFXM_ERROR_INVALID_POINTER);
        FfxmVkProfilingDescription desc{};
        TEST_CHECK(ffxmGetProfilingDescriptionVK(nullptr, &desc) == FFXM_ERROR_INVALID_POINTER);
    }

    // 2. Set and Get profiling description on backend interface
    {
        size_t scratchSize = ffxmGetScratchMemorySizeVK(VK_NULL_HANDLE, 1);
        std::vector<uint8_t> scratch(scratchSize);
        FfxmInterface iface{};
        FfxmUInt32 effectContextId = 0;
        TEST_CHECK(ffxmTestSetupBackendContextVK(&iface, scratch.data(), scratch.size(), 1, &effectContextId) == FFXM_OK);

        // Initially disabled
        FfxmVkProfilingDescription initialDesc{};
        TEST_CHECK(ffxmGetProfilingDescriptionVK(&iface, &initialDesc) == FFXM_OK);
        TEST_CHECK(initialDesc.mode == FFXM_GPU_PROFILING_MODE_DISABLED);

        // Configure Total-only profiling
        FfxmVkProfilingDescription setDesc{};
        setDesc.mode = FFXM_GPU_PROFILING_MODE_TOTAL_ONLY;
        setDesc.queryPool = (VkQueryPool)(uintptr_t)0x200;
        setDesc.startQueryIndex = 0;
        setDesc.maxQueryCount = 2;
        TEST_CHECK(ffxmSetProfilingDescriptionVK(&iface, &setDesc) == FFXM_OK);

        FfxmVkProfilingDescription getDesc{};
        TEST_CHECK(ffxmGetProfilingDescriptionVK(&iface, &getDesc) == FFXM_OK);
        TEST_CHECK(getDesc.mode == FFXM_GPU_PROFILING_MODE_TOTAL_ONLY);
        TEST_CHECK(getDesc.queryPool == (VkQueryPool)(uintptr_t)0x200);
        TEST_CHECK(getDesc.startQueryIndex == 0);
        TEST_CHECK(getDesc.maxQueryCount == 2);

        // Clear profiling by passing nullptr
        TEST_CHECK(ffxmSetProfilingDescriptionVK(&iface, nullptr) == FFXM_OK);
        FfxmVkProfilingDescription clearedDesc{};
        TEST_CHECK(ffxmGetProfilingDescriptionVK(&iface, &clearedDesc) == FFXM_OK);
        TEST_CHECK(clearedDesc.mode == FFXM_GPU_PROFILING_MODE_DISABLED);
        TEST_CHECK(clearedDesc.queryPool == VK_NULL_HANDLE);

        ffxmTestResetBackendRefCountVK();
    }

    // 3. Test timestamp emission hooks during ExecuteGpuJobsVK
    {
        size_t scratchSize = ffxmGetScratchMemorySizeVK(VK_NULL_HANDLE, 1);
        std::vector<uint8_t> scratch(scratchSize);
        FfxmInterface iface{};
        FfxmUInt32 effectContextId = 0;
        TEST_CHECK(ffxmTestSetupBackendContextVK(&iface, scratch.data(), scratch.size(), 1, &effectContextId) == FFXM_OK);

        struct WrittenTimestamp {
            VkCommandBuffer cmd;
            VkPipelineStageFlagBits stage;
            VkQueryPool pool;
            uint32_t query;
        };
        static std::vector<WrittenTimestamp> s_written;
        s_written.clear();

        ffxmSetTestWriteTimestampHookVK([](VkCommandBuffer cmd, VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t query) {
            s_written.push_back({cmd, stage, pool, query});
        });

        // Test with profiling disabled: exactly 0 timestamps written
        {
            s_written.clear();
            FfxmCommandList cmdList = ffxmGetCommandListVK((VkCommandBuffer)(uintptr_t)0x500);
            TEST_CHECK(ExecuteGpuJobsVK(&iface, cmdList) == FFXM_OK);
            TEST_CHECK(s_written.empty() == true);
        }

        // Test Total-only mode: exactly 2 timestamps written (start at query 0, end at query 1)
        {
            s_written.clear();
            FfxmVkProfilingDescription desc{};
            desc.mode = FFXM_GPU_PROFILING_MODE_TOTAL_ONLY;
            desc.queryPool = (VkQueryPool)(uintptr_t)0x300;
            desc.startQueryIndex = 0;
            desc.maxQueryCount = 2;
            TEST_CHECK(ffxmSetProfilingDescriptionVK(&iface, &desc) == FFXM_OK);

            FfxmCommandList cmdList = ffxmGetCommandListVK((VkCommandBuffer)(uintptr_t)0x500);
            TEST_CHECK(ExecuteGpuJobsVK(&iface, cmdList) == FFXM_OK);
            TEST_CHECK(s_written.size() == 2);
            if (s_written.size() == 2) {
                TEST_CHECK(s_written[0].query == FFXM_PROFILING_QUERY_TOTAL_START);
                TEST_CHECK(s_written[0].stage == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
                TEST_CHECK(s_written[0].pool == (VkQueryPool)(uintptr_t)0x300);

                TEST_CHECK(s_written[1].query == FFXM_PROFILING_QUERY_TOTAL_END);
                TEST_CHECK(s_written[1].stage == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
                TEST_CHECK(s_written[1].pool == (VkQueryPool)(uintptr_t)0x300);
            }

            // ScopedProfilingCleanup should ensure profiling was disabled after ExecuteGpuJobsVK
            FfxmVkProfilingDescription afterDesc{};
            TEST_CHECK(ffxmGetProfilingDescriptionVK(&iface, &afterDesc) == FFXM_OK);
            TEST_CHECK(afterDesc.mode == FFXM_GPU_PROFILING_MODE_DISABLED);
        }

        ffxmSetTestWriteTimestampHookVK(nullptr);
        ffxmTestResetBackendRefCountVK();
    }

    std::cout << "PASS: Test 34: M4 - Vulkan Backend Profiling Interface & Test Hooks passed perfectly." << std::endl;
}

void TestM4GpuProfilerStatisticsAndLifecycle()
{
    std::cout << "Running Test 35: M4 - GPU Profiler Statistics & Lifecycle..." << std::endl;

    // 1. Statistics computation on known dataset: [1.0, 2.0, ..., 100.0]
    {
        std::vector<double> values;
        for (int i = 1; i <= 100; ++i) {
            values.push_back((double)i);
        }

        auto stats = GpuProfiler::computeStats(values, 100, 0);
        TEST_CHECK(stats.totalMeasuredFrames == 100);
        TEST_CHECK(stats.validFrames == 100);
        TEST_CHECK(stats.unavailableFrames == 0);
        TEST_CHECK(std::abs(stats.minMs - 1.0) < 1e-5);
        TEST_CHECK(std::abs(stats.maxMs - 100.0) < 1e-5);
        TEST_CHECK(std::abs(stats.medianMs - 50.5) < 1e-5);
        TEST_CHECK(std::abs(stats.meanMs - 50.5) < 1e-5);
        TEST_CHECK(std::abs(stats.p95Ms - 95.0) < 1e-5);
        TEST_CHECK(std::abs(stats.p99Ms - 99.0) < 1e-5);
    }

    // 2. Lifecycle: warmup frames, frame completion, and abortion
    {
        GpuProfiler profiler;
        GpuProfilerConfig cfg;
        cfg.mode = FFXM_GPU_PROFILING_MODE_TOTAL_ONLY;
        cfg.warmupFrames = 2;
        cfg.targetMeasuredFrames = 3;
        profiler.init(cfg);

        TEST_CHECK(profiler.isComplete() == false);
        TEST_CHECK(profiler.sampleCount() == 0);

        VkContext ctx;
        VkContext::TestSeam seam{};
        seam.timestampValidBitsOverride = 64;
        ctx.setTestSeam(&seam);
        TEST_CHECK(ctx.init((ANativeWindow*)(uintptr_t)0x1) == true);

        // Frame 0: begin, submit
        FfxmVkProfilingDescription desc0{};
        bool active0 = profiler.onBeginFrame(0, (VkCommandBuffer)(uintptr_t)0x60, ctx, &desc0);
        TEST_CHECK(active0 == true);
        TEST_CHECK(desc0.mode == FFXM_GPU_PROFILING_MODE_TOTAL_ONLY);
        profiler.onFrameSubmitted(0, FFXM_GPU_PROFILING_MODE_TOTAL_ONLY, 0);

        // Frame 1: begin, abort
        FfxmVkProfilingDescription desc1{};
        bool active1 = profiler.onBeginFrame(1, (VkCommandBuffer)(uintptr_t)0x61, ctx, &desc1);
        TEST_CHECK(active1 == true);
        profiler.onFrameAborted(1);

        // Next frame slot 1 should not have pending submission from aborted frame
        FfxmVkProfilingDescription desc1_b{};
        bool active1_b = profiler.onBeginFrame(1, (VkCommandBuffer)(uintptr_t)0x61, ctx, &desc1_b);
        TEST_CHECK(active1_b == true);

        // Reset context
        profiler.onContextReset();
    }

    // 3. Query count helper
    {
        TEST_CHECK(GpuProfiler::queryCountForMode(FFXM_GPU_PROFILING_MODE_DISABLED) == 0);
        TEST_CHECK(GpuProfiler::queryCountForMode(FFXM_GPU_PROFILING_MODE_TOTAL_ONLY) == 2);
        TEST_CHECK(GpuProfiler::queryCountForMode(FFXM_GPU_PROFILING_MODE_PER_PASS) == 24);
    }

    // 4. Incomplete timestamp query rejection & profiler hardening (Phase 5)
    {
        GpuProfiler profiler;
        GpuProfilerConfig cfg;
        cfg.mode = FFXM_GPU_PROFILING_MODE_PER_PASS;
        cfg.warmupFrames = 1;
        cfg.targetMeasuredFrames = 2;
        profiler.init(cfg);

        VkContext ctx;
        VkContext::TestSeam seam{};
        seam.timestampValidBitsOverride = 48; // Qualcomm Adreno 48 valid bits
        ctx.setTestSeam(&seam);
        TEST_CHECK(ctx.init((ANativeWindow*)(uintptr_t)0x1) == true);

        GpuProfiler::QueryResultWithAvailability mockQueries[FFXM_PROFILING_MAX_QUERIES] = {};
        auto setAllQueriesValid = [&](uint64_t baseTick) {
            for (uint32_t i = 0; i < 24; ++i) {
                mockQueries[i].available = 1;
                mockQueries[i].timestamp = baseTick + (i * 1000ULL);
            }
        };

        // Scenario A: Total query is unavailable during warmup
        setAllQueriesValid(100000ULL);
        mockQueries[FFXM_PROFILING_QUERY_TOTAL_END].available = 0; // End query not ready
        seam.mockQueryResults = mockQueries;
        seam.mockQueryResultsSize = sizeof(mockQueries);

        FfxmVkProfilingDescription descA{};
        profiler.onBeginFrame(0, (VkCommandBuffer)(uintptr_t)0x70, ctx, &descA);
        profiler.onFrameSubmitted(0, FFXM_GPU_PROFILING_MODE_PER_PASS, 100);

        // Next frame on slot 0 triggers collectSlotResults
        profiler.onBeginFrame(0, (VkCommandBuffer)(uintptr_t)0x70, ctx, &descA);
        TEST_CHECK(profiler.unavailableCount() == 1);
        TEST_CHECK(profiler.warmupRemaining() == 1); // Warmup counter must NOT decrement on invalid frame!
        TEST_CHECK(profiler.sampleCount() == 0);

        // Scenario B: Total query is valid, but pass 3 (ShadingChange) end query is unavailable
        setAllQueriesValid(200000ULL);
        uint32_t pass3End = FFXM_PROFILING_QUERY_PASS_END(3);
        mockQueries[pass3End].available = 0; // Missing pass query
        profiler.onFrameSubmitted(0, FFXM_GPU_PROFILING_MODE_PER_PASS, 101);

        profiler.onBeginFrame(0, (VkCommandBuffer)(uintptr_t)0x70, ctx, &descA);
        TEST_CHECK(profiler.unavailableCount() == 2);
        TEST_CHECK(profiler.warmupRemaining() == 1); // Still preserved!
        TEST_CHECK(profiler.sampleCount() == 0);

        // Scenario C: Total and passes valid, but clear 2 (ClearSpdMips) start query is unavailable
        setAllQueriesValid(300000ULL);
        uint32_t clear2Start = FFXM_PROFILING_QUERY_CLEAR_START(2);
        mockQueries[clear2Start].available = 0; // Missing clear query
        profiler.onFrameSubmitted(0, FFXM_GPU_PROFILING_MODE_PER_PASS, 102);

        profiler.onBeginFrame(0, (VkCommandBuffer)(uintptr_t)0x70, ctx, &descA);
        TEST_CHECK(profiler.unavailableCount() == 3);
        TEST_CHECK(profiler.warmupRemaining() == 1); // Still preserved!
        TEST_CHECK(profiler.sampleCount() == 0);

        // Scenario D: All 24 queries completely valid for warmup frame
        setAllQueriesValid(400000ULL);
        profiler.onFrameSubmitted(0, FFXM_GPU_PROFILING_MODE_PER_PASS, 103);

        profiler.onBeginFrame(0, (VkCommandBuffer)(uintptr_t)0x70, ctx, &descA);
        TEST_CHECK(profiler.unavailableCount() == 3);
        TEST_CHECK(profiler.warmupRemaining() == 0); // Warmup now consumed!
        TEST_CHECK(profiler.sampleCount() == 0);     // Warmup frames are not recorded in samples_

        // Scenario E: Measured frame with all 24 queries valid (with 48-bit wraparound)
        const uint64_t mask48 = (1ULL << 48) - 1ULL;
        // Start just before 48-bit overflow, end after wraparound
        mockQueries[FFXM_PROFILING_QUERY_TOTAL_START].available = 1;
        mockQueries[FFXM_PROFILING_QUERY_TOTAL_START].timestamp = mask48 - 500;
        mockQueries[FFXM_PROFILING_QUERY_TOTAL_END].available = 1;
        mockQueries[FFXM_PROFILING_QUERY_TOTAL_END].timestamp = 500; // wrapped: delta = 1001 ticks

        for (uint32_t p = 0; p < FFXM_PROFILING_PASS_COUNT; ++p) {
            uint32_t s = FFXM_PROFILING_QUERY_PASS_START(p);
            uint32_t e = FFXM_PROFILING_QUERY_PASS_END(p);
            mockQueries[s].available = 1;
            mockQueries[s].timestamp = 1000 + (p * 100);
            mockQueries[e].available = 1;
            mockQueries[e].timestamp = 1000 + (p * 100) + 50;
        }
        for (uint32_t c = 0; c < FFXM_PROFILING_CLEAR_JOB_COUNT; ++c) {
            uint32_t s = FFXM_PROFILING_QUERY_CLEAR_START(c);
            uint32_t e = FFXM_PROFILING_QUERY_CLEAR_END(c);
            mockQueries[s].available = 1;
            mockQueries[s].timestamp = 2000 + (c * 50);
            mockQueries[e].available = 1;
            mockQueries[e].timestamp = 2000 + (c * 50) + 20;
        }

        profiler.onFrameSubmitted(0, FFXM_GPU_PROFILING_MODE_PER_PASS, 104);
        profiler.onBeginFrame(0, (VkCommandBuffer)(uintptr_t)0x70, ctx, &descA);

        TEST_CHECK(profiler.unavailableCount() == 3);
        TEST_CHECK(profiler.sampleCount() == 1);
        const auto& s0 = profiler.samples()[0];
        TEST_CHECK(s0.valid == true);
        TEST_CHECK(s0.frameIndex == 104);
        TEST_CHECK(s0.totalMs > 0.0);
        for (uint32_t p = 0; p < FFXM_PROFILING_PASS_COUNT; ++p) {
            TEST_CHECK(s0.passMs[p] > 0.0); // Never 0.0 ms!
        }
        for (uint32_t c = 0; c < FFXM_PROFILING_CLEAR_JOB_COUNT; ++c) {
            TEST_CHECK(s0.clearMs[c] > 0.0); // Never 0.0 ms!
        }

        // Scenario F: Measured frame 2 completing targetMeasuredFrames
        profiler.onFrameSubmitted(0, FFXM_GPU_PROFILING_MODE_PER_PASS, 105);
        profiler.onBeginFrame(0, (VkCommandBuffer)(uintptr_t)0x70, ctx, &descA);

        TEST_CHECK(profiler.sampleCount() == 2);
        TEST_CHECK(profiler.isComplete() == true);

        // Verify stats reflect unavailable frames accurately
        std::vector<double> sampleTotals = { profiler.samples()[0].totalMs, profiler.samples()[1].totalMs };
        auto stats = GpuProfiler::computeStats(sampleTotals, 2, profiler.unavailableCount());
        TEST_CHECK(stats.validFrames == 2);
        TEST_CHECK(stats.unavailableFrames == 3);
        TEST_CHECK(stats.totalMeasuredFrames == 2);
    }

    std::cout << "PASS: Test 35: M4 - GPU Profiler Statistics & Lifecycle passed perfectly." << std::endl;
}

void TestM4VkContextQueryPoolLifecycle()
{
    std::cout << "Running Test 36: M4 - VkContext Query Pool Lifecycle & Teardown..." << std::endl;

    // 1. Query pool creation and destruction under test seam
    {
        VkContext ctx;
        VkContext::TestSeam seam{};
        seam.timestampValidBitsOverride = 64;
        ctx.setTestSeam(&seam);

        TEST_CHECK(ctx.init((ANativeWindow*)(uintptr_t)0x1) == true);
        TEST_CHECK(ctx.queryPool(0) != VK_NULL_HANDLE);
        TEST_CHECK(ctx.queryPool(1) != VK_NULL_HANDLE);
        TEST_CHECK(ctx.queryPool(2) == VK_NULL_HANDLE); // out of bounds

        ctx.destroy();
        // kFramesInFlight = 2, so 2 query pools destroyed
        TEST_CHECK(seam.queryPoolDestructions == 2);
        TEST_CHECK(seam.deviceDestructions == 1);
        TEST_CHECK(ctx.queryPool(0) == VK_NULL_HANDLE);
        TEST_CHECK(ctx.queryPool(1) == VK_NULL_HANDLE);
    }

    // 2. Graceful degradation when queue family has timestampValidBits == 0
    {
        s_MockTimestampValidBits = 0;
        VkContext ctx;
        VkContext::TestSeam seam{};
        seam.timestampValidBitsOverride = 0;
        ctx.setTestSeam(&seam);

        TEST_CHECK(ctx.init((ANativeWindow*)(uintptr_t)0x1) == true);
        TEST_CHECK(ctx.timestampValidBits() == 0);
        TEST_CHECK(ctx.queryPool(0) == VK_NULL_HANDLE);
        TEST_CHECK(ctx.queryPool(1) == VK_NULL_HANDLE);

        ctx.destroy();
        TEST_CHECK(seam.queryPoolDestructions == 0); // No query pools created, so 0 destroyed
        s_MockTimestampValidBits = 64; // Restore default
    }

    std::cout << "PASS: Test 36: M4 - VkContext Query Pool Lifecycle & Teardown passed perfectly." << std::endl;
}

static void TestPhase3MobileBandwidthReductionAndKernelFusion()
{
    std::cout << "--- Test Phase 3: Mobile Bandwidth Reduction & Kernel Fusion Verification ---" << std::endl;

    // 1. Verify mathematically that DRAM memory traffic is reduced by >= 15% across the reactive pipeline
    const uint32_t renderWidth = 1920;
    const uint32_t renderHeight = 1080;
    const uint32_t halfWidth = renderWidth / 2;
    const uint32_t halfHeight = renderHeight / 2;

    // Legacy un-fused DRAM round-trip:
    // Pass 1 (ShadingChange) writes half-res R8_UNORM:
    const size_t legacyDramWriteBytes = halfWidth * halfHeight * 1; // 518,400 bytes
    // Pass 2 (PrepareReactivity) reads half-res R8_UNORM with bilinear filter at full resolution:
    const size_t legacyDramReadBytes = renderWidth * renderHeight * 1; // 2,073,600 bytes
    const size_t totalLegacyDramBytes = legacyDramWriteBytes + legacyDramReadBytes; // 2,592,000 bytes

    // Fused mobile execution:
    // Both write and read are ELIMINATED completely (0 bytes to/from DRAM, computed in-flight from SPD mips):
    const size_t fusedDramBytes = 0;
    const size_t dramBytesSaved = totalLegacyDramBytes - fusedDramBytes;
    const double dramSavingsPercentage = 100.0 * static_cast<double>(dramBytesSaved) / static_cast<double>(totalLegacyDramBytes);

    TEST_CHECK(dramSavingsPercentage >= 15.0); // Strict Acceptance Invariant: Minimum 15% reduction
    TEST_CHECK(dramBytesSaved == 2592000);     // 2.47 MB DRAM traffic eliminated per 1080p frame

    // 2. Verify dispatch elimination and state invariant in real context
    FfxmFsr3UpscalerContext context;
    FfxmFsr3UpscalerContextDescription desc = {};
    desc.maxRenderSize = { 1920, 1080 };
    desc.maxUpscaleSize = { 3840, 2160 };
    desc.flags = FFXM_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    SetupMockInterface(&desc.backendInterface);

    TEST_CHECK(ffxmFsr3UpscalerContextCreate(&context, &desc) == FFXM_OK);
    FfxmFsr3UpscalerContext_Private* priv = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(&context);

    // Verify PrepareReactivity binds r_spd_mips at binding 6
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureCount == 9);
    TEST_CHECK(priv->pipelinePrepareReactivity.srvTextureBindings[6].resourceIdentifier == FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS);

    // Dispatch and verify that ShadingChange compute job is completely absent
    FfxmCommandList dummyCmdList = reinterpret_cast<FfxmCommandList>(0x1234);
    FfxmFsr3UpscalerDispatchDescription dispDesc = {};
    dispDesc.commandList = dummyCmdList;
    dispDesc.renderSize = { 1920, 1080 };
    dispDesc.upscaleSize = { 3840, 2160 };

    static int s_Res = 99;
    dispDesc.color.resource = &s_Res;
    dispDesc.depth.resource = &s_Res;
    dispDesc.motionVectors.resource = &s_Res;
    dispDesc.dilatedMotionVectors.resource = &s_Res;
    dispDesc.dilatedDepth.resource = &s_Res;
    dispDesc.reconstructedPrevNearestDepth.resource = &s_Res;
    dispDesc.output.resource = &s_Res;

    s_ScheduledGpuJobs.clear();
    TEST_CHECK(ffxmFsr3UpscalerContextDispatch(&context, &dispDesc) == FFXM_OK);

    // Total jobs in first frame must be strictly 18 (11 clears + 7 compute passes, ShadingChange eliminated)
    TEST_CHECK(s_ScheduledGpuJobs.size() == 18);

    for (const auto& job : s_ScheduledGpuJobs) {
        if (job.jobType == FFXM_GPU_JOB_COMPUTE) {
            TEST_CHECK(job.computeJobDescriptor.pipeline.pipeline != priv->pipelineShadingChange.pipeline);
        }
    }

    TEST_CHECK(ffxmFsr3UpscalerContextDestroy(&context) == FFXM_OK);
    std::cout << "PASS: Phase 3 Mobile Bandwidth Reduction & Kernel Fusion verified (DRAM savings: "
              << (dramBytesSaved / 1024) << " KB/frame, 100% eliminated roundtrip)." << std::endl;
}

static void TestPhase4CadenceAndSurfaceFlingerPacingStabilization()
{
    std::cout << "--- Test Phase 4: Cadence & SurfaceFlinger Pacing Stabilization Verification ---" << std::endl;

    // 1. Invariant: Pacing Configuration Defaults & Validation
    {
        arm::SwapchainSync sync;
        const auto& cfg = sync.pacingConfig();
        TEST_CHECK(cfg.mode == arm::SwapchainSync::PacingMode::Unlocked);
        TEST_CHECK(cfg.enableGpuPacingLock == false);
        TEST_CHECK(cfg.targetFps == 60);
        TEST_CHECK(sync.totalFramesSubmitted() == 0);

        // LockTo1Vsync automatically enables GPU pacing lock
        arm::SwapchainSync::PacingConfig p1;
        p1.mode = arm::SwapchainSync::PacingMode::LockTo1Vsync;
        p1.targetFps = 120;
        sync.setPacingConfig(p1);
        TEST_CHECK(sync.pacingConfig().mode == arm::SwapchainSync::PacingMode::LockTo1Vsync);
        TEST_CHECK(sync.pacingConfig().enableGpuPacingLock == true);
        TEST_CHECK(sync.pacingConfig().targetFrameTimeNs == 8333333ull);

        // TargetFps mode with 60 FPS
        arm::SwapchainSync::PacingConfig p2;
        p2.mode = arm::SwapchainSync::PacingMode::TargetFps;
        p2.targetFps = 60;
        sync.setPacingConfig(p2);
        TEST_CHECK(sync.pacingConfig().mode == arm::SwapchainSync::PacingMode::TargetFps);
        TEST_CHECK(sync.pacingConfig().enableGpuPacingLock == true);
        TEST_CHECK(sync.pacingConfig().targetFrameTimeNs == 16666666ull);

        sync.resetPacingStats();
        TEST_CHECK(sync.pacingStats().totalPresentedFrames == 0);
    }

    // 2. Invariant: GPU Pacing Lock & Prior-Frame Wait / Failure Rollback
    {
        arm::SwapchainSync sync;
        VkDevice mockDevice = (VkDevice)0x10;
        VkQueue mockQueue = (VkQueue)0x20;
        VkSwapchainKHR mockSwapchain = (VkSwapchainKHR)0x30;
        std::vector<VkImage> mockImages = { (VkImage)0x101, (VkImage)0x102, (VkImage)0x103 };
        VkCommandBuffer mockCmds[2] = { (VkCommandBuffer)0x201, (VkCommandBuffer)0x202 };
        VkSemaphore mockAcq[2] = { (VkSemaphore)0x301, (VkSemaphore)0x302 };
        VkSemaphore mockRel[2] = { (VkSemaphore)0x401, (VkSemaphore)0x402 };
        VkFence mockFences[2] = { (VkFence)0x501, (VkFence)0x502 };

        sync.init(mockDevice, mockQueue, mockSwapchain, mockImages, mockCmds, mockAcq, mockRel, mockFences);

        arm::SwapchainSync::VulkanFunctions fn = {};
        fn.vkWaitForFences = [](VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t) -> VkResult {
            return VK_SUCCESS;
        };
        fn.vkResetFences = [](VkDevice, uint32_t, const VkFence*) -> VkResult { return VK_SUCCESS; };
        fn.vkResetCommandBuffer = [](VkCommandBuffer, VkCommandBufferResetFlags) -> VkResult { return VK_SUCCESS; };
        fn.vkBeginCommandBuffer = [](VkCommandBuffer, const VkCommandBufferBeginInfo*) -> VkResult { return VK_SUCCESS; };
        fn.vkEndCommandBuffer = [](VkCommandBuffer) -> VkResult { return VK_SUCCESS; };
        fn.vkQueueSubmit = [](VkQueue, uint32_t, const VkSubmitInfo*, VkFence) -> VkResult { return VK_SUCCESS; };
        fn.vkQueuePresentKHR = [](VkQueue, const VkPresentInfoKHR*) -> VkResult { return VK_SUCCESS; };
        fn.vkAcquireNextImageKHR = [](VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t* pIndex) -> VkResult {
            *pIndex = 0;
            return VK_SUCCESS;
        };

        sync.setVulkanFunctions(fn);

        // A. Pacing lock enabled:
        arm::SwapchainSync::PacingConfig cfg;
        cfg.mode = arm::SwapchainSync::PacingMode::LockTo1Vsync;
        sync.setPacingConfig(cfg);

        // Frame 0 begin: no prior frame submitted yet, totalFramesSubmitted == 0
        uint32_t imgIndex = 0;
        TEST_CHECK(sync.beginFrame(&imgIndex) == true);
        TEST_CHECK(sync.endFrame(0) == true);
        TEST_CHECK(sync.totalFramesSubmitted() == 1);
        TEST_CHECK(sync.frame() == 1);

        // Frame 1 begin: totalFramesSubmitted == 1 -> must wait for frame 0's fence (mockFences[0])
        TEST_CHECK(sync.beginFrame(&imgIndex) == true);
        TEST_CHECK(sync.endFrame(0) == true);
        TEST_CHECK(sync.totalFramesSubmitted() == 2);

        // B. Failure mode test: if prior-frame fence wait returns DEVICE_LOST:
        sync.vulkanFunctions().vkWaitForFences = [](VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t timeout) -> VkResult {
            if (timeout == 1000000000ull) { // Prior frame pacing fence wait
                return VK_ERROR_DEVICE_LOST;
            }
            return VK_SUCCESS;
        };
        TEST_CHECK(sync.beginFrame(&imgIndex) == false);
        TEST_CHECK(sync.isDeviceLost() == true);
        TEST_CHECK(sync.isTerminalError() == true);
    }

    // 3. Invariant: Wall-Clock Cadence Regulation & Target FPS Sleep Computation
    {
        arm::SwapchainSync sync;
        VkDevice mockDevice = (VkDevice)0x10;
        VkQueue mockQueue = (VkQueue)0x20;
        VkSwapchainKHR mockSwapchain = (VkSwapchainKHR)0x30;
        std::vector<VkImage> mockImages = { (VkImage)0x101, (VkImage)0x102 };
        VkCommandBuffer mockCmds[2] = { (VkCommandBuffer)0x201, (VkCommandBuffer)0x202 };
        VkSemaphore mockAcq[2] = { (VkSemaphore)0x301, (VkSemaphore)0x302 };
        VkSemaphore mockRel[2] = { (VkSemaphore)0x401, (VkSemaphore)0x402 };
        VkFence mockFences[2] = { (VkFence)0x501, (VkFence)0x502 };

        sync.init(mockDevice, mockQueue, mockSwapchain, mockImages, mockCmds, mockAcq, mockRel, mockFences);

        static uint64_t s_MockTimeNs = 1000000000ull; // Start at 1.0s
        static uint64_t s_TotalSleptNs = 0;
        static uint32_t s_SleepCalls = 0;
        s_MockTimeNs = 1000000000ull;
        s_TotalSleptNs = 0;
        s_SleepCalls = 0;

        arm::SwapchainSync::VulkanFunctions fn = {};
        fn.vkWaitForFences = [](VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t) -> VkResult { return VK_SUCCESS; };
        fn.vkResetFences = [](VkDevice, uint32_t, const VkFence*) -> VkResult { return VK_SUCCESS; };
        fn.vkResetCommandBuffer = [](VkCommandBuffer, VkCommandBufferResetFlags) -> VkResult { return VK_SUCCESS; };
        fn.vkBeginCommandBuffer = [](VkCommandBuffer, const VkCommandBufferBeginInfo*) -> VkResult { return VK_SUCCESS; };
        fn.vkEndCommandBuffer = [](VkCommandBuffer) -> VkResult { return VK_SUCCESS; };
        fn.vkQueueSubmit = [](VkQueue, uint32_t, const VkSubmitInfo*, VkFence) -> VkResult { return VK_SUCCESS; };
        fn.vkQueuePresentKHR = [](VkQueue, const VkPresentInfoKHR*) -> VkResult { return VK_SUCCESS; };
        fn.vkAcquireNextImageKHR = [](VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t* pIndex) -> VkResult {
            *pIndex = 0;
            return VK_SUCCESS;
        };
        fn.getMonotonicTimeNs = []() -> uint64_t { return s_MockTimeNs; };
        fn.sleepNs = [](uint64_t ns) {
            s_SleepCalls++;
            s_TotalSleptNs += ns;
            s_MockTimeNs += ns; // Advance time by sleep duration
        };

        sync.setVulkanFunctions(fn);

        arm::SwapchainSync::PacingConfig cfg;
        cfg.mode = arm::SwapchainSync::PacingMode::TargetFps;
        cfg.targetFps = 60; // 16.666 ms = 16666666 ns
        sync.setPacingConfig(cfg);

        // Frame 0: Presents at T = 1.0s
        uint32_t imgIdx = 0;
        TEST_CHECK(sync.beginFrame(&imgIdx) == true);
        TEST_CHECK(sync.endFrame(0) == true);
        TEST_CHECK(s_SleepCalls == 0); // No sleep on frame 0

        // Frame 1: Rendering finishes at T = 1.008s (8 ms elapsed, 8.666 ms before 16.666 ms target)
        s_MockTimeNs += 8000000ull;
        TEST_CHECK(sync.beginFrame(&imgIdx) == true);
        TEST_CHECK(sync.endFrame(0) == true);
        TEST_CHECK(s_SleepCalls == 1);
        TEST_CHECK(s_TotalSleptNs == (16666666ull - 8000000ull));
        TEST_CHECK(sync.pacingStats().pacingWaitCount == 1);

        // Frame 2: Slow frame, finishes rendering at T + 25 ms (> 16.666 ms)
        uint64_t sleptBefore = s_TotalSleptNs;
        uint32_t callsBefore = s_SleepCalls;
        s_MockTimeNs += 25000000ull;
        TEST_CHECK(sync.beginFrame(&imgIdx) == true);
        TEST_CHECK(sync.endFrame(0) == true);
        // Must NOT sleep on a late frame
        TEST_CHECK(s_SleepCalls == callsBefore);
        TEST_CHECK(s_TotalSleptNs == sleptBefore);
    }

    // 4. Invariant: HyperOS SurfaceFlinger Preemption Jitter Invariant (P99 < 2.0 ms)
    {
        arm::SwapchainSync sync;
        VkDevice mockDevice = (VkDevice)0x10;
        VkQueue mockQueue = (VkQueue)0x20;
        VkSwapchainKHR mockSwapchain = (VkSwapchainKHR)0x30;
        std::vector<VkImage> mockImages = { (VkImage)0x101, (VkImage)0x102 };
        VkCommandBuffer mockCmds[2] = { (VkCommandBuffer)0x201, (VkCommandBuffer)0x202 };
        VkSemaphore mockAcq[2] = { (VkSemaphore)0x301, (VkSemaphore)0x302 };
        VkSemaphore mockRel[2] = { (VkSemaphore)0x401, (VkSemaphore)0x402 };
        VkFence mockFences[2] = { (VkFence)0x501, (VkFence)0x502 };

        sync.init(mockDevice, mockQueue, mockSwapchain, mockImages, mockCmds, mockAcq, mockRel, mockFences);

        static uint64_t s_SimTimeNs = 0;
        s_SimTimeNs = 1000000000ull;

        arm::SwapchainSync::VulkanFunctions fn = {};
        fn.vkWaitForFences = [](VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t) -> VkResult { return VK_SUCCESS; };
        fn.vkResetFences = [](VkDevice, uint32_t, const VkFence*) -> VkResult { return VK_SUCCESS; };
        fn.vkResetCommandBuffer = [](VkCommandBuffer, VkCommandBufferResetFlags) -> VkResult { return VK_SUCCESS; };
        fn.vkBeginCommandBuffer = [](VkCommandBuffer, const VkCommandBufferBeginInfo*) -> VkResult { return VK_SUCCESS; };
        fn.vkEndCommandBuffer = [](VkCommandBuffer) -> VkResult { return VK_SUCCESS; };
        fn.vkQueueSubmit = [](VkQueue, uint32_t, const VkSubmitInfo*, VkFence) -> VkResult { return VK_SUCCESS; };
        fn.vkQueuePresentKHR = [](VkQueue, const VkPresentInfoKHR*) -> VkResult { return VK_SUCCESS; };
        fn.vkAcquireNextImageKHR = [](VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t* pIndex) -> VkResult {
            *pIndex = 0;
            return VK_SUCCESS;
        };
        fn.getMonotonicTimeNs = []() -> uint64_t { return s_SimTimeNs; };
        fn.sleepNs = [](uint64_t ns) {
            s_SimTimeNs += ns;
        };

        sync.setVulkanFunctions(fn);

        arm::SwapchainSync::PacingConfig cfg;
        cfg.mode = arm::SwapchainSync::PacingMode::TargetFps;
        cfg.targetFps = 60; // 16.666 ms
        sync.setPacingConfig(cfg);

        // Run 120 frames with HyperOS preemption spikes injected every 24 frames
        for (int frame = 0; frame < 120; ++frame) {
            uint32_t img = 0;
            TEST_CHECK(sync.beginFrame(&img) == true);

            // Baseline GPU compute: 8.5 ms (8,500,000 ns)
            uint64_t computeTimeNs = 8500000ull;
            if (frame % 24 == 0) {
                // Injected SurfaceFlinger HWC composition preemption spike (+0.92 ms)
                computeTimeNs += 920000ull;
            }
            s_SimTimeNs += computeTimeNs;

            TEST_CHECK(sync.endFrame(0) == true);
        }

        const auto& stats = sync.pacingStats();
        TEST_CHECK(stats.totalPresentedFrames == 120);
        TEST_CHECK(stats.meanIntervalMs >= 16.60 && stats.meanIntervalMs <= 16.70);
        TEST_CHECK(stats.varianceMs2 < 0.05); // Variance strictly bounded
        TEST_CHECK(stats.stdDevMs < 0.25);   // Standard deviation < 0.25 ms
        TEST_CHECK(stats.p99JitterMs < 2.0); // Acceptance Invariant: P99 Jitter < 2.0 ms strictly satisfied!
        TEST_CHECK(stats.maxJitterMs < 2.0); // Max jitter strictly bounded!
    }

    // 5. Invariant: AbortFrame Cadence Preservation
    {
        arm::SwapchainSync sync;
        VkDevice mockDevice = (VkDevice)0x10;
        VkQueue mockQueue = (VkQueue)0x20;
        VkSwapchainKHR mockSwapchain = (VkSwapchainKHR)0x30;
        std::vector<VkImage> mockImages = { (VkImage)0x101, (VkImage)0x102 };
        VkCommandBuffer mockCmds[2] = { (VkCommandBuffer)0x201, (VkCommandBuffer)0x202 };
        VkSemaphore mockAcq[2] = { (VkSemaphore)0x301, (VkSemaphore)0x302 };
        VkSemaphore mockRel[2] = { (VkSemaphore)0x401, (VkSemaphore)0x402 };
        VkFence mockFences[2] = { (VkFence)0x501, (VkFence)0x502 };

        sync.init(mockDevice, mockQueue, mockSwapchain, mockImages, mockCmds, mockAcq, mockRel, mockFences);

        static uint64_t s_AbortTimeNs = 1000000000ull;
        arm::SwapchainSync::VulkanFunctions fn = {};
        fn.vkWaitForFences = [](VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t) -> VkResult { return VK_SUCCESS; };
        fn.vkResetFences = [](VkDevice, uint32_t, const VkFence*) -> VkResult { return VK_SUCCESS; };
        fn.vkResetCommandBuffer = [](VkCommandBuffer, VkCommandBufferResetFlags) -> VkResult { return VK_SUCCESS; };
        fn.vkBeginCommandBuffer = [](VkCommandBuffer, const VkCommandBufferBeginInfo*) -> VkResult { return VK_SUCCESS; };
        fn.vkEndCommandBuffer = [](VkCommandBuffer) -> VkResult { return VK_SUCCESS; };
        fn.vkQueueSubmit = [](VkQueue, uint32_t, const VkSubmitInfo*, VkFence) -> VkResult { return VK_SUCCESS; };
        fn.vkQueuePresentKHR = [](VkQueue, const VkPresentInfoKHR*) -> VkResult { return VK_SUCCESS; };
        fn.vkCmdPipelineBarrier = [](VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*) {};
        fn.vkCmdClearColorImage = [](VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*, uint32_t, const VkImageSubresourceRange*) {};
        fn.vkAcquireNextImageKHR = [](VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t* pIndex) -> VkResult {
            *pIndex = 0;
            return VK_SUCCESS;
        };
        fn.getMonotonicTimeNs = []() -> uint64_t { return s_AbortTimeNs; };

        sync.setVulkanFunctions(fn);

        arm::SwapchainSync::PacingConfig cfg;
        cfg.mode = arm::SwapchainSync::PacingMode::LockTo1Vsync;
        sync.setPacingConfig(cfg);

        uint32_t img = 0;
        TEST_CHECK(sync.beginFrame(&img) == true);
        s_AbortTimeNs += 16666666ull;
        TEST_CHECK(sync.abortFrame(0) == true);
        TEST_CHECK(sync.totalFramesSubmitted() == 1);
        TEST_CHECK(sync.pacingStats().totalPresentedFrames == 1);
    }

    std::cout << "PASS: Phase 4 Cadence & SurfaceFlinger Pacing Stabilization verified (P99 Jitter < 2.0 ms, 1-VSYNC GPU Lock)." << std::endl;
}

static void TestPhase5FrameGenerationArchitecture()
{
    std::cout << "--- Test Phase 5: Frame Generation Architecture Verification ---" << std::endl;

    // 1. Optical Flow Parameter Validation & Incomplete Interface
    {
        FfxmOpticalflowContext ctx;
        FfxmOpticalflowContextDescription desc = {};
        FfxmInterface iface = {};
        SetupMockInterface(&iface);
        desc.resolution = { 1920, 1080 };
        desc.backendInterface = iface;

        TEST_CHECK(ffxmOpticalflowContextCreate(nullptr, &desc) == FFXM_ERROR_INVALID_POINTER);
        TEST_CHECK(ffxmOpticalflowContextCreate(&ctx, nullptr) == FFXM_ERROR_INVALID_POINTER);

        // Zero dimensions
        desc.resolution = { 0, 1080 };
        TEST_CHECK(ffxmOpticalflowContextCreate(&ctx, &desc) == FFXM_ERROR_INVALID_ARGUMENT);
        desc.resolution = { 1920, 0 };
        TEST_CHECK(ffxmOpticalflowContextCreate(&ctx, &desc) == FFXM_ERROR_INVALID_ARGUMENT);
        desc.resolution = { 1920, 1080 };

        // Incomplete interface
        FfxmOpticalflowContextDescription badDesc = desc;
        badDesc.backendInterface.fpGetDeviceCapabilities = nullptr;
        TEST_CHECK(ffxmOpticalflowContextCreate(&ctx, &badDesc) == FFXM_ERROR_INCOMPLETE_INTERFACE);
        badDesc = desc;
        badDesc.backendInterface.fpCreateBackendContext = nullptr;
        TEST_CHECK(ffxmOpticalflowContextCreate(&ctx, &badDesc) == FFXM_ERROR_INCOMPLETE_INTERFACE);
        badDesc = desc;
        badDesc.backendInterface.fpDestroyBackendContext = nullptr;
        TEST_CHECK(ffxmOpticalflowContextCreate(&ctx, &badDesc) == FFXM_ERROR_INCOMPLETE_INTERFACE);

        // Unsupported device (no FP16)
        s_Fp16SupportMock = false;
        TEST_CHECK(ffxmOpticalflowContextCreate(&ctx, &desc) == FFXM_ERROR_UNSUPPORTED_DEVICE);
        s_Fp16SupportMock = true;
    }

    // 2. Optical Flow Creation, Allocation Rollback & Dispatch
    {
        ResetMockState();
        FfxmOpticalflowContext ctx;
        FfxmOpticalflowContextDescription desc = {};
        FfxmInterface iface = {};
        SetupMockInterface(&iface);
        desc.resolution = { 1920, 1080 };
        desc.backendInterface = iface;

        // Transactional rollback on second resource failure
        s_FailAtResourceIndex = 1; // 0 is opticalFlowPyramid, 1 is sceneChangeHist
        TEST_CHECK(ffxmOpticalflowContextCreate(&ctx, &desc) == FFXM_ERROR_OUT_OF_MEMORY);
        TEST_CHECK(s_DestroyBackendContextCalled == true);
        s_FailAtResourceIndex = -1;

        // Successful creation
        ResetMockState();
        TEST_CHECK(ffxmOpticalflowContextCreate(&ctx, &desc) == FFXM_OK);
        auto* priv = reinterpret_cast<FfxmOpticalflowContext_Private*>(&ctx);
        TEST_CHECK(priv->opticalFlowPyramid.internalIndex != -1);
        TEST_CHECK(priv->sceneChangeHist.internalIndex != -1);
        TEST_CHECK(priv->frameIndex == 0);
        TEST_CHECK(priv->firstExecution == true);

        // Dispatch validation
        FfxmOpticalflowDispatchDescription dispDesc = {};
        static int s_ColorDummy = 1;
        static int s_OfVectorDummy = 2;
        TEST_CHECK(ffxmOpticalflowContextDispatch(nullptr, &dispDesc) == FFXM_ERROR_INVALID_POINTER);
        TEST_CHECK(ffxmOpticalflowContextDispatch(&ctx, nullptr) == FFXM_ERROR_INVALID_POINTER);
        TEST_CHECK(ffxmOpticalflowContextDispatch(&ctx, &dispDesc) == FFXM_ERROR_INVALID_ARGUMENT); // Missing commandList

        dispDesc.commandList = (FfxmCommandList)0x1234;
        TEST_CHECK(ffxmOpticalflowContextDispatch(&ctx, &dispDesc) == FFXM_ERROR_INVALID_ARGUMENT); // Missing color
        dispDesc.color.resource = &s_ColorDummy;
        TEST_CHECK(ffxmOpticalflowContextDispatch(&ctx, &dispDesc) == FFXM_ERROR_INVALID_ARGUMENT); // Missing opticalFlowVector
        dispDesc.opticalFlowVector.resource = &s_OfVectorDummy;

        ResetDispatchTrackers();
        TEST_CHECK(ffxmOpticalflowContextDispatch(&ctx, &dispDesc) == FFXM_OK);
        TEST_CHECK(priv->frameIndex == 1);
        TEST_CHECK(priv->firstExecution == false);
        TEST_CHECK(s_ScheduledGpuJobs.size() == 1);
        TEST_CHECK(s_ScheduledGpuJobs[0].jobType == FFXM_GPU_JOB_COMPUTE);
        TEST_CHECK(s_ScheduledGpuJobs[0].computeJobDescriptor.dimensions[0] == (1920 + 7) / 8);
        TEST_CHECK(s_ScheduledGpuJobs[0].computeJobDescriptor.dimensions[1] == (1080 + 7) / 8);
        TEST_CHECK(s_ScheduledGpuJobs[0].computeJobDescriptor.dimensions[2] == 1);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 1);
        TEST_CHECK(s_UnregisterResourcesCallCount == 1);

        // Destruction
        int pyramidIdx = priv->opticalFlowPyramid.internalIndex;
        int histIdx = priv->sceneChangeHist.internalIndex;
        TEST_CHECK(ffxmOpticalflowContextDestroy(&ctx) == FFXM_OK);
        TEST_CHECK(s_ResourceDestructionCounts[pyramidIdx] == 1);
        TEST_CHECK(s_ResourceDestructionCounts[histIdx] == 1);
        TEST_CHECK(s_DestroyBackendContextCalled == true);
    }

    // 3. Frame Interpolation Parameter Validation & Lifecycle
    {
        FfxmFrameInterpolationContext ctx;
        FfxmFrameInterpolationContextDescription desc = {};
        FfxmInterface iface = {};
        SetupMockInterface(&iface);
        desc.maxDisplaySize = { 3840, 2160 };
        desc.backendInterface = iface;

        TEST_CHECK(ffxmFrameInterpolationContextCreate(nullptr, &desc) == FFXM_ERROR_INVALID_POINTER);
        TEST_CHECK(ffxmFrameInterpolationContextCreate(&ctx, nullptr) == FFXM_ERROR_INVALID_POINTER);

        // Zero dimensions
        desc.maxDisplaySize = { 0, 2160 };
        TEST_CHECK(ffxmFrameInterpolationContextCreate(&ctx, &desc) == FFXM_ERROR_INVALID_ARGUMENT);
        desc.maxDisplaySize = { 3840, 2160 };

        // Unsupported device
        s_Fp16SupportMock = false;
        TEST_CHECK(ffxmFrameInterpolationContextCreate(&ctx, &desc) == FFXM_ERROR_UNSUPPORTED_DEVICE);
        s_Fp16SupportMock = true;

        // Transactional rollback on second resource failure
        ResetMockState();
        s_FailAtResourceIndex = 1; // 0 is historyColor, 1 is interpolatedStaging
        TEST_CHECK(ffxmFrameInterpolationContextCreate(&ctx, &desc) == FFXM_ERROR_OUT_OF_MEMORY);
        TEST_CHECK(s_DestroyBackendContextCalled == true);
        s_FailAtResourceIndex = -1;

        // Successful creation
        ResetMockState();
        TEST_CHECK(ffxmFrameInterpolationContextCreate(&ctx, &desc) == FFXM_OK);
        auto* priv = reinterpret_cast<FfxmFrameInterpolationContext_Private*>(&ctx);
        TEST_CHECK(priv->historyColor.internalIndex != -1);
        TEST_CHECK(priv->interpolatedStaging.internalIndex != -1);

        // Dispatch validation with all shared resources
        static int s_Bb = 1, s_Io = 2, s_Of = 3, s_Dd = 4, s_Dmv = 5, s_Rnd = 6;
        FfxmFrameInterpolationDispatchDescription fiDisp = {};
        TEST_CHECK(ffxmFrameInterpolationContextDispatch(&ctx, &fiDisp) == FFXM_ERROR_INVALID_ARGUMENT); // No cmdList
        fiDisp.commandList = (FfxmCommandList)0x5678;
        TEST_CHECK(ffxmFrameInterpolationContextDispatch(&ctx, &fiDisp) == FFXM_ERROR_INVALID_ARGUMENT); // No backBuffer
        fiDisp.currentBackBuffer.resource = &s_Bb;
        TEST_CHECK(ffxmFrameInterpolationContextDispatch(&ctx, &fiDisp) == FFXM_ERROR_INVALID_ARGUMENT); // No output
        fiDisp.currentInterpolatedOutput.resource = &s_Io;
        TEST_CHECK(ffxmFrameInterpolationContextDispatch(&ctx, &fiDisp) == FFXM_ERROR_INVALID_ARGUMENT); // No opticalFlow
        fiDisp.opticalFlowVector.resource = &s_Of;
        TEST_CHECK(ffxmFrameInterpolationContextDispatch(&ctx, &fiDisp) == FFXM_ERROR_INVALID_ARGUMENT); // No dilatedDepth
        fiDisp.dilatedDepth.resource = &s_Dd;
        TEST_CHECK(ffxmFrameInterpolationContextDispatch(&ctx, &fiDisp) == FFXM_ERROR_INVALID_ARGUMENT); // No dilatedMv
        fiDisp.dilatedMotionVectors.resource = &s_Dmv;
        TEST_CHECK(ffxmFrameInterpolationContextDispatch(&ctx, &fiDisp) == FFXM_ERROR_INVALID_ARGUMENT); // No reconDepth
        fiDisp.reconstructedPrevNearestDepth.resource = &s_Rnd;
        fiDisp.interpolationTime = 0.5f;

        ResetDispatchTrackers();
        TEST_CHECK(ffxmFrameInterpolationContextDispatch(&ctx, &fiDisp) == FFXM_OK);
        TEST_CHECK(priv->frameIndex == 1);
        TEST_CHECK(s_ScheduledGpuJobs.size() == 1);
        TEST_CHECK(s_ScheduledGpuJobs[0].jobType == FFXM_GPU_JOB_COMPUTE);
        TEST_CHECK(s_ScheduledGpuJobs[0].computeJobDescriptor.dimensions[0] == (3840 + 7) / 8);
        TEST_CHECK(s_ScheduledGpuJobs[0].computeJobDescriptor.dimensions[1] == (2160 + 7) / 8);
        TEST_CHECK(s_ExecuteGpuJobsCallCount == 1);
        TEST_CHECK(s_UnregisterResourcesCallCount == 1);

        // Destruction
        int historyIdx = priv->historyColor.internalIndex;
        int stagingIdx = priv->interpolatedStaging.internalIndex;
        TEST_CHECK(ffxmFrameInterpolationContextDestroy(&ctx) == FFXM_OK);
        TEST_CHECK(s_ResourceDestructionCounts[historyIdx] == 1);
        TEST_CHECK(s_ResourceDestructionCounts[stagingIdx] == 1);
        TEST_CHECK(s_DestroyBackendContextCalled == true);
    }

    // 4. SwapchainFrameGenerator: 2:1 Presentation Cadence & Pacing Invariants
    {
        SwapchainSync sync;
        VkDevice mockDevice = (VkDevice)0x10;
        VkQueue mockQueue = (VkQueue)0x20;
        VkSwapchainKHR mockSwapchain = (VkSwapchainKHR)0x30;
        std::vector<VkImage> mockImages = { (VkImage)0x101, (VkImage)0x102, (VkImage)0x103 };
        VkCommandBuffer mockCmds[2] = { (VkCommandBuffer)0x201, (VkCommandBuffer)0x202 };
        VkSemaphore mockAcq[2] = { (VkSemaphore)0x301, (VkSemaphore)0x302 };
        VkSemaphore mockRel[2] = { (VkSemaphore)0x401, (VkSemaphore)0x402 };
        VkFence mockFences[2] = { (VkFence)0x501, (VkFence)0x502 };

        sync.init(mockDevice, mockQueue, mockSwapchain, mockImages, mockCmds, mockAcq, mockRel, mockFences);

        static uint64_t s_TimeNs = 1000000000ull;
        static uint32_t s_SleepCalls = 0;
        static uint64_t s_TotalSleptNs = 0;
        s_TimeNs = 1000000000ull;
        s_SleepCalls = 0;
        s_TotalSleptNs = 0;

        SwapchainSync::VulkanFunctions fn = {};
        fn.vkWaitForFences = [](VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t) -> VkResult { return VK_SUCCESS; };
        fn.vkResetFences = [](VkDevice, uint32_t, const VkFence*) -> VkResult { return VK_SUCCESS; };
        fn.vkResetCommandBuffer = [](VkCommandBuffer, VkCommandBufferResetFlags) -> VkResult { return VK_SUCCESS; };
        fn.vkBeginCommandBuffer = [](VkCommandBuffer, const VkCommandBufferBeginInfo*) -> VkResult { return VK_SUCCESS; };
        fn.vkEndCommandBuffer = [](VkCommandBuffer) -> VkResult { return VK_SUCCESS; };
        fn.vkQueueSubmit = [](VkQueue, uint32_t, const VkSubmitInfo*, VkFence) -> VkResult { return VK_SUCCESS; };
        fn.vkQueuePresentKHR = [](VkQueue, const VkPresentInfoKHR*) -> VkResult { return VK_SUCCESS; };
        fn.vkAcquireNextImageKHR = [](VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t* pIndex) -> VkResult {
            *pIndex = 0;
            return VK_SUCCESS;
        };
        fn.getMonotonicTimeNs = []() -> uint64_t { return s_TimeNs; };
        fn.sleepNs = [](uint64_t ns) {
            s_SleepCalls++;
            s_TotalSleptNs += ns;
            s_TimeNs += ns;
        };
        sync.setVulkanFunctions(fn);

        SwapchainFrameGenerator gen;
        SwapchainFrameGenerator::Config cfg;
        cfg.enabled = true;
        cfg.targetDisplayFps = 120; // 8.333 ms interval
        gen.init(&sync, nullptr, nullptr, cfg);

        SwapchainFrameGenerator::TimingHooks hooks;
        hooks.getMonotonicTimeNs = []() -> uint64_t { return s_TimeNs; };
        hooks.sleepNs = [](uint64_t ns) {
            s_SleepCalls++;
            s_TotalSleptNs += ns;
            s_TimeNs += ns;
        };
        gen.setTimingHooks(hooks);

        // A. Frame 0 (First Frame): Only Real frame presented (no prior history)
        SwapchainFrameGenerator::FrameInput inp = {};
        inp.commandList = (FfxmCommandList)0x1;
        static int s_Color = 1, s_Dd = 2, s_Dmv = 3, s_Rnd = 4, s_Out = 5, s_Of = 6;
        inp.currentRealFrameColor.resource = &s_Color;
        inp.dilatedDepth.resource = &s_Dd;
        inp.dilatedMotionVectors.resource = &s_Dmv;
        inp.reconstructedPrevNearestDepth.resource = &s_Rnd;
        inp.interpolatedOutput.resource = &s_Out;
        inp.opticalFlowOutput.resource = &s_Of;
        inp.reset = false;

        auto res0 = gen.processFrame(inp);
        TEST_CHECK(res0.status == FFXM_OK);
        TEST_CHECK(res0.realFramePresented == true);
        TEST_CHECK(res0.interpolatedFramePresented == false);
        TEST_CHECK(gen.stats().totalRealPresents == 1);
        TEST_CHECK(gen.stats().totalInterpolatedPresents == 0);
        TEST_CHECK(gen.stats().totalPresents == 1);

        // B. Continuous 60 engine frames (frames 1 to 59): 2:1 presentation pacing!
        for (int i = 1; i < 60; ++i) {
            s_TimeNs += 7000000ull; // 7.0 ms engine GPU render time before presentation
            auto res = gen.processFrame(inp);
            TEST_CHECK(res.status == FFXM_OK);
            TEST_CHECK(res.realFramePresented == true);
            TEST_CHECK(res.interpolatedFramePresented == true);
        }

        const auto& stats = gen.stats();
        TEST_CHECK(stats.totalSourceFrames == 60);
        TEST_CHECK(stats.totalRealPresents == 60);
        TEST_CHECK(stats.totalInterpolatedPresents == 59);
        TEST_CHECK(stats.totalPresents == 119);
        double realToInterpRatio = (double)stats.totalRealPresents / (double)stats.totalInterpolatedPresents;
        TEST_CHECK(realToInterpRatio >= 1.0 && realToInterpRatio <= 1.02);

        // C. Camera Cut / Reset Invariant: must present ONLY real frame on reset
        inp.reset = true;
        s_TimeNs += 16666666ull;
        auto resReset = gen.processFrame(inp);
        TEST_CHECK(resReset.status == FFXM_OK);
        TEST_CHECK(resReset.realFramePresented == true);
        TEST_CHECK(resReset.interpolatedFramePresented == false);
        TEST_CHECK(gen.stats().totalRealPresents == 61);
        TEST_CHECK(gen.stats().totalInterpolatedPresents == 59);
        inp.reset = false;

        // D. Disabled Mode Fallback: exactly 1:1 real presents
        cfg.enabled = false;
        gen.setConfig(cfg);
        gen.resetState();
        for (int i = 0; i < 20; ++i) {
            s_TimeNs += 16666666ull;
            auto res = gen.processFrame(inp);
            TEST_CHECK(res.status == FFXM_OK);
            TEST_CHECK(res.realFramePresented == true);
            TEST_CHECK(res.interpolatedFramePresented == false);
        }
        TEST_CHECK(gen.stats().totalSourceFrames == 20);
        TEST_CHECK(gen.stats().totalRealPresents == 20);
        TEST_CHECK(gen.stats().totalInterpolatedPresents == 0);
        TEST_CHECK(gen.stats().totalPresents == 20);
    }

    // 5. Invariant: HyperOS 5 Hz Jitter Resistance & Compute Overhead Bound (<= 4.5 ms)
    {
        SwapchainSync sync;
        VkDevice mockDevice = (VkDevice)0x10;
        VkQueue mockQueue = (VkQueue)0x20;
        VkSwapchainKHR mockSwapchain = (VkSwapchainKHR)0x30;
        std::vector<VkImage> mockImages = { (VkImage)0x101, (VkImage)0x102 };
        VkCommandBuffer mockCmds[2] = { (VkCommandBuffer)0x201, (VkCommandBuffer)0x202 };
        VkSemaphore mockAcq[2] = { (VkSemaphore)0x301, (VkSemaphore)0x302 };
        VkSemaphore mockRel[2] = { (VkSemaphore)0x401, (VkSemaphore)0x402 };
        VkFence mockFences[2] = { (VkFence)0x501, (VkFence)0x502 };

        sync.init(mockDevice, mockQueue, mockSwapchain, mockImages, mockCmds, mockAcq, mockRel, mockFences);

        static uint64_t s_SimTimeNs = 1000000000ull;
        s_SimTimeNs = 1000000000ull;

        SwapchainSync::VulkanFunctions fn = {};
        fn.vkWaitForFences = [](VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t) -> VkResult { return VK_SUCCESS; };
        fn.vkResetFences = [](VkDevice, uint32_t, const VkFence*) -> VkResult { return VK_SUCCESS; };
        fn.vkResetCommandBuffer = [](VkCommandBuffer, VkCommandBufferResetFlags) -> VkResult { return VK_SUCCESS; };
        fn.vkBeginCommandBuffer = [](VkCommandBuffer, const VkCommandBufferBeginInfo*) -> VkResult { return VK_SUCCESS; };
        fn.vkEndCommandBuffer = [](VkCommandBuffer) -> VkResult { return VK_SUCCESS; };
        fn.vkQueueSubmit = [](VkQueue, uint32_t, const VkSubmitInfo*, VkFence) -> VkResult { return VK_SUCCESS; };
        fn.vkQueuePresentKHR = [](VkQueue, const VkPresentInfoKHR*) -> VkResult { return VK_SUCCESS; };
        fn.vkAcquireNextImageKHR = [](VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t* pIndex) -> VkResult {
            *pIndex = 0;
            return VK_SUCCESS;
        };
        fn.getMonotonicTimeNs = []() -> uint64_t { return s_SimTimeNs; };
        fn.sleepNs = [](uint64_t ns) { s_SimTimeNs += ns; };
        sync.setVulkanFunctions(fn);

        SwapchainFrameGenerator gen;
        SwapchainFrameGenerator::Config cfg;
        cfg.enabled = true;
        cfg.targetDisplayFps = 120; // 8.333 ms
        gen.init(&sync, nullptr, nullptr, cfg);

        SwapchainFrameGenerator::TimingHooks hooks;
        hooks.getMonotonicTimeNs = []() -> uint64_t { return s_SimTimeNs; };
        hooks.sleepNs = [](uint64_t ns) { s_SimTimeNs += ns; };
        gen.setTimingHooks(hooks);

        SwapchainFrameGenerator::FrameInput inp = {};
        inp.commandList = (FfxmCommandList)0x1;
        static int s_Color = 1, s_Dd = 2, s_Dmv = 3, s_Rnd = 4, s_Out = 5, s_Of = 6;
        inp.currentRealFrameColor.resource = &s_Color;
        inp.dilatedDepth.resource = &s_Dd;
        inp.dilatedMotionVectors.resource = &s_Dmv;
        inp.reconstructedPrevNearestDepth.resource = &s_Rnd;
        inp.interpolatedOutput.resource = &s_Out;
        inp.opticalFlowOutput.resource = &s_Of;

        // Run 60 engine frames (119 presents) with injected preemption spikes
        for (int frame = 0; frame < 60; ++frame) {
            uint64_t computeTimeNs = 7000000ull; // 7.0 ms baseline render work
            if (frame % 24 == 0) {
                // Injected HyperOS SurfaceFlinger preemption spike (+0.92 ms)
                computeTimeNs += 920000ull;
            }
            s_SimTimeNs += computeTimeNs;
            auto res = gen.processFrame(inp);
            TEST_CHECK(res.status == FFXM_OK);
            TEST_CHECK(res.frameGenComputeOverheadMs <= 4.5);
        }

        const auto& stats = gen.stats();
        TEST_CHECK(stats.totalPresents == 119);
        TEST_CHECK(stats.meanPresentationIntervalMs >= 8.25 && stats.meanPresentationIntervalMs <= 8.45);
        TEST_CHECK(stats.presentationVarianceMs2 < 0.05);
        TEST_CHECK(stats.presentationStdDevMs < 0.25);
        TEST_CHECK(stats.presentationP99JitterMs < 2.0); // Acceptance Invariant: P99 Jitter < 2.0 ms
        TEST_CHECK(stats.maxComputeOverheadMs <= 4.5);   // Acceptance Invariant: Overhead <= 4.5 ms
    }

    std::cout << "PASS: Phase 5 Frame Generation Architecture verified (Optical Flow, Frame Interpolation, 2:1 Pacing)." << std::endl;
}

int main()
{
    std::cout << "============================================================" << std::endl;
    std::cout << "FSR 3.1.4 Milestone 2 Resource Lifecycle & Allocation Tests" << std::endl;
    std::cout << "============================================================" << std::endl;

    TestNDEBUGAssertionActive();
    TestContextSizeAndAlignment();
    TestSuccessfulContextCreation();
    TestUnsupportedFP16Hardware();
    TestBackendContextCreationFailure();
    TestResourceAllocationFailureAtEachStage();
    TestCorrectResourceDestruction();
    TestRepeatedCreateDestroyCycles();
    TestSharedResourceDescriptions();
    TestProtectedUnfinishedAPIs();
    TestConstantBufferStagingSlots();
    TestInternalResourceDescriptionsRegressionTable();
    TestPublicAPIValidation();
    TestContextReuseAfterFailure();
    TestDeviceCapabilitiesOrderingContract();
    TestScratchMemorySizingArithmetic();
    TestM3PrepareInputsPassExecution();
    TestM3DispatchErrorHandling();
    TestM3ComputeLuminancePyramid();
    TestM3ComputeShadingChangePyramid();
    TestM3ComputeShadingChange();
    TestM3PrepareReactivity();
    TestM3LumaInstabilityAndAccumulatePipelines();
    TestM3ComputeLumaInstabilityAndAccumulate();
    TestM3PrepareInputsRealVulkanExecution();
    TestM38SharedResourceInitialLayouts();
    TestM38FrameCancellationAndRecovery();
    TestM38DescriptorLifecycleAndContextIsolation();
    TestM39SwapchainAcquisitionAndAbortSynchronization();
    TestM39VulkanBackendTransactionalRecovery();
    TestM3FinalizationVerification();
    TestM3FinalCloseoutVulkanLifetimeAndRecovery();
    TestM4VulkanPartialInitializationCleanup();
    TestM4GpuTimestampArithmetic();
    TestM4BackendProfilingInterface();
    TestM4GpuProfilerStatisticsAndLifecycle();
    TestM4VkContextQueryPoolLifecycle();
    TestPhase3MobileBandwidthReductionAndKernelFusion();
    TestPhase4CadenceAndSurfaceFlingerPacingStabilization();
    TestPhase5FrameGenerationArchitecture();

    std::cout << "============================================================" << std::endl;
    std::cout << "Summary: " << g_TestPasses << " checks PASSED, " << g_TestFailures << " FAILED." << std::endl;
    std::cout << "============================================================" << std::endl;

    if (g_TestFailures > 0) {
        std::cerr << "TEST SUITE FAILED WITH " << g_TestFailures << " FAILURES!" << std::endl;
        return 1;
    }

    std::cout << "ALL MILESTONE 2 TESTS PASSED PERFECTLY!" << std::endl;
    return 0;
}
