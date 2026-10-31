// Copyright  © 2024 Advanced Micro Devices, Inc.
// Copyright  © 2026 Arm Limited.
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

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cfloat>

#define FFXM_CPU
#include "ffxm_core.h"
#include "fsr1/ffxm_fsr1.h"
#include "spd/ffxm_spd.h"

#include "host/ffxm_fsr3upscaler.h"
#include "ffxm_fsr3upscaler_private.h"
#include "host/ffxm_util.h"
#include "host/ffxm_assert.h"
#include "src/shared/ffxm_object_management.h"

namespace arm
{

// Lanczos kernel generator for LUT construction
static float lanczos2(float value)
{
    return fabsf(value) < FFXM_EPSILON
        ? 1.0f
        : (sinf(FFXM_PI * value) / (FFXM_PI * value)) * (sinf(0.5f * FFXM_PI * value) / (0.5f * FFXM_PI * value));
}

// Halton sequence generator for subpixel camera jitter
static float halton(int32_t index, int32_t base)
{
    float f = 1.0f;
    float result = 0.0f;
    for (int32_t currentIndex = index; currentIndex > 0;) {
        f /= static_cast<float>(base);
        result += f * static_cast<float>(currentIndex % base);
        currentIndex = static_cast<int32_t>(floorf(static_cast<float>(currentIndex) / static_cast<float>(base)));
    }
    return result;
}

static inline float saturate(float value)
{
    return (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
}

static inline float clamp(float value, float minVal, float maxVal)
{
    return (value < minVal) ? minVal : ((value > maxVal) ? maxVal : value);
}

static inline float maxZero(float value)
{
    return (value < 0.0f) ? 0.0f : value;
}

// Inline constant-buffer staging adapter replacing upstream fpStageConstantBufferDataFunc
void stageConstantBuffer(
    FfxmConstantBuffer* dstBuffer,
    const void* srcData,
    size_t srcSizeBytes)
{
    FFXM_ASSERT(dstBuffer != nullptr);
    FFXM_ASSERT(srcData != nullptr);
    FFXM_ASSERT(srcSizeBytes <= FFXM_MAX_CONST_SIZE * sizeof(uint32_t));

    dstBuffer->num32BitEntries = static_cast<uint32_t>(srcSizeBytes / sizeof(uint32_t));
    memcpy(dstBuffer->data, srcData, srcSizeBytes);
}

// Internal resource destruction helper
FfxmErrorCode fsr3UpscalerDestroy(FfxmFsr3UpscalerContext_Private* context)
{
    if (context == nullptr) {
        return FFXM_OK;
    }

    FfxmInterface* backendInterface = &context->contextDescription.backendInterface;

    // 1. Release copy staging resources for surfaces created with initial data
    if (backendInterface->fpDestroyResource != nullptr) {
        const uint32_t copyResourceIds[] = {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_ATOMIC_COUNT,
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LANCZOS_LUT,
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY,
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_EXPOSURE
        };

        for (uint32_t id : copyResourceIds) {
            if (context->srvResources[id].internalIndex != -1) {
                ffxmSafeReleaseCopyResource(backendInterface, context->srvResources[id]);
            }
        }

        // 2. Destroy internal GPU resources in reverse order of allocation
        for (int32_t i = static_cast<int32_t>(context->allocatedResourceCount) - 1; i >= 0; --i) {
            uint32_t id = context->allocatedResourceIds[i];
            if (context->srvResources[id].internalIndex != -1) {
                backendInterface->fpDestroyResource(backendInterface, context->srvResources[id]);
                context->srvResources[id].internalIndex = -1;
                context->uavResources[id].internalIndex = -1;
                context->rtResources[id].internalIndex = -1;
            }
        }

        // Ensure any remaining tracked slots are also cleared
        for (uint32_t i = 0; i < FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_COUNT; ++i) {
            if (context->srvResources[i].internalIndex != -1) {
                backendInterface->fpDestroyResource(backendInterface, context->srvResources[i]);
                context->srvResources[i].internalIndex = -1;
                context->uavResources[i].internalIndex = -1;
                context->rtResources[i].internalIndex = -1;
            }
        }
    }
    context->allocatedResourceCount = 0;

    // 3. Destroy backend context AFTER destroying all resources
    if (context->backendContextCreated) {
        if (backendInterface->fpDestroyBackendContext != nullptr) {
            backendInterface->fpDestroyBackendContext(backendInterface, context->effectContextId);
        }
        context->backendContextCreated = false;
    }

    return FFXM_OK;
}

// Internal resource creation helper
FfxmErrorCode fsr3UpscalerCreate(
    FfxmFsr3UpscalerContext_Private* context,
    const FfxmFsr3UpscalerContextDescription* contextDescription)
{
    FFXM_ASSERT(context != nullptr);
    FFXM_ASSERT(contextDescription != nullptr);

    // Ensure all resource tracking indices are reset to -1 (unallocated)
    for (uint32_t i = 0; i < FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_COUNT; ++i) {
        context->srvResources[i].internalIndex = -1;
        context->uavResources[i].internalIndex = -1;
        context->rtResources[i].internalIndex = -1;
    }
    context->backendContextCreated = false;
    context->allocatedResourceCount = 0;

    context->device = contextDescription->backendInterface.device;
    if (context->device == nullptr) {
        return FFXM_ERROR_NULL_DEVICE;
    }

    memcpy(&context->contextDescription, contextDescription, sizeof(FfxmFsr3UpscalerContextDescription));

    // Create backend context space first so that backend state (e.g. Vulkan device extensions and physical device) is initialized
    FfxmErrorCode errorCode = context->contextDescription.backendInterface.fpCreateBackendContext(
        &context->contextDescription.backendInterface, &context->effectContextId);
    FFXM_RETURN_ON_ERROR(errorCode == FFXM_OK, errorCode);
    context->backendContextCreated = true;

    // Retrieve device capabilities from initialized backend context
    errorCode = context->contextDescription.backendInterface.fpGetDeviceCapabilities(
        &context->contextDescription.backendInterface, &context->deviceCapabilities);
    FFXM_RETURN_ON_ERROR(errorCode == FFXM_OK, errorCode);

    // Reject unsupported FP16 hardware cleanly using FFXM_ERROR_UNSUPPORTED_DEVICE
    if (!context->deviceCapabilities.fp16Supported) {
        return FFXM_ERROR_UNSUPPORTED_DEVICE;
    }

    // Setup constant buffer sizes
    context->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER].num32BitEntries =
        sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t);
    context->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD].num32BitEntries =
        sizeof(Fsr3UpscalerSpdConstants) / sizeof(uint32_t);
    context->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_RCAS].num32BitEntries =
        sizeof(Fsr3UpscalerRcasConstants) / sizeof(uint32_t);
    context->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_GENREACTIVE].num32BitEntries =
        sizeof(Fsr3UpscalerGenerateReactiveConstants) / sizeof(uint32_t);

    // Initialize constants with default parameters
    context->constants.maxRenderSize[0] = contextDescription->maxRenderSize.width;
    context->constants.maxRenderSize[1] = contextDescription->maxRenderSize.height;
    context->constants.maxUpscaleSize[0] = contextDescription->maxUpscaleSize.width;
    context->constants.maxUpscaleSize[1] = contextDescription->maxUpscaleSize.height;

    context->constants.velocityFactor = 1.0f;
    context->constants.reactivenessScale = 1.0f;
    context->constants.shadingChangeScale = 1.0f;
    context->constants.accumulationAddedPerFrame = 0.33333333f;
    context->constants.minDisocclusionAccumulation = -0.33333333f;

    context->firstExecution = true;
    context->resourceFrameIndex = 0;
    context->preExposure = 1.0f;
    context->previousFramePreExposure = 1.0f;

    // Stage initial default constants into constant buffer slot 0
    stageConstantBuffer(
        &context->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER],
        &context->constants,
        sizeof(context->constants));

    // Generate initial data for LUT and single-value surfaces
    const uint32_t lanczos2LutWidth = 128;
    int16_t lanczos2Weights[lanczos2LutWidth] = {};
    for (uint32_t currentLanczosWidthIndex = 0; currentLanczosWidthIndex < lanczos2LutWidth; currentLanczosWidthIndex++) {
        const float x = 2.0f * static_cast<float>(currentLanczosWidthIndex) / static_cast<float>(lanczos2LutWidth - 1);
        const float y = lanczos2(x);
        lanczos2Weights[currentLanczosWidthIndex] = static_cast<int16_t>(roundf(y * 32767.0f));
    }

    uint8_t defaultReactiveMaskData = 0U;
    uint32_t atomicInitData = 0U;
    float defaultExposure[2] = { 0.0f, 0.0f };

    const FfxmDimensions2D maxRenderSizeDiv2 = {
        contextDescription->maxRenderSize.width / 2,
        contextDescription->maxRenderSize.height / 2
    };

    // The complete 19 internal surfaces specified by upstream FSR 3.1.4
    const FfxmInternalResourceDescription internalSurfaceDesc[FFXM_FSR3UPSCALER_INTERNAL_SURFACE_COUNT] = {
        // 1. Accumulation 1
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1,
            L"FSR3UPSCALER_Accumulation1",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
            FFXM_SURFACE_FORMAT_R8_UNORM,
            contextDescription->maxRenderSize.width,
            contextDescription->maxRenderSize.height,
            1,
            FFXM_RESOURCE_FLAGS_NONE,
            0,
            nullptr
        },
        // 2. Accumulation 2
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2,
            L"FSR3UPSCALER_Accumulation2",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
            FFXM_SURFACE_FORMAT_R8_UNORM,
            contextDescription->maxRenderSize.width,
            contextDescription->maxRenderSize.height,
            1,
            FFXM_RESOURCE_FLAGS_NONE,
            0,
            nullptr
        },
        // 3. Luma 1
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_1,
            L"FSR3UPSCALER_Luma1",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
            FFXM_SURFACE_FORMAT_R16_FLOAT,
            contextDescription->maxRenderSize.width,
            contextDescription->maxRenderSize.height,
            1,
            FFXM_RESOURCE_FLAGS_NONE,
            0,
            nullptr
        },
        // 4. Luma 2
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_2,
            L"FSR3UPSCALER_Luma2",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
            FFXM_SURFACE_FORMAT_R16_FLOAT,
            contextDescription->maxRenderSize.width,
            contextDescription->maxRenderSize.height,
            1,
            FFXM_RESOURCE_FLAGS_NONE,
            0,
            nullptr
        },
        // 5. Intermediate FP16x1 (Aliasable)
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERMEDIATE_FP16x1,
            L"FSR3UPSCALER_IntermediateFp16x1",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
            FFXM_SURFACE_FORMAT_R16_FLOAT,
            contextDescription->maxRenderSize.width,
            contextDescription->maxRenderSize.height,
            1,
            FFXM_RESOURCE_FLAGS_ALIASABLE,
            0,
            nullptr
        },
        // 6. Shading Change (Aliasable)
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SHADING_CHANGE,
            L"FSR3UPSCALER_ShadingChange",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
            FFXM_SURFACE_FORMAT_R8_UNORM,
            maxRenderSizeDiv2.width,
            maxRenderSizeDiv2.height,
            1,
            FFXM_RESOURCE_FLAGS_ALIASABLE,
            0,
            nullptr
        },
        // 7. New Locks (Aliasable)
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS,
            L"FSR3UPSCALER_NewLocks",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            FFXM_RESOURCE_USAGE_UAV,
            FFXM_SURFACE_FORMAT_R8_UNORM,
            contextDescription->maxUpscaleSize.width,
            contextDescription->maxUpscaleSize.height,
            1,
            FFXM_RESOURCE_FLAGS_ALIASABLE,
            0,
            nullptr
        },
        // 8. Internal Upscaled Color 1
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_1,
            L"FSR3UPSCALER_InternalUpscaled1",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
            FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT,
            contextDescription->maxUpscaleSize.width,
            contextDescription->maxUpscaleSize.height,
            1,
            FFXM_RESOURCE_FLAGS_NONE,
            0,
            nullptr
        },
        // 9. Internal Upscaled Color 2
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_2,
            L"FSR3UPSCALER_InternalUpscaled2",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
            FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT,
            contextDescription->maxUpscaleSize.width,
            contextDescription->maxUpscaleSize.height,
            1,
            FFXM_RESOURCE_FLAGS_NONE,
            0,
            nullptr
        },
        // 10. SPD Mips (Aliasable, auto mip count)
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS,
            L"FSR3UPSCALER_SpdMips",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            FFXM_RESOURCE_USAGE_UAV,
            FFXM_SURFACE_FORMAT_R16G16_FLOAT,
            maxRenderSizeDiv2.width,
            maxRenderSizeDiv2.height,
            0,
            FFXM_RESOURCE_FLAGS_ALIASABLE,
            0,
            nullptr
        },
        // 11. Farthest Depth Mip 1 (Aliasable)
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH_MIP1,
            L"FSR3UPSCALER_FarthestDepthMip1",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            FFXM_RESOURCE_USAGE_UAV,
            FFXM_SURFACE_FORMAT_R16_FLOAT,
            maxRenderSizeDiv2.width,
            maxRenderSizeDiv2.height,
            1,
            FFXM_RESOURCE_FLAGS_ALIASABLE,
            0,
            nullptr
        },
        // 12. Luma History 1
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_1,
            L"FSR3UPSCALER_LumaHistory1",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
            FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT,
            contextDescription->maxRenderSize.width,
            contextDescription->maxRenderSize.height,
            1,
            FFXM_RESOURCE_FLAGS_NONE,
            0,
            nullptr
        },
        // 13. Luma History 2
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_2,
            L"FSR3UPSCALER_LumaHistory2",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
            FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT,
            contextDescription->maxRenderSize.width,
            contextDescription->maxRenderSize.height,
            1,
            FFXM_RESOURCE_FLAGS_NONE,
            0,
            nullptr
        },
        // 14. SPD Atomic Counter (Init value: 0)
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_ATOMIC_COUNT,
            L"FSR3UPSCALER_SpdAtomicCounter",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            FFXM_RESOURCE_USAGE_UAV,
            FFXM_SURFACE_FORMAT_R32_UINT,
            1,
            1,
            1,
            FFXM_RESOURCE_FLAGS_NONE,
            static_cast<uint32_t>(sizeof(atomicInitData)),
            &atomicInitData
        },
        // 15. Dilated Reactive Masks (Aliasable)
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS,
            L"FSR3UPSCALER_DilatedReactiveMasks",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV),
            FFXM_SURFACE_FORMAT_R8G8B8A8_UNORM,
            contextDescription->maxRenderSize.width,
            contextDescription->maxRenderSize.height,
            1,
            FFXM_RESOURCE_FLAGS_ALIASABLE,
            0,
            nullptr
        },
        // 16. Lanczos LUT (Init buffer: lanczos2Weights)
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LANCZOS_LUT,
            L"FSR3UPSCALER_LanczosLutData",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            FFXM_RESOURCE_USAGE_READ_ONLY,
            FFXM_SURFACE_FORMAT_R16_SNORM,
            lanczos2LutWidth,
            1,
            1,
            FFXM_RESOURCE_FLAGS_NONE,
            static_cast<uint32_t>(sizeof(lanczos2Weights)),
            lanczos2Weights
        },
        // 17. Internal Default Reactivity Mask (Init value: 0)
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY,
            L"FSR3UPSCALER_DefaultReactivityMask",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            FFXM_RESOURCE_USAGE_READ_ONLY,
            FFXM_SURFACE_FORMAT_R8_UNORM,
            1,
            1,
            1,
            FFXM_RESOURCE_FLAGS_NONE,
            static_cast<uint32_t>(sizeof(defaultReactiveMaskData)),
            &defaultReactiveMaskData
        },
        // 18. Internal Default Exposure (Init buffer: {0.0f, 0.0f})
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_EXPOSURE,
            L"FSR3UPSCALER_DefaultExposure",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            FFXM_RESOURCE_USAGE_READ_ONLY,
            FFXM_SURFACE_FORMAT_R32G32_FLOAT,
            1,
            1,
            1,
            FFXM_RESOURCE_FLAGS_NONE,
            static_cast<uint32_t>(sizeof(defaultExposure)),
            defaultExposure
        },
        // 19. Frame Info
        {
            FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FRAME_INFO,
            L"FSR3UPSCALER_FrameInfo",
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            FFXM_RESOURCE_USAGE_UAV,
            FFXM_SURFACE_FORMAT_R32G32B32A32_FLOAT,
            1,
            1,
            1,
            FFXM_RESOURCE_FLAGS_NONE,
            0,
            nullptr
        }
    };

    // Allocate all 19 internal surfaces through the backend interface
    for (uint32_t currentSurfaceIndex = 0; currentSurfaceIndex < FFXM_FSR3UPSCALER_INTERNAL_SURFACE_COUNT; ++currentSurfaceIndex) {
        const FfxmInternalResourceDescription* currentSurfaceDescription = &internalSurfaceDesc[currentSurfaceIndex];

        FfxmCreateResourceDescription createResourceDescription = {};
        createResourceDescription.heapType = FFXM_HEAP_TYPE_DEFAULT;
        createResourceDescription.resourceDescription.type = currentSurfaceDescription->type;
        createResourceDescription.resourceDescription.format = currentSurfaceDescription->format;
        createResourceDescription.resourceDescription.width = currentSurfaceDescription->width;
        createResourceDescription.resourceDescription.height = currentSurfaceDescription->height;
        createResourceDescription.resourceDescription.depth = 1;
        createResourceDescription.resourceDescription.mipCount = currentSurfaceDescription->mipCount;
        createResourceDescription.resourceDescription.flags = currentSurfaceDescription->flags;
        createResourceDescription.resourceDescription.usage = currentSurfaceDescription->usage;
        createResourceDescription.initalState = (currentSurfaceDescription->usage == FFXM_RESOURCE_USAGE_READ_ONLY)
            ? FFXM_RESOURCE_STATE_COMPUTE_READ
            : FFXM_RESOURCE_STATE_UNORDERED_ACCESS;
        createResourceDescription.initDataSize = currentSurfaceDescription->initDataSize;
        createResourceDescription.initData = currentSurfaceDescription->initData;
        createResourceDescription.name = currentSurfaceDescription->name;
        createResourceDescription.id = currentSurfaceDescription->id;

        FfxmErrorCode res = context->contextDescription.backendInterface.fpCreateResource(
            &context->contextDescription.backendInterface,
            &createResourceDescription,
            context->effectContextId,
            &context->srvResources[currentSurfaceDescription->id]);
        if (res != FFXM_OK) {
            // Partial initialization failure: return immediately to trigger transactional rollback
            return res;
        }

        context->allocatedResourceIds[context->allocatedResourceCount++] = currentSurfaceDescription->id;
    }

    // Mirror internal handles to UAV and RT binding tracking arrays
    memcpy(context->uavResources, context->srvResources, sizeof(context->srvResources));
    memcpy(context->rtResources, context->srvResources, sizeof(context->srvResources));

    return FFXM_OK;
}

