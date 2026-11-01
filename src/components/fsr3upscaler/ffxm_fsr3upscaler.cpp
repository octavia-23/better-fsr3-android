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
#include <cwchar>

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

#ifndef FFXM_COUNTOF
#define FFXM_COUNTOF(ARRAY) (sizeof(ARRAY) / sizeof(ARRAY[0]))
#endif

// Lists to map shader resource bindpoint names to internal resource identifiers
typedef struct ResourceBinding
{
    uint32_t index;
    wchar_t  name[64];
} ResourceBinding;

static const ResourceBinding srvTextureBindingTable[] =
{
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_COLOR,                              L"r_input_color_jittered" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_OPAQUE_ONLY,                        L"r_input_opaque_only" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_MOTION_VECTORS,                     L"r_input_motion_vectors" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_DEPTH,                              L"r_input_depth" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_EXPOSURE,                           L"r_input_exposure" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FRAME_INFO,                               L"r_frame_info" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_REACTIVE_MASK,                      L"r_reactive_mask" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_TRANSPARENCY_AND_COMPOSITION_MASK,  L"r_transparency_and_composition_mask" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH,     L"r_reconstructed_previous_nearest_depth" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS,                   L"r_dilated_motion_vectors" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_DEPTH,                            L"r_dilated_depth" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR,                  L"r_internal_upscaled_color" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION,                             L"r_accumulation" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY,                             L"r_luma_history" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_RCAS_INPUT,                               L"r_rcas_input" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LANCZOS_LUT,                              L"r_lanczos_lut" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS,                                 L"r_spd_mips" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS,                   L"r_dilated_reactive_masks" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS,                                L"r_new_locks" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH,                           L"r_farthest_depth" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH_MIP1,                      L"r_farthest_depth_mip1" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SHADING_CHANGE,                           L"r_shading_change" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_CURRENT_LUMA,                             L"r_current_luma" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_PREVIOUS_LUMA,                            L"r_previous_luma" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_INSTABILITY,                         L"r_luma_instability" },
};

static const ResourceBinding uavTextureBindingTable[] =
{
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH,     L"rw_reconstructed_previous_nearest_depth" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS,                   L"rw_dilated_motion_vectors" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_DEPTH,                            L"rw_dilated_depth" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR,                  L"rw_internal_upscaled_color" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION,                             L"rw_accumulation" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY,                             L"rw_luma_history" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT,                          L"rw_upscaled_output" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS,                   L"rw_dilated_reactive_masks" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FRAME_INFO,                               L"rw_frame_info" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_ATOMIC_COUNT,                         L"rw_spd_global_atomic" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS,                                L"rw_new_locks" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_AUTOREACTIVE,                             L"rw_output_autoreactive" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SHADING_CHANGE,                           L"rw_shading_change" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH,                           L"rw_farthest_depth" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH_MIP1,                      L"rw_farthest_depth_mip1" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_CURRENT_LUMA,                             L"rw_current_luma" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_INSTABILITY,                         L"rw_luma_instability" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_0,                         L"rw_spd_mip0" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_1,                         L"rw_spd_mip1" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_2,                         L"rw_spd_mip2" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_3,                         L"rw_spd_mip3" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_4,                         L"rw_spd_mip4" },
    { FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_5,                         L"rw_spd_mip5" },
};

static const ResourceBinding constantBufferBindingTable[] =
{
    { FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER, L"cbFSR3UPSCALER" },
    { FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER, L"cbFSR3Upscaler" },
    { FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD,          L"cbSPD" },
    { FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_RCAS,         L"cbRCAS" },
    { FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_GENREACTIVE,  L"cbGenerateReactive" },
};

static FfxmErrorCode patchResourceBindings(FfxmPipelineState* inoutPipeline)
{
    for (uint32_t srvIndex = 0; srvIndex < inoutPipeline->srvTextureCount; ++srvIndex) {
        int32_t mapIndex = 0;
        for (mapIndex = 0; mapIndex < static_cast<int32_t>(FFXM_COUNTOF(srvTextureBindingTable)); ++mapIndex) {
            if (0 == wcscmp(srvTextureBindingTable[mapIndex].name, inoutPipeline->srvTextureBindings[srvIndex].name))
                break;
        }
        if (mapIndex == static_cast<int32_t>(FFXM_COUNTOF(srvTextureBindingTable)))
            return FFXM_ERROR_INVALID_ARGUMENT;

        inoutPipeline->srvTextureBindings[srvIndex].resourceIdentifier = srvTextureBindingTable[mapIndex].index;
    }

    for (uint32_t uavIndex = 0; uavIndex < inoutPipeline->uavTextureCount; ++uavIndex) {
        int32_t mapIndex = 0;
        for (mapIndex = 0; mapIndex < static_cast<int32_t>(FFXM_COUNTOF(uavTextureBindingTable)); ++mapIndex) {
            if (0 == wcscmp(uavTextureBindingTable[mapIndex].name, inoutPipeline->uavTextureBindings[uavIndex].name))
                break;
        }
        if (mapIndex == static_cast<int32_t>(FFXM_COUNTOF(uavTextureBindingTable)))
            return FFXM_ERROR_INVALID_ARGUMENT;

        inoutPipeline->uavTextureBindings[uavIndex].resourceIdentifier = uavTextureBindingTable[mapIndex].index;
    }

    for (uint32_t cbIndex = 0; cbIndex < inoutPipeline->constCount; ++cbIndex) {
        int32_t mapIndex = 0;
        for (mapIndex = 0; mapIndex < static_cast<int32_t>(FFXM_COUNTOF(constantBufferBindingTable)); ++mapIndex) {
            if (0 == wcscmp(constantBufferBindingTable[mapIndex].name, inoutPipeline->constantBufferBindings[cbIndex].name))
                break;
        }
        if (mapIndex == static_cast<int32_t>(FFXM_COUNTOF(constantBufferBindingTable)))
            return FFXM_ERROR_INVALID_ARGUMENT;

        inoutPipeline->constantBufferBindings[cbIndex].resourceIdentifier = constantBufferBindingTable[mapIndex].index;
    }

    return FFXM_OK;
}

