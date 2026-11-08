// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#include "ffxm_frameinterpolation_private.h"
#include "host/ffxm_assert.h"
#include "host/ffxm_util.h"
#include <cstring>
#include <cwchar>

namespace arm {

FfxmErrorCode ffxmFrameInterpolationContextCreate(
    FfxmFrameInterpolationContext* context,
    const FfxmFrameInterpolationContextDescription* contextDescription)
{
    FFXM_RETURN_ON_ERROR(context, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(contextDescription, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(contextDescription->backendInterface.fpGetDeviceCapabilities, FFXM_ERROR_INCOMPLETE_INTERFACE);
    FFXM_RETURN_ON_ERROR(contextDescription->backendInterface.fpCreateBackendContext, FFXM_ERROR_INCOMPLETE_INTERFACE);
    FFXM_RETURN_ON_ERROR(contextDescription->backendInterface.fpDestroyBackendContext, FFXM_ERROR_INCOMPLETE_INTERFACE);

    if (contextDescription->maxDisplaySize.width == 0 || contextDescription->maxDisplaySize.height == 0) {
        return FFXM_ERROR_INVALID_ARGUMENT;
    }

    std::memset(context, 0, sizeof(FfxmFrameInterpolationContext));
    auto* priv = reinterpret_cast<FfxmFrameInterpolationContext_Private*>(context);
    priv->backendInterface = contextDescription->backendInterface;
    priv->contextDescription = *contextDescription;
    priv->effectContextId = 0;
    priv->frameIndex = 0;
    priv->firstExecution = true;
    priv->historyColor.internalIndex = -1;
    priv->interpolatedStaging.internalIndex = -1;

    // 1. Create backend context FIRST so backend state is initialized
    FfxmErrorCode err = priv->backendInterface.fpCreateBackendContext(&priv->backendInterface, &priv->effectContextId);
    if (err != FFXM_OK) {
        return err;
    }

    // 2. Query device capabilities from initialized backend context
    FfxmDeviceCapabilities caps = {};
    err = priv->backendInterface.fpGetDeviceCapabilities(&priv->backendInterface, &caps);
    if (err != FFXM_OK) {
        priv->backendInterface.fpDestroyBackendContext(&priv->backendInterface, priv->effectContextId);
        return err;
    }

    if (!caps.fp16Supported) {
        priv->backendInterface.fpDestroyBackendContext(&priv->backendInterface, priv->effectContextId);
        return FFXM_ERROR_UNSUPPORTED_DEVICE;
    }

    // 3. Allocate internal resources with transactional rollback
    if (priv->backendInterface.fpCreateResource) {
        FfxmCreateResourceDescription resDesc = {};
        resDesc.heapType = FFXM_HEAP_TYPE_DEFAULT;
        resDesc.resourceDescription.type = FFXM_RESOURCE_TYPE_TEXTURE2D;
        resDesc.resourceDescription.format = FFXM_SURFACE_FORMAT_R16G16B16A16_FLOAT;
        resDesc.resourceDescription.width = contextDescription->maxDisplaySize.width;
        resDesc.resourceDescription.height = contextDescription->maxDisplaySize.height;
        resDesc.resourceDescription.mipCount = 1;
        resDesc.resourceDescription.flags = FFXM_RESOURCE_FLAGS_ALIASABLE;
        resDesc.resourceDescription.usage = FFXM_RESOURCE_USAGE_UAV;
        resDesc.name = L"FrameInterpolation_HistoryColor";

        err = priv->backendInterface.fpCreateResource(&priv->backendInterface, &resDesc, priv->effectContextId, &priv->historyColor);
        if (err != FFXM_OK) {
            priv->backendInterface.fpDestroyBackendContext(&priv->backendInterface, priv->effectContextId);
            return err;
        }

        // Allocate interpolated staging texture
        resDesc.name = L"FrameInterpolation_InterpolatedStaging";
        err = priv->backendInterface.fpCreateResource(&priv->backendInterface, &resDesc, priv->effectContextId, &priv->interpolatedStaging);
        if (err != FFXM_OK) {
            if (priv->backendInterface.fpDestroyResource && priv->historyColor.internalIndex != -1) {
                priv->backendInterface.fpDestroyResource(&priv->backendInterface, priv->historyColor);
            }
            priv->backendInterface.fpDestroyBackendContext(&priv->backendInterface, priv->effectContextId);
            return err;
        }
    }

    // 4. Create compute pipeline for Frame Interpolation
    if (priv->backendInterface.fpCreateComputePipeline) {
        FfxmPipelineDescription pipelineDescription = {};
        pipelineDescription.contextFlags = contextDescription->flags;
        pipelineDescription.stage = FFXM_BIND_COMPUTE_SHADER_STAGE;

        pipelineDescription.samplerCount = 2;
        FfxmSamplerDescription samplerDescs[2] = {
            { 0, FFXM_FILTER_TYPE_MINMAGMIP_LINEAR, FFXM_ADDRESS_MODE_CLAMP, FFXM_ADDRESS_MODE_CLAMP, FFXM_ADDRESS_MODE_CLAMP, FFXM_BIND_COMPUTE_SHADER_STAGE },
            { 1, FFXM_FILTER_TYPE_MINMAGMIP_POINT,  FFXM_ADDRESS_MODE_CLAMP, FFXM_ADDRESS_MODE_CLAMP, FFXM_ADDRESS_MODE_CLAMP, FFXM_BIND_COMPUTE_SHADER_STAGE }
        };
        pipelineDescription.samplers = samplerDescs;

        pipelineDescription.rootConstantBufferCount = 1;
        FfxmRootConstantDescription rootConstantDescs[1] = {
            { sizeof(FrameInterpolationConstants) / sizeof(uint32_t), FFXM_BIND_COMPUTE_SHADER_STAGE }
        };
        pipelineDescription.rootConstants = rootConstantDescs;

        std::wcscpy(pipelineDescription.name, L"FRAMEINTERPOLATION-INTERPOLATE");
        err = priv->backendInterface.fpCreateComputePipeline(
            &priv->backendInterface,
            FFXM_EFFECT_FRAMEINTERPOLATION,
            FFXM_FRAMEINTERPOLATION_PASS_INTERPOLATE,
            0,
            0,
            &pipelineDescription,
            priv->effectContextId,
            &priv->pipelineInterpolate);
        if (err != FFXM_OK) {
            ffxmFrameInterpolationContextDestroy(context);
            return err;
        }
    }

    return FFXM_OK;
}

FfxmErrorCode ffxmFrameInterpolationContextDispatch(
    FfxmFrameInterpolationContext* context,
    const FfxmFrameInterpolationDispatchDescription* dispatchDescription)
{
    FFXM_RETURN_ON_ERROR(context, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(dispatchDescription, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(dispatchDescription->commandList, FFXM_ERROR_INVALID_ARGUMENT);
    FFXM_RETURN_ON_ERROR(dispatchDescription->currentBackBuffer.resource, FFXM_ERROR_INVALID_ARGUMENT);
    FFXM_RETURN_ON_ERROR(dispatchDescription->currentInterpolatedOutput.resource, FFXM_ERROR_INVALID_ARGUMENT);
    FFXM_RETURN_ON_ERROR(dispatchDescription->opticalFlowVector.resource, FFXM_ERROR_INVALID_ARGUMENT);
    FFXM_RETURN_ON_ERROR(dispatchDescription->dilatedDepth.resource, FFXM_ERROR_INVALID_ARGUMENT);
    FFXM_RETURN_ON_ERROR(dispatchDescription->dilatedMotionVectors.resource, FFXM_ERROR_INVALID_ARGUMENT);
    FFXM_RETURN_ON_ERROR(dispatchDescription->reconstructedPrevNearestDepth.resource, FFXM_ERROR_INVALID_ARGUMENT);

    auto* priv = reinterpret_cast<FfxmFrameInterpolationContext_Private*>(context);
    priv->frameIndex++;
    priv->firstExecution = false;

    // Stage constant buffer
    FrameInterpolationConstants constants = {};
    constants.iDisplaySize[0] = static_cast<int32_t>(priv->contextDescription.maxDisplaySize.width);
    constants.iDisplaySize[1] = static_cast<int32_t>(priv->contextDescription.maxDisplaySize.height);
    constants.fInvDisplaySize[0] = 1.0f / static_cast<float>(priv->contextDescription.maxDisplaySize.width);
    constants.fInvDisplaySize[1] = 1.0f / static_cast<float>(priv->contextDescription.maxDisplaySize.height);
    constants.fInterpolationTime = dispatchDescription->interpolationTime > 0.0f ? dispatchDescription->interpolationTime : 0.5f;
    constants.uReset = (dispatchDescription->reset || priv->firstExecution) ? 1 : 0;
    constants.fDepthThreshold = 0.05f;
    constants.fMotionScale = 1.0f;

    priv->constantBuffer.num32BitEntries = static_cast<uint32_t>(sizeof(FrameInterpolationConstants) / sizeof(uint32_t));
    std::memcpy(priv->constantBuffer.data, &constants, sizeof(FrameInterpolationConstants));

    // Register dynamic resources with backend if available
    FfxmResourceInternal regCurrentBackBuffer = {};
    FfxmResourceInternal regInterpolatedOutput = {};
    FfxmResourceInternal regOpticalFlow = {};
    FfxmResourceInternal regDilatedDepth = {};
    FfxmResourceInternal regDilatedMotionVectors = {};
    FfxmResourceInternal regPrevDepth = {};

    if (priv->backendInterface.fpRegisterResource) {
        FfxmErrorCode err = priv->backendInterface.fpRegisterResource(&priv->backendInterface, &dispatchDescription->currentBackBuffer, priv->effectContextId, &regCurrentBackBuffer);
        if (err != FFXM_OK) return err;
        err = priv->backendInterface.fpRegisterResource(&priv->backendInterface, &dispatchDescription->currentInterpolatedOutput, priv->effectContextId, &regInterpolatedOutput);
        if (err != FFXM_OK) return err;
        err = priv->backendInterface.fpRegisterResource(&priv->backendInterface, &dispatchDescription->opticalFlowVector, priv->effectContextId, &regOpticalFlow);
        if (err != FFXM_OK) return err;
        err = priv->backendInterface.fpRegisterResource(&priv->backendInterface, &dispatchDescription->dilatedDepth, priv->effectContextId, &regDilatedDepth);
        if (err != FFXM_OK) return err;
        err = priv->backendInterface.fpRegisterResource(&priv->backendInterface, &dispatchDescription->dilatedMotionVectors, priv->effectContextId, &regDilatedMotionVectors);
        if (err != FFXM_OK) return err;
        err = priv->backendInterface.fpRegisterResource(&priv->backendInterface, &dispatchDescription->reconstructedPrevNearestDepth, priv->effectContextId, &regPrevDepth);
        if (err != FFXM_OK) return err;
    }

    const uint32_t dispatchX = (priv->contextDescription.maxDisplaySize.width + 7) / 8;
    const uint32_t dispatchY = (priv->contextDescription.maxDisplaySize.height + 7) / 8;

    // Schedule Frame Interpolation GPU compute job
    if (priv->backendInterface.fpScheduleGpuJob) {
        FfxmGpuJobDescription jobInterp = {};
        jobInterp.jobType = FFXM_GPU_JOB_COMPUTE;
        jobInterp.computeJobDescriptor.pipeline = priv->pipelineInterpolate;
        jobInterp.computeJobDescriptor.dimensions[0] = dispatchX;
        jobInterp.computeJobDescriptor.dimensions[1] = dispatchY;
        jobInterp.computeJobDescriptor.dimensions[2] = 1;

        jobInterp.computeJobDescriptor.srvTextures[0] = regCurrentBackBuffer;
        std::wcscpy(jobInterp.computeJobDescriptor.srvTextureNames[0], L"r_current_back_buffer");

        jobInterp.computeJobDescriptor.srvTextures[1] = priv->historyColor;
        std::wcscpy(jobInterp.computeJobDescriptor.srvTextureNames[1], L"r_history_color");

        jobInterp.computeJobDescriptor.srvTextures[2] = regOpticalFlow;
        std::wcscpy(jobInterp.computeJobDescriptor.srvTextureNames[2], L"r_optical_flow");

        jobInterp.computeJobDescriptor.srvTextures[3] = regDilatedDepth;
        std::wcscpy(jobInterp.computeJobDescriptor.srvTextureNames[3], L"r_dilated_depth");

        jobInterp.computeJobDescriptor.srvTextures[4] = regDilatedMotionVectors;
        std::wcscpy(jobInterp.computeJobDescriptor.srvTextureNames[4], L"r_dilated_motion_vectors");

        jobInterp.computeJobDescriptor.srvTextures[5] = regPrevDepth;
        std::wcscpy(jobInterp.computeJobDescriptor.srvTextureNames[5], L"r_reconstructed_prev_nearest_depth");

        jobInterp.computeJobDescriptor.uavTextures[0] = regInterpolatedOutput;
        jobInterp.computeJobDescriptor.uavTextureMips[0] = 0;
        std::wcscpy(jobInterp.computeJobDescriptor.uavTextureNames[0], L"rw_interpolated_output");

        jobInterp.computeJobDescriptor.cbs[0] = priv->constantBuffer;
        std::wcscpy(jobInterp.computeJobDescriptor.cbNames[0], L"cbFrameInterpolation");

        FfxmErrorCode err = priv->backendInterface.fpScheduleGpuJob(&priv->backendInterface, &jobInterp);
        if (err != FFXM_OK) return err;

        // Schedule history update copy job: regCurrentBackBuffer -> priv->historyColor
        FfxmGpuJobDescription jobCopy = {};
        jobCopy.jobType = FFXM_GPU_JOB_COPY;
        jobCopy.copyJobDescriptor.src = regCurrentBackBuffer;
        jobCopy.copyJobDescriptor.dst = priv->historyColor;
        err = priv->backendInterface.fpScheduleGpuJob(&priv->backendInterface, &jobCopy);
        if (err != FFXM_OK) return err;
    }

    // Execute jobs
    if (priv->backendInterface.fpExecuteGpuJobs) {
        FfxmErrorCode err = priv->backendInterface.fpExecuteGpuJobs(&priv->backendInterface, dispatchDescription->commandList);
        if (err != FFXM_OK) return err;
    }

    // Unregister dynamic resources
    if (priv->backendInterface.fpUnregisterResources) {
        FfxmErrorCode err = priv->backendInterface.fpUnregisterResources(&priv->backendInterface, dispatchDescription->commandList, priv->effectContextId);
        if (err != FFXM_OK) return err;
    }

    return FFXM_OK;
}

FfxmErrorCode ffxmFrameInterpolationContextDestroy(FfxmFrameInterpolationContext* context)
{
    FFXM_RETURN_ON_ERROR(context, FFXM_ERROR_INVALID_POINTER);
    auto* priv = reinterpret_cast<FfxmFrameInterpolationContext_Private*>(context);

    // Destroy compute pipeline
    if (priv->backendInterface.fpDestroyPipeline) {
        if (priv->pipelineInterpolate.pipeline) {
            priv->backendInterface.fpDestroyPipeline(&priv->backendInterface, &priv->pipelineInterpolate, priv->effectContextId);
            priv->pipelineInterpolate.pipeline = nullptr;
        }
    }

    if (priv->backendInterface.fpDestroyResource) {
        if (priv->historyColor.internalIndex != -1) {
            priv->backendInterface.fpDestroyResource(&priv->backendInterface, priv->historyColor);
            priv->historyColor.internalIndex = -1;
        }
        if (priv->interpolatedStaging.internalIndex != -1) {
            priv->backendInterface.fpDestroyResource(&priv->backendInterface, priv->interpolatedStaging);
            priv->interpolatedStaging.internalIndex = -1;
        }
    }

    if (priv->backendInterface.fpDestroyBackendContext) {
        priv->backendInterface.fpDestroyBackendContext(&priv->backendInterface, priv->effectContextId);
    }

    std::memset(context, 0, sizeof(FfxmFrameInterpolationContext));
    return FFXM_OK;
}

} // namespace arm