// Public API Implementation

FFXM_API FfxmErrorCode ffxmFsr3UpscalerContextCreate(
    FfxmFsr3UpscalerContext* pContext,
    const FfxmFsr3UpscalerContextDescription* pContextDescription)
{
    FFXM_RETURN_ON_ERROR(pContext != nullptr, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(pContextDescription != nullptr, FFXM_ERROR_INVALID_POINTER);

    // Validate dimensions
    FFXM_RETURN_ON_ERROR(
        pContextDescription->maxRenderSize.width > 0 &&
        pContextDescription->maxRenderSize.height > 0 &&
        pContextDescription->maxUpscaleSize.width > 0 &&
        pContextDescription->maxUpscaleSize.height > 0,
        FFXM_ERROR_INVALID_ARGUMENT);
    FFXM_RETURN_ON_ERROR(
        pContextDescription->maxRenderSize.width <= pContextDescription->maxUpscaleSize.width &&
        pContextDescription->maxRenderSize.height <= pContextDescription->maxUpscaleSize.height,
        FFXM_ERROR_INVALID_ARGUMENT);

    // Validate that all required callbacks are provided by the backend interface
    FFXM_RETURN_ON_ERROR(pContextDescription->backendInterface.fpGetDeviceCapabilities != nullptr, FFXM_ERROR_INCOMPLETE_INTERFACE);
    FFXM_RETURN_ON_ERROR(pContextDescription->backendInterface.fpCreateBackendContext != nullptr, FFXM_ERROR_INCOMPLETE_INTERFACE);
    FFXM_RETURN_ON_ERROR(pContextDescription->backendInterface.fpDestroyBackendContext != nullptr, FFXM_ERROR_INCOMPLETE_INTERFACE);
    FFXM_RETURN_ON_ERROR(pContextDescription->backendInterface.fpCreateResource != nullptr, FFXM_ERROR_INCOMPLETE_INTERFACE);
    FFXM_RETURN_ON_ERROR(pContextDescription->backendInterface.fpDestroyResource != nullptr, FFXM_ERROR_INCOMPLETE_INTERFACE);
    FFXM_RETURN_ON_ERROR(pContextDescription->backendInterface.fpRegisterResource != nullptr, FFXM_ERROR_INCOMPLETE_INTERFACE);
    FFXM_RETURN_ON_ERROR(pContextDescription->backendInterface.fpGetResource != nullptr, FFXM_ERROR_INCOMPLETE_INTERFACE);
    FFXM_RETURN_ON_ERROR(pContextDescription->backendInterface.fpUnregisterResources != nullptr, FFXM_ERROR_INCOMPLETE_INTERFACE);
    FFXM_RETURN_ON_ERROR(pContextDescription->backendInterface.fpGetResourceDescription != nullptr, FFXM_ERROR_INCOMPLETE_INTERFACE);
    FFXM_RETURN_ON_ERROR(pContextDescription->backendInterface.fpCreateComputePipeline != nullptr, FFXM_ERROR_INCOMPLETE_INTERFACE);
    FFXM_RETURN_ON_ERROR(pContextDescription->backendInterface.fpDestroyPipeline != nullptr, FFXM_ERROR_INCOMPLETE_INTERFACE);
    FFXM_RETURN_ON_ERROR(pContextDescription->backendInterface.fpScheduleGpuJob != nullptr, FFXM_ERROR_INCOMPLETE_INTERFACE);
    FFXM_RETURN_ON_ERROR(pContextDescription->backendInterface.fpExecuteGpuJobs != nullptr, FFXM_ERROR_INCOMPLETE_INTERFACE);

    if (pContextDescription->backendInterface.scratchBuffer != nullptr) {
        FFXM_RETURN_ON_ERROR(pContextDescription->backendInterface.scratchBufferSize > 0, FFXM_ERROR_INCOMPLETE_INTERFACE);
    }

    // Zero context memory before creation
    memset(pContext, 0, sizeof(FfxmFsr3UpscalerContext));

    FfxmFsr3UpscalerContext_Private* contextPrivate = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(pContext);

    // Explicitly initialize all resource slots to -1 (unallocated sentinel)
    for (uint32_t i = 0; i < FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_COUNT; ++i) {
        contextPrivate->srvResources[i].internalIndex = -1;
        contextPrivate->uavResources[i].internalIndex = -1;
        contextPrivate->rtResources[i].internalIndex = -1;
    }
    contextPrivate->backendContextCreated = false;
    contextPrivate->allocatedResourceCount = 0;

    const FfxmErrorCode errorCode = fsr3UpscalerCreate(contextPrivate, pContextDescription);
    if (errorCode != FFXM_OK) {
        // Transactional rollback on failure: cleans up only what was acquired
        fsr3UpscalerDestroy(contextPrivate);
        memset(pContext, 0, sizeof(FfxmFsr3UpscalerContext));
        return errorCode;
    }

    return FFXM_OK;
}

FFXM_API FfxmErrorCode ffxmFsr3UpscalerGetSharedResourceDescriptions(
    FfxmFsr3UpscalerContext* pContext,
    FfxmFsr3UpscalerSharedResourceDescriptions* pSharedResources)
{
    FFXM_RETURN_ON_ERROR(pContext != nullptr, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(pSharedResources != nullptr, FFXM_ERROR_INVALID_POINTER);

    FfxmFsr3UpscalerContext_Private* contextPrivate = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(pContext);

    pSharedResources->dilatedDepth = {
        FFXM_HEAP_TYPE_DEFAULT,
        {
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            FFXM_SURFACE_FORMAT_R32_FLOAT,
            contextPrivate->contextDescription.maxRenderSize.width,
            contextPrivate->contextDescription.maxRenderSize.height,
            1,
            1,
            FFXM_RESOURCE_FLAGS_NONE,
            static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV)
        },
        FFXM_RESOURCE_STATE_UNORDERED_ACCESS,
        0,
        nullptr,
        L"FSR3UPSCALER_DilatedDepth",
        FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_DEPTH
    };

    pSharedResources->dilatedMotionVectors = {
        FFXM_HEAP_TYPE_DEFAULT,
        {
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            FFXM_SURFACE_FORMAT_R16G16_FLOAT,
            contextPrivate->contextDescription.maxRenderSize.width,
            contextPrivate->contextDescription.maxRenderSize.height,
            1,
            1,
            FFXM_RESOURCE_FLAGS_NONE,
            static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_RENDERTARGET | FFXM_RESOURCE_USAGE_UAV)
        },
        FFXM_RESOURCE_STATE_UNORDERED_ACCESS,
        0,
        nullptr,
        L"FSR3UPSCALER_DilatedVelocity",
        FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS
    };

    pSharedResources->reconstructedPrevNearestDepth = {
        FFXM_HEAP_TYPE_DEFAULT,
        {
            FFXM_RESOURCE_TYPE_TEXTURE2D,
            FFXM_SURFACE_FORMAT_R32_UINT,
            contextPrivate->contextDescription.maxRenderSize.width,
            contextPrivate->contextDescription.maxRenderSize.height,
            1,
            1,
            FFXM_RESOURCE_FLAGS_NONE,
            static_cast<FfxmResourceUsage>(FFXM_RESOURCE_USAGE_UAV)
        },
        FFXM_RESOURCE_STATE_UNORDERED_ACCESS,
        0,
        nullptr,
        L"FSR3UPSCALER_ReconstructedPrevNearestDepth",
        FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH
    };

    return FFXM_OK;
}