static uint32_t getPipelinePermutationFlags(
    uint32_t contextFlags,
    FfxmFsr3UpscalerPass passId,
    bool fp16,
    bool force64,
    bool useLut)
{
    uint32_t flags = 0;
    flags |= (contextFlags & FFXM_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE) ? FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT : 0;
    flags |= (contextFlags & FFXM_FSR3UPSCALER_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) ? 0 : FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS;
    flags |= (contextFlags & FFXM_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) ? FSR3UPSCALER_SHADER_PERMUTATION_JITTER_MOTION_VECTORS : 0;
    flags |= (contextFlags & FFXM_FSR3UPSCALER_ENABLE_DEPTH_INVERTED) ? FSR3UPSCALER_SHADER_PERMUTATION_DEPTH_INVERTED : 0;
    flags |= (passId == FFXM_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN) ? FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING : 0;
    flags |= (useLut) ? FSR3UPSCALER_SHADER_PERMUTATION_USE_LANCZOS_TYPE : 0;
    flags |= (force64) ? FSR3UPSCALER_SHADER_PERMUTATION_FORCE_WAVE64 : 0;
    flags |= (fp16 && (passId != FFXM_FSR3UPSCALER_PASS_RCAS)) ? FSR3UPSCALER_SHADER_PERMUTATION_ALLOW_FP16 : 0;
    return flags;
}

static void setupDeviceDepthToViewSpaceDepthParams(
    Fsr3UpscalerConstants* constants,
    const FfxmFsr3UpscalerContextDescription* contextDescription,
    const FfxmFsr3UpscalerDispatchDescription* params)
{
    const bool bInverted = (contextDescription->flags & FFXM_FSR3UPSCALER_ENABLE_DEPTH_INVERTED) == FFXM_FSR3UPSCALER_ENABLE_DEPTH_INVERTED;
    const bool bInfinite = (contextDescription->flags & FFXM_FSR3UPSCALER_ENABLE_DEPTH_INFINITE) == FFXM_FSR3UPSCALER_ENABLE_DEPTH_INFINITE;

    float fMin = std::min(params->cameraNear, params->cameraFar);
    float fMax = std::max(params->cameraNear, params->cameraFar);

    if (bInverted) {
        float tmp = fMin;
        fMin = fMax;
        fMax = tmp;
    }

    const float fQ = (fabsf(fMin - fMax) > 1e-7f) ? (fMax / (fMin - fMax)) : 0.0f;
    const float d = -1.0f;

    const float matrix_elem_c[2][2] = {
        { fQ, -1.0f - FLT_EPSILON },
        { fQ, 0.0f + FLT_EPSILON }
    };

    const float matrix_elem_e[2][2] = {
        { fQ * fMin, -fMin - FLT_EPSILON },
        { fQ * fMin, fMax }
    };

    constants->deviceToViewDepth[0] = d * matrix_elem_c[bInverted ? 1 : 0][bInfinite ? 1 : 0];
    constants->deviceToViewDepth[1] = matrix_elem_e[bInverted ? 1 : 0][bInfinite ? 1 : 0];

    const float aspect = (params->renderSize.height > 0)
        ? (static_cast<float>(params->renderSize.width) / static_cast<float>(params->renderSize.height))
        : 1.0f;
    const float halfFov = 0.5f * params->cameraFovAngleVertical;
    const float sinHalfFov = sinf(halfFov);
    const float cotHalfFovY = (fabsf(sinHalfFov) > 1e-7f) ? (cosf(halfFov) / sinHalfFov) : 1.0f;
    const float a = (aspect > 0.0f) ? (cotHalfFovY / aspect) : cotHalfFovY;
    const float b = cotHalfFovY;

    constants->deviceToViewDepth[2] = (fabsf(a) > 1e-7f) ? (1.0f / a) : 0.0f;
    constants->deviceToViewDepth[3] = (fabsf(b) > 1e-7f) ? (1.0f / b) : 0.0f;
}

