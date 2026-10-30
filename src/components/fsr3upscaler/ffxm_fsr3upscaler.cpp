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

#include "host/ffxm_fsr3upscaler.h"
#include "ffxm_fsr3upscaler_private.h"
#include "host/ffxm_util.h"
#include "host/ffxm_assert.h"

namespace arm
{

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
static inline void stageConstantBuffer(
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

// Internal creation function
FfxmErrorCode fsr3UpscalerCreate(
    FfxmFsr3UpscalerContext_Private* context,
    const FfxmFsr3UpscalerContextDescription* contextDescription)
{
    FFXM_ASSERT(context != nullptr);
    FFXM_ASSERT(contextDescription != nullptr);

    // Setup the data for implementation
    memset(context, 0, sizeof(FfxmFsr3UpscalerContext_Private));
    context->device = contextDescription->backendInterface.device;
    memcpy(&context->contextDescription, contextDescription, sizeof(FfxmFsr3UpscalerContextDescription));

    // Retrieve device capabilities first
    FfxmErrorCode errorCode = context->contextDescription.backendInterface.fpGetDeviceCapabilities(
        &context->contextDescription.backendInterface, &context->deviceCapabilities);
    FFXM_RETURN_ON_ERROR(errorCode == FFXM_OK, errorCode);

    // Reject unsupported FP16 hardware cleanly
    if (!context->deviceCapabilities.fp16Supported) {
        return FFXM_ERROR_BACKEND_API_ERROR;
    }

    // Create backend context space
    errorCode = context->contextDescription.backendInterface.fpCreateBackendContext(
        &context->contextDescription.backendInterface, &context->effectContextId);
    FFXM_RETURN_ON_ERROR(errorCode == FFXM_OK, errorCode);

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
    context->constants.accumulationAddedPerFrame = 0.333f;
    context->constants.minDisocclusionAccumulation = -0.333f;

    context->firstExecution = true;
    context->resourceFrameIndex = 0;
    context->preExposure = 1.0f;
    context->previousFramePreExposure = 1.0f;

    // Stage initial default constants into constant buffer slot 0
    stageConstantBuffer(
        &context->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER],
        &context->constants,
        sizeof(context->constants));

    return FFXM_OK;
}

// Internal destruction function
FfxmErrorCode fsr3UpscalerDestroy(FfxmFsr3UpscalerContext_Private* context)
{
    FFXM_ASSERT(context != nullptr);

    // Destroy any allocated internal resources
    if (context->contextDescription.backendInterface.fpDestroyResource) {
        for (uint32_t i = 0; i < FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_COUNT; ++i) {
            if (context->srvResources[i].internalIndex != 0) {
                context->contextDescription.backendInterface.fpDestroyResource(
                    &context->contextDescription.backendInterface, context->srvResources[i]);
                context->srvResources[i].internalIndex = 0;
            }
        }
    }

    // Destroy backend context
    if (context->contextDescription.backendInterface.fpDestroyBackendContext) {
        context->contextDescription.backendInterface.fpDestroyBackendContext(
            &context->contextDescription.backendInterface, context->effectContextId);
    }

    return FFXM_OK;
}

// Public API Implementation

FFXM_API FfxmErrorCode ffxmFsr3UpscalerContextCreate(
    FfxmFsr3UpscalerContext* pContext,
    const FfxmFsr3UpscalerContextDescription* pContextDescription)
{
    FFXM_RETURN_ON_ERROR(pContext != nullptr, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(pContextDescription != nullptr, FFXM_ERROR_INVALID_POINTER);

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
    const FfxmErrorCode errorCode = fsr3UpscalerCreate(contextPrivate, pContextDescription);
    if (errorCode != FFXM_OK) {
        // Transactional rollback on failure
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

    FfxmFsr3UpscalerContext_Private* contextPrivate = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(pContext);

    FFXM_RETURN_ON_ERROR(
        pDispatchDescription->renderSize.width <= contextPrivate->contextDescription.maxRenderSize.width,
        FFXM_ERROR_OUT_OF_RANGE);
    FFXM_RETURN_ON_ERROR(
        pDispatchDescription->renderSize.height <= contextPrivate->contextDescription.maxRenderSize.height,
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

    return FFXM_OK;
}

FFXM_API FfxmErrorCode ffxmFsr3UpscalerContextGenerateReactiveMask(
    FfxmFsr3UpscalerContext* pContext,
    const FfxmFsr3UpscalerGenerateReactiveDescription* pParams)
{
    FFXM_RETURN_ON_ERROR(pContext != nullptr, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(pParams != nullptr, FFXM_ERROR_INVALID_POINTER);

    FfxmFsr3UpscalerContext_Private* contextPrivate = reinterpret_cast<FfxmFsr3UpscalerContext_Private*>(pContext);

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

    return FFXM_OK;
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