FFXM_API FfxmErrorCode ffxmFsr3UpscalerContextDispatch(
    FfxmFsr3UpscalerContext* pContext,
    const FfxmFsr3UpscalerDispatchDescription* pDispatchDescription)
{
    FFXM_RETURN_ON_ERROR(pContext != nullptr, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(pDispatchDescription != nullptr, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(pDispatchDescription->commandList != nullptr, FFXM_ERROR_INVALID_POINTER);

    FfxmFsr3UpscalerContext_Private* contextPrivate = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(pContext);

    FFXM_RETURN_ON_ERROR(contextPrivate->device != nullptr, FFXM_ERROR_NULL_DEVICE);

    FFXM_RETURN_ON_ERROR(
        pDispatchDescription->renderSize.width <= contextPrivate->contextDescription.maxRenderSize.width,
        FFXM_ERROR_OUT_OF_RANGE);
    FFXM_RETURN_ON_ERROR(
        pDispatchDescription->renderSize.height <= contextPrivate->contextDescription.maxRenderSize.height,
        FFXM_ERROR_OUT_OF_RANGE);
    FFXM_RETURN_ON_ERROR(
        pDispatchDescription->upscaleSize.width <= contextPrivate->contextDescription.maxUpscaleSize.width,
        FFXM_ERROR_OUT_OF_RANGE);
    FFXM_RETURN_ON_ERROR(
        pDispatchDescription->upscaleSize.height <= contextPrivate->contextDescription.maxUpscaleSize.height,
        FFXM_ERROR_OUT_OF_RANGE);

    // Update frame constants
    contextPrivate->constants.renderSize[0] = pDispatchDescription->renderSize.width;
    contextPrivate->constants.renderSize[1] = pDispatchDescription->renderSize.height;
    contextPrivate->constants.upscaleSize[0] = pDispatchDescription->upscaleSize.width;
    contextPrivate->constants.upscaleSize[1] = pDispatchDescription->upscaleSize.height;

    contextPrivate->constants.jitterOffset[0] = pDispatchDescription->jitterOffset.x;
    contextPrivate->constants.jitterOffset[1] = pDispatchDescription->jitterOffset.y;
    contextPrivate->constants.motionVectorScale[0] = pDispatchDescription->motionVectorScale.x;
    contextPrivate->constants.motionVectorScale[1] = pDispatchDescription->motionVectorScale.y;

    contextPrivate->constants.deltaTime = pDispatchDescription->frameTimeDelta;
    contextPrivate->constants.tanHalfFOV = tanf(pDispatchDescription->cameraFovAngleVertical * 0.5f);
    contextPrivate->constants.viewSpaceToMetersFactor = pDispatchDescription->viewSpaceToMetersFactor;

    // Stage updated constants into constant buffer slot 0 via adapter
    stageConstantBuffer(
        &contextPrivate->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER],
        &contextPrivate->constants,
        sizeof(contextPrivate->constants));

    // Auto exposure SPD constants setup
    uint32_t dispatchThreadGroupCountXY[2];
    uint32_t workGroupOffset[2];
    uint32_t numWorkGroupsAndMips[2];
    uint32_t rectInfo[4] = { 0, 0, pDispatchDescription->renderSize.width, pDispatchDescription->renderSize.height };
    ffxSpdSetup(dispatchThreadGroupCountXY, workGroupOffset, numWorkGroupsAndMips, rectInfo);

    // Downsample SPD pyramid constants
    Fsr3UpscalerSpdConstants luminancePyramidConstants;
    luminancePyramidConstants.mips = numWorkGroupsAndMips[1];
    luminancePyramidConstants.numWorkGroups = numWorkGroupsAndMips[0];
    luminancePyramidConstants.workGroupOffset[0] = workGroupOffset[0];
    luminancePyramidConstants.workGroupOffset[1] = workGroupOffset[1];
    luminancePyramidConstants.renderSize[0] = pDispatchDescription->renderSize.width;
    luminancePyramidConstants.renderSize[1] = pDispatchDescription->renderSize.height;

    // RCAS sharpening constants
    Fsr3UpscalerRcasConstants rcasConsts = {};
    const float sharpnessRemapped = (-2.0f * pDispatchDescription->sharpness) + 2.0f;
    FsrRcasCon(rcasConsts.rcasConfig, sharpnessRemapped);

    // Stage SPD (slot 1) and RCAS (slot 2) constant buffers
    stageConstantBuffer(
        &contextPrivate->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD],
        &luminancePyramidConstants,
        sizeof(luminancePyramidConstants));
    stageConstantBuffer(
        &contextPrivate->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_RCAS],
        &rcasConsts,
        sizeof(rcasConsts));

    // Milestone 2 boundary: GPU compute dispatch passes are scheduled in Milestone 3.
    // Return FFXM_ERROR_INCOMPLETE_INTERFACE to prevent reporting execution before GPU jobs are scheduled.
    return FFXM_ERROR_INCOMPLETE_INTERFACE;
}

