// Copyright  © 2023 Advanced Micro Devices, Inc.
// Copyright  © 2024-2026 Arm Limited.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

/// @defgroup VKBackend Vulkan Backend
/// FidelityFX SDK native backend implementation for Vulkan.
///
/// @ingroup Backends

#pragma once

#ifdef FFXM_VKLOADER_VOLK
// __ANDROID__ must be tested before __linux__: the NDK defines both, and Android
// has no XCB/Xlib surfaces.
#if defined(__ANDROID__)
#	define VK_USE_PLATFORM_ANDROID_KHR 1
#elif defined(__linux__)
#	define VK_USE_PLATFORM_XCB_KHR 1
#	define VK_USE_PLATFORM_XLIB_KHR 1
#elif defined(_WIN32)
#	define VK_USE_PLATFORM_WIN32_KHR 1
#else
#	error Not implemented
#endif
#include <Volk/volk.h>
#else // FFXM_VKLOADER_VOLK
#include <vulkan/vulkan.h>
#endif

#include <host/ffxm_interface.h>

namespace arm
{

#if defined(__cplusplus)
extern "C" {
#endif // #if defined(__cplusplus)

/// Query how much memory is required for the Vulkan backend's scratch buffer.
///
/// @param [in] physicalDevice              A pointer to the VkPhysicalDevice device.
/// @param [in] maxContexts                 The maximum number of simultaneous effect contexts that will share the backend.
///                                         (Note that some effects contain internal contexts which count towards this maximum)
///
/// @returns
/// The size (in bytes) of the required scratch memory buffer for the VK backend.
///
/// @ingroup VKBackend
FFXM_API size_t ffxmGetScratchMemorySizeVK(VkPhysicalDevice physicalDevice, size_t maxContexts);

/// Convenience structure to hold all VK-related device information
typedef struct VkDeviceContext {
    VkDevice                vkDevice;           /// The Vulkan device
    VkPhysicalDevice        vkPhysicalDevice;   /// The Vulkan physical device
    PFN_vkGetDeviceProcAddr vkDeviceProcAddr;   /// The device's function address table
} VkDeviceContext;

/// Create a <c><i>FfxmDevice</i></c> from a <c><i>VkDevice</i></c>.
///
/// @param [in] vkDeviceContext             A pointer to a VKDeviceContext that holds all needed information
///
/// @returns
/// An abstract FidelityFX device.
///
/// @ingroup VKBackend
FFXM_API FfxmDevice ffxmGetDeviceVK(VkDeviceContext* vkDeviceContext);

/// Populate an interface with pointers for the VK backend.
///
/// @param [out] backendInterface           A pointer to a <c><i>FfxmInterface</i></c> structure to populate with pointers.
/// @param [in] device                      A pointer to the VkDevice device.
/// @param [in] scratchBuffer               A pointer to a buffer of memory which can be used by the DirectX(R)12 backend.
/// @param [in] scratchBufferSize           The size (in bytes) of the buffer pointed to by <c><i>scratchBuffer</i></c>.
/// @param [in] maxContexts                 The maximum number of simultaneous effect contexts that will share the backend.
///                                         (Note that some effects contain internal contexts which count towards this maximum)
///
/// @retval
/// FFXM_OK                                  The operation completed successfully.
/// @retval
/// FFXM_ERROR_CODE_INVALID_POINTER          The <c><i>interface</i></c> pointer was <c><i>NULL</i></c>.
///
/// @ingroup VKBackend
FFXM_API FfxmErrorCode ffxmGetInterfaceVK(
    FfxmInterface* backendInterface,
    FfxmDevice device,
    void* scratchBuffer,
    size_t scratchBufferSize,
    size_t maxContexts);

/// Create a <c><i>FfxmCommandList</i></c> from a <c><i>VkCommandBuffer</i></c>.
///
/// @param [in] cmdBuf                      A pointer to the Vulkan command buffer.
///
/// @returns
/// An abstract FidelityFX command list.
///
/// @ingroup VKBackend
FFXM_API FfxmCommandList ffxmGetCommandListVK(VkCommandBuffer cmdBuf);

/// Fetch a <c><i>FfxmResource</i></c> from a <c><i>GPUResource</i></c>.
///
/// @param [in] vkResource                  A pointer to the (agnostic) VK resource.
/// @param [in] ffxmResDescription           An <c><i>FfxmResourceDescription</i></c> for the resource representation.
/// @param [in] ffxmResName                  (optional) A name string to identify the resource in debug mode.
/// @param [in] state                       The state the resource is currently in.
///
/// @returns
/// An abstract FidelityFX resources.
///
/// @ingroup VKBackend
FFXM_API FfxmResource ffxmGetResourceVK(void*  vkResource,
    FfxmResourceDescription                  ffxmResDescription,
    wchar_t*                                ffxmResName,
    FfxmResourceStates                       state = FFXM_RESOURCE_STATE_COMPUTE_READ);

/// Execute all queued GPU jobs recorded in the backend context.
///
/// @param [in] backendInterface           A pointer to a <c><i>FfxmInterface</i></c> structure.
/// @param [in] commandList                The Vulkan command buffer wrapped in a <c><i>FfxmCommandList</i></c>.
///
/// @returns
/// FFXM_OK on success, or the first error code encountered during execution.
///
/// @ingroup VKBackend
FFXM_API FfxmErrorCode ExecuteGpuJobsVK(FfxmInterface* backendInterface, FfxmCommandList commandList);

/// Unregister dynamic resources and recycle descriptor rings for an effect context.
///
/// @param [in] backendInterface           A pointer to a <c><i>FfxmInterface</i></c> structure.
/// @param [in] commandList                The Vulkan command buffer wrapped in a <c><i>FfxmCommandList</i></c>.
/// @param [in] effectContextId            The ID of the effect context.
///
/// @returns
/// FFXM_OK on success.
///
/// @ingroup VKBackend
FFXM_API FfxmErrorCode UnregisterResourcesVK(FfxmInterface* backendInterface, FfxmCommandList commandList, FfxmUInt32 effectContextId);

#if defined(FFXM_ENABLE_TEST_HOOKS)
// Test inspection and injection helpers (gated for testing only)
typedef FfxmErrorCode (*FfxmTestJobExecutionHook)(FfxmUInt32 jobIndex, const FfxmGpuJobDescription* job);
typedef VkResult (VKAPI_PTR *FfxmTestCreateImageViewHook)(VkDevice device, const VkImageViewCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImageView* pView);
typedef void (VKAPI_PTR *FfxmTestDestroyImageViewHook)(VkDevice device, VkImageView imageView, const VkAllocationCallbacks* pAllocator);

FFXM_API void ffxmSetTestJobExecutionHookVK(FfxmTestJobExecutionHook hook);
FFXM_API void ffxmTestSetImageViewHooksVK(FfxmTestCreateImageViewHook createHook, FfxmTestDestroyImageViewHook destroyHook);
FFXM_API FfxmUInt32 ffxmTestGetNextDynamicResourceViewVK(FfxmInterface* backendInterface, FfxmUInt32 effectContextId, FfxmUInt32 frameIndex);
FFXM_API FfxmResourceStates ffxmTestGetResourceCurrentStateVK(FfxmInterface* backendInterface, FfxmUInt32 resourceIndex);
FFXM_API FfxmResourceStates ffxmTestGetResourceCommittedStateVK(FfxmInterface* backendInterface, FfxmUInt32 resourceIndex);
FFXM_API bool ffxmTestGetResourceUndefinedVK(FfxmInterface* backendInterface, FfxmUInt32 resourceIndex);
FFXM_API bool ffxmTestGetResourceCommittedUndefinedVK(FfxmInterface* backendInterface, FfxmUInt32 resourceIndex);
FFXM_API FfxmUInt32 ffxmTestGetEffectContextFrameIndexVK(FfxmInterface* backendInterface, FfxmUInt32 effectContextId);
FFXM_API FfxmErrorCode ffxmTestSetupBackendContextVK(
    FfxmInterface* backendInterface,
    void* scratchBuffer,
    size_t scratchBufferSize,
    FfxmUInt32 maxContexts,
    FfxmUInt32* outEffectContextId);
FFXM_API void ffxmTestSetResourceStateVK(
    FfxmInterface* backendInterface,
    FfxmUInt32 resourceIndex,
    FfxmResourceStates state,
    bool undefined);
FFXM_API void ffxmTestResetBackendRefCountVK();
#endif // FFXM_ENABLE_TEST_HOOKS

#if defined(__cplusplus)
}
#endif // #if defined(__cplusplus)

} // namespace arm