static FfxmErrorCode scheduleDispatch(
    FfxmFsr3UpscalerContext_Private* context,
    const FfxmFsr3UpscalerDispatchDescription* params,
    const FfxmPipelineState* pipeline,
    uint32_t dispatchX,
    uint32_t dispatchY)
{
    (void)params;
    FfxmGpuJobDescription dispatchJob = { FFXM_GPU_JOB_COMPUTE };

    for (uint32_t currentShaderResourceViewIndex = 0; currentShaderResourceViewIndex < pipeline->srvTextureCount; ++currentShaderResourceViewIndex) {
        const uint32_t currentResourceId = pipeline->srvTextureBindings[currentShaderResourceViewIndex].resourceIdentifier;
        const FfxmResourceInternal currentResource = context->srvResources[currentResourceId];
        dispatchJob.computeJobDescriptor.srvTextures[currentShaderResourceViewIndex] = currentResource;
        wcscpy(dispatchJob.computeJobDescriptor.srvTextureNames[currentShaderResourceViewIndex],
               pipeline->srvTextureBindings[currentShaderResourceViewIndex].name);
    }

    for (uint32_t currentUnorderedAccessViewIndex = 0; currentUnorderedAccessViewIndex < pipeline->uavTextureCount; ++currentUnorderedAccessViewIndex) {
        const uint32_t currentResourceId = pipeline->uavTextureBindings[currentUnorderedAccessViewIndex].resourceIdentifier;
        wcscpy(dispatchJob.computeJobDescriptor.uavTextureNames[currentUnorderedAccessViewIndex],
               pipeline->uavTextureBindings[currentUnorderedAccessViewIndex].name);

        if (currentResourceId >= FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_0 &&
            currentResourceId <= FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_5) {
            const FfxmResourceInternal currentResource = context->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS];
            dispatchJob.computeJobDescriptor.uavTextures[currentUnorderedAccessViewIndex] = currentResource;
            dispatchJob.computeJobDescriptor.uavTextureMips[currentUnorderedAccessViewIndex] =
                currentResourceId - FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS_LEVEL_0;
        } else {
            const FfxmResourceInternal currentResource = context->uavResources[currentResourceId];
            dispatchJob.computeJobDescriptor.uavTextures[currentUnorderedAccessViewIndex] = currentResource;
            dispatchJob.computeJobDescriptor.uavTextureMips[currentUnorderedAccessViewIndex] = 0;
        }
    }

    dispatchJob.computeJobDescriptor.dimensions[0] = dispatchX;
    dispatchJob.computeJobDescriptor.dimensions[1] = dispatchY;
    dispatchJob.computeJobDescriptor.dimensions[2] = 1;
    dispatchJob.computeJobDescriptor.pipeline = *pipeline;

    for (uint32_t currentRootConstantIndex = 0; currentRootConstantIndex < pipeline->constCount; ++currentRootConstantIndex) {
        wcscpy(dispatchJob.computeJobDescriptor.cbNames[currentRootConstantIndex],
               pipeline->constantBufferBindings[currentRootConstantIndex].name);
        dispatchJob.computeJobDescriptor.cbs[currentRootConstantIndex] =
            context->constantBuffers[pipeline->constantBufferBindings[currentRootConstantIndex].resourceIdentifier];
    }

    return context->contextDescription.backendInterface.fpScheduleGpuJob(
        &context->contextDescription.backendInterface, &dispatchJob);
}

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

    // 0. Destroy compute pipelines if created
    if (context->pipelinePrepareInputs.pipeline != nullptr && backendInterface->fpDestroyPipeline != nullptr) {
        backendInterface->fpDestroyPipeline(backendInterface, &context->pipelinePrepareInputs, context->effectContextId);
        memset(&context->pipelinePrepareInputs, 0, sizeof(context->pipelinePrepareInputs));
    }
    if (context->pipelineLumaPyramid.pipeline != nullptr && backendInterface->fpDestroyPipeline != nullptr) {
        backendInterface->fpDestroyPipeline(backendInterface, &context->pipelineLumaPyramid, context->effectContextId);
        memset(&context->pipelineLumaPyramid, 0, sizeof(context->pipelineLumaPyramid));
    }
    if (context->pipelineShadingChangePyramid.pipeline != nullptr && backendInterface->fpDestroyPipeline != nullptr) {
        backendInterface->fpDestroyPipeline(backendInterface, &context->pipelineShadingChangePyramid, context->effectContextId);
        memset(&context->pipelineShadingChangePyramid, 0, sizeof(context->pipelineShadingChangePyramid));
    }
    if (context->pipelineShadingChange.pipeline != nullptr && backendInterface->fpDestroyPipeline != nullptr) {
        backendInterface->fpDestroyPipeline(backendInterface, &context->pipelineShadingChange, context->effectContextId);
        memset(&context->pipelineShadingChange, 0, sizeof(context->pipelineShadingChange));
    }
    if (context->pipelinePrepareReactivity.pipeline != nullptr && backendInterface->fpDestroyPipeline != nullptr) {
        backendInterface->fpDestroyPipeline(backendInterface, &context->pipelinePrepareReactivity, context->effectContextId);
        memset(&context->pipelinePrepareReactivity, 0, sizeof(context->pipelinePrepareReactivity));
    }
    if (context->pipelineLumaInstability.pipeline != nullptr && backendInterface->fpDestroyPipeline != nullptr) {
        backendInterface->fpDestroyPipeline(backendInterface, &context->pipelineLumaInstability, context->effectContextId);
        memset(&context->pipelineLumaInstability, 0, sizeof(context->pipelineLumaInstability));
    }
    if (context->pipelineAccumulate.pipeline != nullptr && backendInterface->fpDestroyPipeline != nullptr) {
        backendInterface->fpDestroyPipeline(backendInterface, &context->pipelineAccumulate, context->effectContextId);
        memset(&context->pipelineAccumulate, 0, sizeof(context->pipelineAccumulate));
    }

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
    }
    context->allocatedResourceCount = 0;

    // Reset all resource identifier slots to unallocated sentinel
    for (uint32_t i = 0; i < FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_COUNT; ++i) {
        context->srvResources[i].internalIndex = -1;
        context->uavResources[i].internalIndex = -1;
        context->rtResources[i].internalIndex = -1;
    }

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

    // Create compute pipeline for PrepareInputs pass
    FfxmPipelineDescription pipelineDescription = {};
    pipelineDescription.contextFlags = contextDescription->flags;
    pipelineDescription.stage = FFXM_BIND_COMPUTE_SHADER_STAGE;

    pipelineDescription.samplerCount = 2;
    FfxmSamplerDescription samplerDescs[2] = {
        { 0, FFXM_FILTER_TYPE_MINMAGMIP_POINT, FFXM_ADDRESS_MODE_CLAMP, FFXM_ADDRESS_MODE_CLAMP, FFXM_ADDRESS_MODE_CLAMP, FFXM_BIND_COMPUTE_SHADER_STAGE },
        { 1, FFXM_FILTER_TYPE_MINMAGMIP_LINEAR, FFXM_ADDRESS_MODE_CLAMP, FFXM_ADDRESS_MODE_CLAMP, FFXM_ADDRESS_MODE_CLAMP, FFXM_BIND_COMPUTE_SHADER_STAGE }
    };
    pipelineDescription.samplers = samplerDescs;

    pipelineDescription.rootConstantBufferCount = 1;
    FfxmRootConstantDescription rootConstantDescs[1] = {
        { sizeof(Fsr3UpscalerConstants) / sizeof(uint32_t), FFXM_BIND_COMPUTE_SHADER_STAGE }
    };
    pipelineDescription.rootConstants = rootConstantDescs;

    bool supportedFP16 = context->deviceCapabilities.fp16Supported;
    bool canForceWave64 = false;
    bool useLut = false;
    if (context->deviceCapabilities.waveLaneCountMin == 32 && context->deviceCapabilities.waveLaneCountMax == 64) {
        useLut = true;
    }

    wcscpy(pipelineDescription.name, L"FSR3-PREPARE-INPUTS");
    FfxmErrorCode pipelineErr = context->contextDescription.backendInterface.fpCreateComputePipeline(
        &context->contextDescription.backendInterface,
        FFXM_EFFECT_FSR3UPSCALER,
        FFXM_FSR3UPSCALER_PASS_PREPARE_INPUTS,
        0,
        getPipelinePermutationFlags(contextDescription->flags, FFXM_FSR3UPSCALER_PASS_PREPARE_INPUTS, supportedFP16, canForceWave64, useLut),
        &pipelineDescription,
        context->effectContextId,
        &context->pipelinePrepareInputs);
    if (pipelineErr != FFXM_OK) {
        return pipelineErr;
    }

    pipelineErr = patchResourceBindings(&context->pipelinePrepareInputs);
    if (pipelineErr != FFXM_OK) {
        return pipelineErr;
    }

    // Create compute pipeline for Compute Luminance Pyramid pass
    wcscpy(pipelineDescription.name, L"FSR3-LUMA-PYRAMID");
    pipelineErr = context->contextDescription.backendInterface.fpCreateComputePipeline(
        &context->contextDescription.backendInterface,
        FFXM_EFFECT_FSR3UPSCALER,
        FFXM_FSR3UPSCALER_PASS_LUMA_PYRAMID,
        0,
        getPipelinePermutationFlags(contextDescription->flags, FFXM_FSR3UPSCALER_PASS_LUMA_PYRAMID, supportedFP16, canForceWave64, useLut),
        &pipelineDescription,
        context->effectContextId,
        &context->pipelineLumaPyramid);
    if (pipelineErr != FFXM_OK) {
        return pipelineErr;
    }

    pipelineErr = patchResourceBindings(&context->pipelineLumaPyramid);
    if (pipelineErr != FFXM_OK) {
        return pipelineErr;
    }

    // Create compute pipeline for Compute Shading Change Pyramid pass
    wcscpy(pipelineDescription.name, L"FSR3-SHADING-CHANGE-PYRAMID");
    pipelineErr = context->contextDescription.backendInterface.fpCreateComputePipeline(
        &context->contextDescription.backendInterface,
        FFXM_EFFECT_FSR3UPSCALER,
        FFXM_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID,
        0,
        getPipelinePermutationFlags(contextDescription->flags, FFXM_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID, supportedFP16, canForceWave64, useLut),
        &pipelineDescription,
        context->effectContextId,
        &context->pipelineShadingChangePyramid);
    if (pipelineErr != FFXM_OK) {
        return pipelineErr;
    }

    pipelineErr = patchResourceBindings(&context->pipelineShadingChangePyramid);
    if (pipelineErr != FFXM_OK) {
        return pipelineErr;
    }

    // Create compute pipeline for Compute Shading Change pass
    wcscpy(pipelineDescription.name, L"FSR3-SHADING-CHANGE");
    pipelineErr = context->contextDescription.backendInterface.fpCreateComputePipeline(
        &context->contextDescription.backendInterface,
        FFXM_EFFECT_FSR3UPSCALER,
        FFXM_FSR3UPSCALER_PASS_SHADING_CHANGE,
        0,
        getPipelinePermutationFlags(contextDescription->flags, FFXM_FSR3UPSCALER_PASS_SHADING_CHANGE, supportedFP16, canForceWave64, useLut),
        &pipelineDescription,
        context->effectContextId,
        &context->pipelineShadingChange);
    if (pipelineErr != FFXM_OK) {
        return pipelineErr;
    }

    pipelineErr = patchResourceBindings(&context->pipelineShadingChange);
    if (pipelineErr != FFXM_OK) {
        return pipelineErr;
    }

    // Create compute pipeline for Prepare Reactivity pass
    wcscpy(pipelineDescription.name, L"FSR3-PREPARE-REACTIVITY");
    pipelineErr = context->contextDescription.backendInterface.fpCreateComputePipeline(
        &context->contextDescription.backendInterface,
        FFXM_EFFECT_FSR3UPSCALER,
        FFXM_FSR3UPSCALER_PASS_PREPARE_REACTIVITY,
        0,
        getPipelinePermutationFlags(contextDescription->flags, FFXM_FSR3UPSCALER_PASS_PREPARE_REACTIVITY, supportedFP16, canForceWave64, useLut),
        &pipelineDescription,
        context->effectContextId,
        &context->pipelinePrepareReactivity);
    if (pipelineErr != FFXM_OK) {
        return pipelineErr;
    }

    pipelineErr = patchResourceBindings(&context->pipelinePrepareReactivity);
    if (pipelineErr != FFXM_OK) {
        return pipelineErr;
    }

    // Create compute pipeline for Luma Instability pass
    wcscpy(pipelineDescription.name, L"FSR3-LUMA-INSTABILITY");
    pipelineErr = context->contextDescription.backendInterface.fpCreateComputePipeline(
        &context->contextDescription.backendInterface,
        FFXM_EFFECT_FSR3UPSCALER,
        FFXM_FSR3UPSCALER_PASS_LUMA_INSTABILITY,
        0,
        getPipelinePermutationFlags(contextDescription->flags, FFXM_FSR3UPSCALER_PASS_LUMA_INSTABILITY, supportedFP16, canForceWave64, useLut),
        &pipelineDescription,
        context->effectContextId,
        &context->pipelineLumaInstability);
    if (pipelineErr != FFXM_OK) {
        return pipelineErr;
    }

    pipelineErr = patchResourceBindings(&context->pipelineLumaInstability);
    if (pipelineErr != FFXM_OK) {
        return pipelineErr;
    }

    // Create compute pipeline for Accumulate pass
    wcscpy(pipelineDescription.name, L"FSR3-ACCUMULATE");
    pipelineErr = context->contextDescription.backendInterface.fpCreateComputePipeline(
        &context->contextDescription.backendInterface,
        FFXM_EFFECT_FSR3UPSCALER,
        FFXM_FSR3UPSCALER_PASS_ACCUMULATE,
        0,
        getPipelinePermutationFlags(contextDescription->flags, FFXM_FSR3UPSCALER_PASS_ACCUMULATE, supportedFP16, canForceWave64, useLut),
        &pipelineDescription,
        context->effectContextId,
        &context->pipelineAccumulate);
    if (pipelineErr != FFXM_OK) {
        return pipelineErr;
    }

    pipelineErr = patchResourceBindings(&context->pipelineAccumulate);
    if (pipelineErr != FFXM_OK) {
        return pipelineErr;
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
        pDispatchDescription->renderSize.width > 0 &&
        pDispatchDescription->renderSize.width <= contextPrivate->contextDescription.maxRenderSize.width,
        FFXM_ERROR_OUT_OF_RANGE);
    FFXM_RETURN_ON_ERROR(
        pDispatchDescription->renderSize.height > 0 &&
        pDispatchDescription->renderSize.height <= contextPrivate->contextDescription.maxRenderSize.height,
        FFXM_ERROR_OUT_OF_RANGE);
    FFXM_RETURN_ON_ERROR(
        pDispatchDescription->upscaleSize.width > 0 &&
        pDispatchDescription->upscaleSize.width <= contextPrivate->contextDescription.maxUpscaleSize.width,
        FFXM_ERROR_OUT_OF_RANGE);
    FFXM_RETURN_ON_ERROR(
        pDispatchDescription->upscaleSize.height > 0 &&
        pDispatchDescription->upscaleSize.height <= contextPrivate->contextDescription.maxUpscaleSize.height,
        FFXM_ERROR_OUT_OF_RANGE);

    // Transactional temporal state handling:
    // Build pending constants locally starting from the last committed state.
    Fsr3UpscalerConstants pendingConstants = contextPrivate->constants;

    pendingConstants.renderSize[0] = pDispatchDescription->renderSize.width;
    pendingConstants.renderSize[1] = pDispatchDescription->renderSize.height;
    pendingConstants.upscaleSize[0] = pDispatchDescription->upscaleSize.width;
    pendingConstants.upscaleSize[1] = pDispatchDescription->upscaleSize.height;

    pendingConstants.jitterOffset[0] = pDispatchDescription->jitterOffset.x;
    pendingConstants.jitterOffset[1] = pDispatchDescription->jitterOffset.y;

    // Motion vector scales
    const int32_t* motionVectorsTargetSize = (contextPrivate->contextDescription.flags & FFXM_FSR3UPSCALER_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS)
        ? pendingConstants.upscaleSize
        : pendingConstants.renderSize;
    if (motionVectorsTargetSize[0] > 0 && motionVectorsTargetSize[1] > 0) {
        pendingConstants.motionVectorScale[0] = (pDispatchDescription->motionVectorScale.x / static_cast<float>(motionVectorsTargetSize[0]));
        pendingConstants.motionVectorScale[1] = (pDispatchDescription->motionVectorScale.y / static_cast<float>(motionVectorsTargetSize[1]));
    } else {
        pendingConstants.motionVectorScale[0] = pDispatchDescription->motionVectorScale.x;
        pendingConstants.motionVectorScale[1] = pDispatchDescription->motionVectorScale.y;
    }

    // Downscale factor
    if (pendingConstants.upscaleSize[0] > 0 && pendingConstants.upscaleSize[1] > 0) {
        pendingConstants.downscaleFactor[0] = static_cast<float>(pendingConstants.renderSize[0]) / static_cast<float>(pendingConstants.upscaleSize[0]);
        pendingConstants.downscaleFactor[1] = static_cast<float>(pendingConstants.renderSize[1]) / static_cast<float>(pendingConstants.upscaleSize[1]);
    }

    pendingConstants.deltaTime = pDispatchDescription->frameTimeDelta;
    pendingConstants.tanHalfFOV = tanf(pDispatchDescription->cameraFovAngleVertical * 0.5f);
    pendingConstants.viewSpaceToMetersFactor = pDispatchDescription->viewSpaceToMetersFactor;

    // Compute device depth to view space depth parameters into pendingConstants
    setupDeviceDepthToViewSpaceDepthParams(&pendingConstants, &contextPrivate->contextDescription, pDispatchDescription);

    // Pending Pre-exposure factors
    const float pendingPreviousFramePreExposure = contextPrivate->preExposure;
    const float pendingPreExposure = (pDispatchDescription->preExposure != 0.0f) ? pDispatchDescription->preExposure : 1.0f;
    pendingConstants.deltaPreExposure = 1.0f;
    if (pendingPreviousFramePreExposure > 0.0f) {
        pendingConstants.deltaPreExposure = pendingPreExposure / pendingPreviousFramePreExposure;
    }

    // Pending Frame index & jitter phase count
    const int32_t jitterPhaseCount = ffxmFsr3UpscalerGetJitterPhaseCount(pDispatchDescription->renderSize.width, pendingConstants.upscaleSize[0]);
    float pendingJitterPhaseCount = contextPrivate->constants.jitterPhaseCount;
    float pendingFrameIndex = contextPrivate->constants.frameIndex;

    if (pDispatchDescription->reset || contextPrivate->firstExecution || pendingJitterPhaseCount == 0.0f) {
        pendingJitterPhaseCount = static_cast<float>(jitterPhaseCount);
        pendingFrameIndex = 0.0f;
    } else {
        const int32_t jitterPhaseCountDelta = static_cast<int32_t>(jitterPhaseCount - pendingJitterPhaseCount);
        if (jitterPhaseCountDelta > 0) {
            pendingJitterPhaseCount += 1.0f;
        } else if (jitterPhaseCountDelta < 0) {
            pendingJitterPhaseCount -= 1.0f;
        }
        pendingFrameIndex += 1.0f;
    }
    pendingConstants.jitterPhaseCount = pendingJitterPhaseCount;
    pendingConstants.frameIndex = pendingFrameIndex;

    // Stage pending constants into constant buffer slot 0 for shader execution
    stageConstantBuffer(
        &contextPrivate->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER],
        &pendingConstants,
        sizeof(pendingConstants));

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

    // Half-resolution SPD constants setup for Shading Change Pyramid
    const uint32_t halfRenderWidth = (pDispatchDescription->renderSize.width + 1) / 2;
    const uint32_t halfRenderHeight = (pDispatchDescription->renderSize.height + 1) / 2;
    uint32_t dispatchShadingThreadGroupCountXY[2];
    uint32_t workGroupOffsetShading[2];
    uint32_t numWorkGroupsAndMipsShading[2];
    uint32_t rectInfoShading[4] = { 0, 0, halfRenderWidth, halfRenderHeight };
    ffxSpdSetup(dispatchShadingThreadGroupCountXY, workGroupOffsetShading, numWorkGroupsAndMipsShading, rectInfoShading);

    Fsr3UpscalerSpdConstants shadingChangePyramidConstants;
    shadingChangePyramidConstants.mips = numWorkGroupsAndMipsShading[1];
    shadingChangePyramidConstants.numWorkGroups = numWorkGroupsAndMipsShading[0];
    shadingChangePyramidConstants.workGroupOffset[0] = workGroupOffsetShading[0];
    shadingChangePyramidConstants.workGroupOffset[1] = workGroupOffsetShading[1];
    shadingChangePyramidConstants.renderSize[0] = halfRenderWidth;
    shadingChangePyramidConstants.renderSize[1] = halfRenderHeight;

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

    // Dynamic resource registration table
    struct DynamicResourceRegistration {
        const FfxmResource* resource;
        uint32_t srvIdentifier;
        uint32_t uavIdentifier;
    };

    const DynamicResourceRegistration registrations[] = {
        { &pDispatchDescription->color, FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_COLOR, 0 },
        { &pDispatchDescription->depth, FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_DEPTH, 0 },
        { &pDispatchDescription->motionVectors, FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_MOTION_VECTORS, 0 },
        { &pDispatchDescription->dilatedMotionVectors, FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS, FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS },
        { &pDispatchDescription->dilatedDepth, FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_DEPTH, FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_DEPTH },
        { &pDispatchDescription->reconstructedPrevNearestDepth, FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH, FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH },
        { &pDispatchDescription->output, FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT, FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT },
    };

    uint32_t registeredCount = 0;
    FfxmErrorCode dispatchResult = FFXM_OK;

    for (uint32_t i = 0; i < FFXM_COUNTOF(registrations); ++i) {
        FfxmErrorCode regRes = contextPrivate->contextDescription.backendInterface.fpRegisterResource(
            &contextPrivate->contextDescription.backendInterface,
            registrations[i].resource,
            contextPrivate->effectContextId,
            &contextPrivate->srvResources[registrations[i].srvIdentifier]);
        if (regRes != FFXM_OK) {
            dispatchResult = regRes;
            break;
        }
        if (registrations[i].uavIdentifier != 0) {
            contextPrivate->uavResources[registrations[i].uavIdentifier] =
                contextPrivate->srvResources[registrations[i].srvIdentifier];
        }
        registeredCount++;
    }

    if (dispatchResult == FFXM_OK) {
        // Route exposure resource: if external exposure provided, register it; otherwise alias to default exposure
        if (pDispatchDescription->exposure.resource != nullptr) {
            FfxmErrorCode expRes = contextPrivate->contextDescription.backendInterface.fpRegisterResource(
                &contextPrivate->contextDescription.backendInterface,
                &pDispatchDescription->exposure,
                contextPrivate->effectContextId,
                &contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_EXPOSURE]);
            if (expRes != FFXM_OK) {
                dispatchResult = expRes;
            } else {
                registeredCount++;
            }
        } else {
            contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_EXPOSURE] =
                contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_EXPOSURE];
        }

        // Route reactive mask: if external mask provided, register it; otherwise alias to default zero reactivity
        if (dispatchResult == FFXM_OK) {
            if (pDispatchDescription->reactive.resource != nullptr) {
                FfxmErrorCode reactRes = contextPrivate->contextDescription.backendInterface.fpRegisterResource(
                    &contextPrivate->contextDescription.backendInterface,
                    &pDispatchDescription->reactive,
                    contextPrivate->effectContextId,
                    &contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_REACTIVE_MASK]);
                if (reactRes != FFXM_OK) {
                    dispatchResult = reactRes;
                } else {
                    registeredCount++;
                }
            } else {
                contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_REACTIVE_MASK] =
                    contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY];
            }
        }

        // Route transparency and composition mask: if provided, register it; otherwise alias to default zero reactivity
        if (dispatchResult == FFXM_OK) {
            if (pDispatchDescription->transparencyAndComposition.resource != nullptr) {
                FfxmErrorCode tcRes = contextPrivate->contextDescription.backendInterface.fpRegisterResource(
                    &contextPrivate->contextDescription.backendInterface,
                    &pDispatchDescription->transparencyAndComposition,
                    contextPrivate->effectContextId,
                    &contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_TRANSPARENCY_AND_COMPOSITION_MASK]);
                if (tcRes != FFXM_OK) {
                    dispatchResult = tcRes;
                } else {
                    registeredCount++;
                }
            } else {
                contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_TRANSPARENCY_AND_COMPOSITION_MASK] =
                    contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY];
            }
        }

        // Ping-pong & aliasable resource routing
        const bool isOddFrame = !!(contextPrivate->resourceFrameIndex & 1);
        const uint32_t currentLumaResourceIndex = isOddFrame
            ? FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_2
            : FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_1;
        const uint32_t previousLumaResourceIndex = isOddFrame
            ? FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_1
            : FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_2;

        contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_CURRENT_LUMA] = contextPrivate->srvResources[currentLumaResourceIndex];
        contextPrivate->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_CURRENT_LUMA] = contextPrivate->uavResources[currentLumaResourceIndex];
        contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_PREVIOUS_LUMA] = contextPrivate->srvResources[previousLumaResourceIndex];

        const uint32_t currentAccumulationResourceIndex = isOddFrame
            ? FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2
            : FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1;
        const uint32_t previousAccumulationResourceIndex = isOddFrame
            ? FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1
            : FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2;

        contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION] =
            contextPrivate->srvResources[previousAccumulationResourceIndex];
        contextPrivate->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION] =
            contextPrivate->uavResources[currentAccumulationResourceIndex];

        // Luma history ping-pong: previous history as SRV, current history as UAV
        const uint32_t currentLumaHistoryResourceIndex = isOddFrame
            ? FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_2
            : FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_1;
        const uint32_t previousLumaHistoryResourceIndex = isOddFrame
            ? FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_1
            : FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_2;

        contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY] =
            contextPrivate->srvResources[previousLumaHistoryResourceIndex];
        contextPrivate->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY] =
            contextPrivate->uavResources[currentLumaHistoryResourceIndex];

        // Internal upscaled color ping-pong: previous upscaled color as SRV, current upscaled color as UAV
        const uint32_t currentUpscaledColorResourceIndex = isOddFrame
            ? FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_2
            : FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_1;
        const uint32_t previousUpscaledColorResourceIndex = isOddFrame
            ? FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_1
            : FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_2;

        contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR] =
            contextPrivate->srvResources[previousUpscaledColorResourceIndex];
        contextPrivate->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR] =
            contextPrivate->uavResources[currentUpscaledColorResourceIndex];

        // Aliasable intermediate surface routing:
        // Farthest depth and LumaInstability both alias INTERMEDIATE_FP16x1 safely without lifetime overlap
        contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH] =
            contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERMEDIATE_FP16x1];
        contextPrivate->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH] =
            contextPrivate->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERMEDIATE_FP16x1];

        contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_INSTABILITY] =
            contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERMEDIATE_FP16x1];
        contextPrivate->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_INSTABILITY] =
            contextPrivate->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERMEDIATE_FP16x1];

        // History Reset Clears
        if (contextPrivate->firstExecution || pDispatchDescription->reset) {
            // Clear Frame Info to {-1.0f, 1.0f, 0.0f, 0.0f}
            FfxmGpuJobDescription clearFrameInfoJob = { FFXM_GPU_JOB_CLEAR_FLOAT };
            const float clearFrameInfoValue[4] = { -1.0f, 1.0f, 0.0f, 0.0f };
            memcpy(clearFrameInfoJob.clearJobDescriptor.color, clearFrameInfoValue, 4 * sizeof(float));
            clearFrameInfoJob.clearJobDescriptor.target = contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_FRAME_INFO];
            dispatchResult = contextPrivate->contextDescription.backendInterface.fpScheduleGpuJob(
                &contextPrivate->contextDescription.backendInterface, &clearFrameInfoJob);

            // Clear Accumulation 1 to 0.0f
            if (dispatchResult == FFXM_OK) {
                FfxmGpuJobDescription clearAccum1Job = { FFXM_GPU_JOB_CLEAR_FLOAT };
                const float clearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                memcpy(clearAccum1Job.clearJobDescriptor.color, clearZero, 4 * sizeof(float));
                clearAccum1Job.clearJobDescriptor.target = contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1];
                dispatchResult = contextPrivate->contextDescription.backendInterface.fpScheduleGpuJob(
                    &contextPrivate->contextDescription.backendInterface, &clearAccum1Job);
            }

            // Clear Accumulation 2 to 0.0f
            if (dispatchResult == FFXM_OK) {
                FfxmGpuJobDescription clearAccum2Job = { FFXM_GPU_JOB_CLEAR_FLOAT };
                const float clearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                memcpy(clearAccum2Job.clearJobDescriptor.color, clearZero, 4 * sizeof(float));
                clearAccum2Job.clearJobDescriptor.target = contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2];
                dispatchResult = contextPrivate->contextDescription.backendInterface.fpScheduleGpuJob(
                    &contextPrivate->contextDescription.backendInterface, &clearAccum2Job);
            }

            // Clear Luma History 1 to 0.0f
            if (dispatchResult == FFXM_OK) {
                FfxmGpuJobDescription clearLumaHist1Job = { FFXM_GPU_JOB_CLEAR_FLOAT };
                const float clearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                memcpy(clearLumaHist1Job.clearJobDescriptor.color, clearZero, 4 * sizeof(float));
                clearLumaHist1Job.clearJobDescriptor.target = contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_1];
                dispatchResult = contextPrivate->contextDescription.backendInterface.fpScheduleGpuJob(
                    &contextPrivate->contextDescription.backendInterface, &clearLumaHist1Job);
            }

            // Clear Luma History 2 to 0.0f
            if (dispatchResult == FFXM_OK) {
                FfxmGpuJobDescription clearLumaHist2Job = { FFXM_GPU_JOB_CLEAR_FLOAT };
                const float clearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                memcpy(clearLumaHist2Job.clearJobDescriptor.color, clearZero, 4 * sizeof(float));
                clearLumaHist2Job.clearJobDescriptor.target = contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_2];
                dispatchResult = contextPrivate->contextDescription.backendInterface.fpScheduleGpuJob(
                    &contextPrivate->contextDescription.backendInterface, &clearLumaHist2Job);
            }

            // Clear Internal Upscaled Color 1 to 0.0f
            if (dispatchResult == FFXM_OK) {
                FfxmGpuJobDescription clearUpscaled1Job = { FFXM_GPU_JOB_CLEAR_FLOAT };
                const float clearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                memcpy(clearUpscaled1Job.clearJobDescriptor.color, clearZero, 4 * sizeof(float));
                clearUpscaled1Job.clearJobDescriptor.target = contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_1];
                dispatchResult = contextPrivate->contextDescription.backendInterface.fpScheduleGpuJob(
                    &contextPrivate->contextDescription.backendInterface, &clearUpscaled1Job);
            }

            // Clear Internal Upscaled Color 2 to 0.0f
            if (dispatchResult == FFXM_OK) {
                FfxmGpuJobDescription clearUpscaled2Job = { FFXM_GPU_JOB_CLEAR_FLOAT };
                const float clearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                memcpy(clearUpscaled2Job.clearJobDescriptor.color, clearZero, 4 * sizeof(float));
                clearUpscaled2Job.clearJobDescriptor.target = contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_2];
                dispatchResult = contextPrivate->contextDescription.backendInterface.fpScheduleGpuJob(
                    &contextPrivate->contextDescription.backendInterface, &clearUpscaled2Job);
            }

            // Clear New Locks to 0.0f
            if (dispatchResult == FFXM_OK) {
                FfxmGpuJobDescription clearNewLocksJob = { FFXM_GPU_JOB_CLEAR_FLOAT };
                const float clearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                memcpy(clearNewLocksJob.clearJobDescriptor.color, clearZero, 4 * sizeof(float));
                clearNewLocksJob.clearJobDescriptor.target = contextPrivate->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS];
                dispatchResult = contextPrivate->contextDescription.backendInterface.fpScheduleGpuJob(
                    &contextPrivate->contextDescription.backendInterface, &clearNewLocksJob);
            }
        }

        // Per-frame clears
        // 1. Clear Reconstructed Previous Nearest Depth surface
        if (dispatchResult == FFXM_OK) {
            FfxmGpuJobDescription clearJob = { FFXM_GPU_JOB_CLEAR_FLOAT };
            const bool bInverted = (contextPrivate->contextDescription.flags & FFXM_FSR3UPSCALER_ENABLE_DEPTH_INVERTED) == FFXM_FSR3UPSCALER_ENABLE_DEPTH_INVERTED;
            const float clearDepthValue[]{ bInverted ? 0.f : 1.f, bInverted ? 0.f : 1.f, bInverted ? 0.f : 1.f, bInverted ? 0.f : 1.f };
            memcpy(clearJob.clearJobDescriptor.color, clearDepthValue, 4 * sizeof(float));
            clearJob.clearJobDescriptor.target = contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH];
            dispatchResult = contextPrivate->contextDescription.backendInterface.fpScheduleGpuJob(
                &contextPrivate->contextDescription.backendInterface, &clearJob);
        }

        // 2. Clear SPD Atomic Counter (1x1 uint32_t) to 0
        if (dispatchResult == FFXM_OK) {
            FfxmGpuJobDescription clearSpdAtomicJob = { FFXM_GPU_JOB_CLEAR_FLOAT };
            const float clearZeroValue[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            memcpy(clearSpdAtomicJob.clearJobDescriptor.color, clearZeroValue, 4 * sizeof(float));
            clearSpdAtomicJob.clearJobDescriptor.target = contextPrivate->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_ATOMIC_COUNT];
            dispatchResult = contextPrivate->contextDescription.backendInterface.fpScheduleGpuJob(
                &contextPrivate->contextDescription.backendInterface, &clearSpdAtomicJob);
        }

        // 3. Clear SPD_MIPS UAV to 0.0f
        if (dispatchResult == FFXM_OK) {
            FfxmGpuJobDescription clearSpdMipsJob = { FFXM_GPU_JOB_CLEAR_FLOAT };
            const float clearZeroValue[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            memcpy(clearSpdMipsJob.clearJobDescriptor.color, clearZeroValue, 4 * sizeof(float));
            clearSpdMipsJob.clearJobDescriptor.target = contextPrivate->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS];
            dispatchResult = contextPrivate->contextDescription.backendInterface.fpScheduleGpuJob(
                &contextPrivate->contextDescription.backendInterface, &clearSpdMipsJob);
        }

        // Schedule PrepareInputs compute pass
        if (dispatchResult == FFXM_OK) {
            const int32_t threadGroupWorkRegionDim = 8;
            const int32_t dispatchSrcX = (pendingConstants.renderSize[0] + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
            const int32_t dispatchSrcY = (pendingConstants.renderSize[1] + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
            dispatchResult = scheduleDispatch(contextPrivate, pDispatchDescription, &contextPrivate->pipelinePrepareInputs, dispatchSrcX, dispatchSrcY);
        }

        // Schedule Compute Luminance Pyramid compute pass
        if (dispatchResult == FFXM_OK) {
            stageConstantBuffer(
                &contextPrivate->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD],
                &luminancePyramidConstants,
                sizeof(luminancePyramidConstants));
            dispatchResult = scheduleDispatch(contextPrivate, pDispatchDescription, &contextPrivate->pipelineLumaPyramid, dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1]);
        }

        // Schedule Compute Shading Change Pyramid compute pass
        if (dispatchResult == FFXM_OK) {
            // Reset SPD atomic counter before second SPD downsampling pass
            FfxmGpuJobDescription clearSpdAtomicJob = { FFXM_GPU_JOB_CLEAR_FLOAT };
            const float clearZeroValue[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            memcpy(clearSpdAtomicJob.clearJobDescriptor.color, clearZeroValue, 4 * sizeof(float));
            clearSpdAtomicJob.clearJobDescriptor.target = contextPrivate->uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_ATOMIC_COUNT];
            dispatchResult = contextPrivate->contextDescription.backendInterface.fpScheduleGpuJob(
                &contextPrivate->contextDescription.backendInterface, &clearSpdAtomicJob);

            if (dispatchResult == FFXM_OK) {
                stageConstantBuffer(
                    &contextPrivate->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD],
                    &shadingChangePyramidConstants,
                    sizeof(shadingChangePyramidConstants));
                dispatchResult = scheduleDispatch(contextPrivate, pDispatchDescription, &contextPrivate->pipelineShadingChangePyramid, dispatchShadingThreadGroupCountXY[0], dispatchShadingThreadGroupCountXY[1]);
            }
        }

        // Schedule Compute Shading Change compute pass
        if (dispatchResult == FFXM_OK) {
            const int32_t threadGroupWorkRegionDim = 8;
            const int32_t shadingChangeWidth = std::max(1, pendingConstants.renderSize[0] / 2);
            const int32_t shadingChangeHeight = std::max(1, pendingConstants.renderSize[1] / 2);
            const int32_t dispatchShadingChangeX = (shadingChangeWidth + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
            const int32_t dispatchShadingChangeY = (shadingChangeHeight + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
            dispatchResult = scheduleDispatch(contextPrivate, pDispatchDescription, &contextPrivate->pipelineShadingChange, dispatchShadingChangeX, dispatchShadingChangeY);
        }

        // Schedule Prepare Reactivity compute pass
        if (dispatchResult == FFXM_OK) {
            const int32_t threadGroupWorkRegionDim = 8;
            const int32_t dispatchReactivityX = (pendingConstants.renderSize[0] + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
            const int32_t dispatchReactivityY = (pendingConstants.renderSize[1] + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
            dispatchResult = scheduleDispatch(contextPrivate, pDispatchDescription, &contextPrivate->pipelinePrepareReactivity, dispatchReactivityX, dispatchReactivityY);
        }

        // Schedule LumaInstability compute pass
        if (dispatchResult == FFXM_OK) {
            const int32_t threadGroupWorkRegionDim = 8;
            const int32_t dispatchLumaInstabilityX = (pendingConstants.renderSize[0] + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
            const int32_t dispatchLumaInstabilityY = (pendingConstants.renderSize[1] + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
            dispatchResult = scheduleDispatch(contextPrivate, pDispatchDescription, &contextPrivate->pipelineLumaInstability, dispatchLumaInstabilityX, dispatchLumaInstabilityY);
        }

        // Schedule Accumulate compute pass
        if (dispatchResult == FFXM_OK) {
            const int32_t threadGroupWorkRegionDim = 8;
            const int32_t dispatchAccumulateX = (pendingConstants.upscaleSize[0] + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
            const int32_t dispatchAccumulateY = (pendingConstants.upscaleSize[1] + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
            dispatchResult = scheduleDispatch(contextPrivate, pDispatchDescription, &contextPrivate->pipelineAccumulate, dispatchAccumulateX, dispatchAccumulateY);
        }

        // Execute scheduled GPU jobs only if all jobs scheduled successfully
        if (dispatchResult == FFXM_OK) {
            dispatchResult = contextPrivate->contextDescription.backendInterface.fpExecuteGpuJobs(
                &contextPrivate->contextDescription.backendInterface,
                pDispatchDescription->commandList);
        }
    }

    // Resource unregistration & dynamic slot cleanup
    if (registeredCount > 0) {
        FfxmErrorCode unregRes = contextPrivate->contextDescription.backendInterface.fpUnregisterResources(
            &contextPrivate->contextDescription.backendInterface,
            pDispatchDescription->commandList,
            contextPrivate->effectContextId);

        // If dispatch pipeline succeeded, propagate cleanup errors;
        // if dispatch already encountered an earlier failure, preserve that original error.
        if (dispatchResult == FFXM_OK && unregRes != FFXM_OK) {
            dispatchResult = unregRes;
        }

        // Reset dynamic resource tracking slots back to unallocated sentinel (-1)
        for (uint32_t i = 0; i < FFXM_COUNTOF(registrations); ++i) {
            contextPrivate->srvResources[registrations[i].srvIdentifier].internalIndex = -1;
            if (registrations[i].uavIdentifier != 0) {
                contextPrivate->uavResources[registrations[i].uavIdentifier].internalIndex = -1;
            }
        }
        contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_EXPOSURE].internalIndex = -1;
        contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_REACTIVE_MASK].internalIndex = -1;
        contextPrivate->srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_TRANSPARENCY_AND_COMPOSITION_MASK].internalIndex = -1;
    }

    // Transactional state commit: only commit pending frame state on complete dispatch success
    if (dispatchResult == FFXM_OK) {
        contextPrivate->constants = pendingConstants;
        contextPrivate->previousFramePreExposure = pendingPreviousFramePreExposure;
        contextPrivate->preExposure = pendingPreExposure;

        contextPrivate->constants.previousFrameRenderSize[0] = pDispatchDescription->renderSize.width;
        contextPrivate->constants.previousFrameRenderSize[1] = pDispatchDescription->renderSize.height;
        contextPrivate->constants.previousFrameUpscaleSize[0] = pDispatchDescription->upscaleSize.width;
        contextPrivate->constants.previousFrameUpscaleSize[1] = pDispatchDescription->upscaleSize.height;

        contextPrivate->constants.previousFrameJitterOffset[0] = pDispatchDescription->jitterOffset.x;
        contextPrivate->constants.previousFrameJitterOffset[1] = pDispatchDescription->jitterOffset.y;
        contextPrivate->previousJitterOffset[0] = pDispatchDescription->jitterOffset.x;
        contextPrivate->previousJitterOffset[1] = pDispatchDescription->jitterOffset.y;

        contextPrivate->resourceFrameIndex = (contextPrivate->resourceFrameIndex + 1) % 16;
        contextPrivate->firstExecution = false;

        stageConstantBuffer(
            &contextPrivate->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER],
            &contextPrivate->constants,
            sizeof(contextPrivate->constants));
    } else {
        // Transactional rollback: preserve committed constants in constant buffer slot 0
        stageConstantBuffer(
            &contextPrivate->constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER],
            &contextPrivate->constants,
            sizeof(contextPrivate->constants));
    }

    return dispatchResult;
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