FFXM_API FfxmErrorCode ffxmFsr3UpscalerContextGenerateReactiveMask(
    FfxmFsr3UpscalerContext* pContext,
    const FfxmFsr3UpscalerGenerateReactiveDescription* pParams)
{
    FFXM_RETURN_ON_ERROR(pContext != nullptr, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(pParams != nullptr, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(pParams->commandList != nullptr, FFXM_ERROR_INVALID_POINTER);

    FfxmFsr3UpscalerContext_Private* contextPrivate = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(pContext);

    FFXM_RETURN_ON_ERROR(contextPrivate->device != nullptr, FFXM_ERROR_NULL_DEVICE);

    // Stage GenerateReactive parameters into dedicated constant buffer slot 3
    Fsr3UpscalerGenerateReactiveConstants genReactiveConsts = {};
    genReactiveConsts.scale = pParams->scale;
    genReactiveConsts.cutoffThreshold = pParams->cutoffThreshold;
    genReactiveConsts.binaryValue = pParams->binaryValue;
    genReactiveConsts.flags = pParams->flags;

    stageConstantBuffer(
        &contextPrivate->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_GENREACTIVE],
        &genReactiveConsts,
        sizeof(genReactiveConsts));

    // Milestone 2 boundary: GPU reactive mask generation pass is scheduled in Milestone 3.
    // Return FFXM_ERROR_INCOMPLETE_INTERFACE to prevent reporting execution before GPU jobs are scheduled.
    return FFXM_ERROR_INCOMPLETE_INTERFACE;
}

FFXM_API FfxmErrorCode ffxmFsr3UpscalerContextDestroy(
    FfxmFsr3UpscalerContext* pContext)
{
    FFXM_RETURN_ON_ERROR(pContext != nullptr, FFXM_ERROR_INVALID_POINTER);

    FfxmFsr3UpscalerContext_Private* contextPrivate = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(pContext);
    const FfxmErrorCode errorCode = fsr3UpscalerDestroy(contextPrivate);
    memset(pContext, 0, sizeof(FfxmFsr3UpscalerContext));

    return errorCode;
}

FFXM_API float ffxmFsr3UpscalerGetUpscaleRatioFromQualityMode(
    FfxmFsr3UpscalerQualityMode qualityMode)
{
    switch (qualityMode) {
    case FFXM_FSR3UPSCALER_QUALITY_MODE_NATIVEAA:
        return 1.0f;
    case FFXM_FSR3UPSCALER_QUALITY_MODE_QUALITY:
        return 1.5f;
    case FFXM_FSR3UPSCALER_QUALITY_MODE_BALANCED:
        return 1.7f;
    case FFXM_FSR3UPSCALER_QUALITY_MODE_PERFORMANCE:
        return 2.0f;
    case FFXM_FSR3UPSCALER_QUALITY_MODE_ULTRA_PERFORMANCE:
        return 3.0f;
    default:
        return 0.0f;
    }
}

FFXM_API FfxmErrorCode ffxmFsr3UpscalerGetRenderResolutionFromQualityMode(
    uint32_t* pRenderWidth,
    uint32_t* pRenderHeight,
    uint32_t displayWidth,
    uint32_t displayHeight,
    FfxmFsr3UpscalerQualityMode qualityMode)
{
    FFXM_RETURN_ON_ERROR(pRenderWidth != nullptr, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(pRenderHeight != nullptr, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(
        qualityMode <= FFXM_FSR3UPSCALER_QUALITY_MODE_ULTRA_PERFORMANCE,
        FFXM_ERROR_INVALID_ENUM);

    const float upscaleRatio = ffxmFsr3UpscalerGetUpscaleRatioFromQualityMode(qualityMode);
    FFXM_RETURN_ON_ERROR(upscaleRatio > 0.0f, FFXM_ERROR_INVALID_ENUM);

    *pRenderWidth = static_cast<uint32_t>(roundf(static_cast<float>(displayWidth) / upscaleRatio));
    *pRenderHeight = static_cast<uint32_t>(roundf(static_cast<float>(displayHeight) / upscaleRatio));

    return FFXM_OK;
}

FFXM_API int32_t ffxmFsr3UpscalerGetJitterPhaseCount(
    int32_t renderWidth,
    int32_t displayWidth)
{
    const float basePhaseCount = 8.0f;
    const int32_t jitterPhaseCount = static_cast<int32_t>(
        ceilf(basePhaseCount * powf(static_cast<float>(displayWidth) / static_cast<float>(renderWidth), 2.0f)));
    return jitterPhaseCount;
}

FFXM_API FfxmErrorCode ffxmFsr3UpscalerGetJitterOffset(
    float* pOutX,
    float* pOutY,
    int32_t index,
    int32_t phaseCount)
{
    FFXM_RETURN_ON_ERROR(pOutX != nullptr, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(pOutY != nullptr, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(phaseCount > 0, FFXM_ERROR_INVALID_ARGUMENT);

    *pOutX = halton((index % phaseCount) + 1, 2) - 0.5f;
    *pOutY = halton((index % phaseCount) + 1, 3) - 0.5f;

    return FFXM_OK;
}

FFXM_API bool ffxmFsr3UpscalerResourceIsNull(
    FfxmResource resource)
{
    return resource.resource == nullptr;
}

FFXM_API FfxmVersionNumber ffxmFsr3UpscalerGetEffectVersion(void)
{
    return FFXM_SDK_MAKE_VERSION(
        FFXM_FSR3UPSCALER_VERSION_MAJOR,
        FFXM_FSR3UPSCALER_VERSION_MINOR,
        FFXM_FSR3UPSCALER_VERSION_PATCH);
}

FFXM_API FfxmErrorCode ffxmFsr3UpscalerSetConstant(
    FfxmFsr3UpscalerContext* pContext,
    FfxmFsr3UpscalerConfigureKey key,
    void* valuePtr)
{
    FFXM_RETURN_ON_ERROR(pContext != nullptr, FFXM_ERROR_INVALID_POINTER);

    FfxmFsr3UpscalerContext_Private* contextPrivate = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(pContext);

    switch (key) {
    case FFXM_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR: {
        float val = 1.0f;
        if (valuePtr != nullptr) {
            val = *static_cast<float*>(valuePtr);
        }
        contextPrivate->constants.velocityFactor = saturate(val);
        break;
    }
    case FFXM_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FREACTIVENESSSCALE: {
        float val = 1.0f;
        if (valuePtr != nullptr) {
            val = *static_cast<float*>(valuePtr);
        }
        contextPrivate->constants.reactivenessScale = maxZero(val);
        break;
    }
    case FFXM_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FSHADINGCHANGESCALE: {
        float val = 1.0f;
        if (valuePtr != nullptr) {
            val = *static_cast<float*>(valuePtr);
        }
        contextPrivate->constants.shadingChangeScale = maxZero(val);
        break;
    }
    case FFXM_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FACCUMULATIONADDEDPERFRAME: {
        float val = 1.0f / 3.0f;
        if (valuePtr != nullptr) {
            val = *static_cast<float*>(valuePtr);
        }
        contextPrivate->constants.accumulationAddedPerFrame = saturate(val);
        break;
    }
    case FFXM_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FMINDISOCCLUSIONACCUMULATION: {
        float val = -1.0f / 3.0f;
        if (valuePtr != nullptr) {
            val = *static_cast<float*>(valuePtr);
        }
        contextPrivate->constants.minDisocclusionAccumulation = clamp(val, -1.0f, 1.0f);
        break;
    }
    default:
        return FFXM_ERROR_INVALID_ENUM;
    }

    // Re-stage constant buffer 0 after configuration update
    stageConstantBuffer(
        &contextPrivate->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER],
        &contextPrivate->constants,
        sizeof(contextPrivate->constants));

    return FFXM_OK;
}

} // namespace arm
