// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#include "ffxm_opticalflow_private.h"
#include "host/ffxm_assert.h"
#include "host/ffxm_util.h"
#include <cstring>
#include <cwchar>

namespace arm {

FfxmErrorCode ffxmOpticalflowContextCreate(
    FfxmOpticalflowContext* context,
    const FfxmOpticalflowContextDescription* contextDescription)
{
    FFXM_RETURN_ON_ERROR(context, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(contextDescription, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(contextDescription->backendInterface.fpGetDeviceCapabilities, FFXM_ERROR_INCOMPLETE_INTERFACE);
    FFXM_RETURN_ON_ERROR(contextDescription->backendInterface.fpCreateBackendContext, FFXM_ERROR_INCOMPLETE_INTERFACE);
    FFXM_RETURN_ON_ERROR(contextDescription->backendInterface.fpDestroyBackendContext, FFXM_ERROR_INCOMPLETE_INTERFACE);

    if (contextDescription->resolution.width == 0 || contextDescription->resolution.height == 0) {
        return FFXM_ERROR_INVALID_ARGUMENT;
    }

    std::memset(context, 0, sizeof(FfxmOpticalflowContext));
    auto* priv = reinterpret_cast<FfxmOpticalflowContext_Private*>(context);
    priv->backendInterface = contextDescription->backendInterface;
    priv->contextDescription = *contextDescription;
    priv->effectContextId = 0;
    priv->frameIndex = 0;
    priv->firstExecution = true;
    priv->opticalFlowPyramid.internalIndex = -1;
    priv->sceneChangeHist.internalIndex = -1;

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
        resDesc.resourceDescription.format = FFXM_SURFACE_FORMAT_R16G16_FLOAT;
        resDesc.resourceDescription.width = contextDescription->resolution.width;
        resDesc.resourceDescription.height = contextDescription->resolution.height;
        resDesc.resourceDescription.mipCount = 4;
        resDesc.resourceDescription.flags = FFXM_RESOURCE_FLAGS_ALIASABLE;
        resDesc.resourceDescription.usage = FFXM_RESOURCE_USAGE_UAV;
        resDesc.name = L"OpticalFlow_Pyramid";

        err = priv->backendInterface.fpCreateResource(&priv->backendInterface, &resDesc, priv->effectContextId, &priv->opticalFlowPyramid);
        if (err != FFXM_OK) {
            priv->backendInterface.fpDestroyBackendContext(&priv->backendInterface, priv->effectContextId);
            return err;
        }

        // Allocate scene change history (R32U)
        resDesc.resourceDescription.format = FFXM_SURFACE_FORMAT_R32_UINT;
        resDesc.resourceDescription.width = 1;
        resDesc.resourceDescription.height = 1;
        resDesc.resourceDescription.mipCount = 1;
        resDesc.name = L"OpticalFlow_SceneChangeHist";
        err = priv->backendInterface.fpCreateResource(&priv->backendInterface, &resDesc, priv->effectContextId, &priv->sceneChangeHist);
        if (err != FFXM_OK) {
            if (priv->backendInterface.fpDestroyResource && priv->opticalFlowPyramid.internalIndex != -1) {
                priv->backendInterface.fpDestroyResource(&priv->backendInterface, priv->opticalFlowPyramid);
            }
            priv->backendInterface.fpDestroyBackendContext(&priv->backendInterface, priv->effectContextId);
            return err;
        }
    }

    // 4. Create compute pipelines for Prepare and Search passes
    if (priv->backendInterface.fpCreateComputePipeline) {
        FfxmPipelineDescription pipelineDescription = {};
        pipelineDescription.contextFlags = contextDescription->flags;
        pipelineDescription.stage = FFXM_BIND_COMPUTE_SHADER_STAGE;
        pipelineDescription.samplerCount = 0;
        pipelineDescription.samplers = nullptr;

        pipelineDescription.rootConstantBufferCount = 1;
        FfxmRootConstantDescription rootConstantDescs[1] = {
            { sizeof(OpticalflowConstants) / sizeof(uint32_t), FFXM_BIND_COMPUTE_SHADER_STAGE }
        };
        pipelineDescription.rootConstants = rootConstantDescs;

        std::wcscpy(pipelineDescription.name, L"OPTICALFLOW-PREPARE");
        err = priv->backendInterface.fpCreateComputePipeline(
            &priv->backendInterface,
            FFXM_EFFECT_OPTICALFLOW,
            FFXM_OPTICALFLOW_PASS_PREPARE,
            0,
            0,
            &pipelineDescription,
            priv->effectContextId,
            &priv->pipelinePrepare);
        if (err != FFXM_OK) {
            ffxmOpticalflowContextDestroy(context);
            return err;
        }

        std::wcscpy(pipelineDescription.name, L"OPTICALFLOW-SEARCH");
        err = priv->backendInterface.fpCreateComputePipeline(
            &priv->backendInterface,
            FFXM_EFFECT_OPTICALFLOW,
            FFXM_OPTICALFLOW_PASS_SEARCH,
            0,
            0,
            &pipelineDescription,
            priv->effectContextId,
            &priv->pipelineSearch);
        if (err != FFXM_OK) {
            ffxmOpticalflowContextDestroy(context);
            return err;
        }
    }

    return FFXM_OK;
}

FfxmErrorCode ffxmOpticalflowContextDispatch(
    FfxmOpticalflowContext* context,
    const FfxmOpticalflowDispatchDescription* dispatchDescription)
{
    FFXM_RETURN_ON_ERROR(context, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(dispatchDescription, FFXM_ERROR_INVALID_POINTER);
    FFXM_RETURN_ON_ERROR(dispatchDescription->commandList, FFXM_ERROR_INVALID_ARGUMENT);
    FFXM_RETURN_ON_ERROR(dispatchDescription->color.resource, FFXM_ERROR_INVALID_ARGUMENT);
    FFXM_RETURN_ON_ERROR(dispatchDescription->opticalFlowVector.resource, FFXM_ERROR_INVALID_ARGUMENT);

    auto* priv = reinterpret_cast<FfxmOpticalflowContext_Private*>(context);
    priv->frameIndex++;
    priv->firstExecution = false;

    // Stage constant buffer
    OpticalflowConstants constants = {};
    constants.iResolution[0] = static_cast<int32_t>(priv->contextDescription.resolution.width);
    constants.iResolution[1] = static_cast<int32_t>(priv->contextDescription.resolution.height);
    constants.fInvResolution[0] = 1.0f / static_cast<float>(priv->contextDescription.resolution.width);
    constants.fInvResolution[1] = 1.0f / static_cast<float>(priv->contextDescription.resolution.height);
    constants.uFrameIndex = priv->frameIndex;
    constants.fSceneChangeThreshold = 0.25f;

    priv->constantBuffer.num32BitEntries = static_cast<uint32_t>(sizeof(OpticalflowConstants) / sizeof(uint32_t));
    std::memcpy(priv->constantBuffer.data, &constants, sizeof(OpticalflowConstants));

    // Register dynamic resources with backend if available
    FfxmResourceInternal regColor = {};
    FfxmResourceInternal regFlowVector = {};
    FfxmResourceInternal regFlowScd = {};
    if (priv->backendInterface.fpRegisterResource) {
        FfxmErrorCode err = priv->backendInterface.fpRegisterResource(&priv->backendInterface, &dispatchDescription->color, priv->effectContextId, &regColor);
        if (err != FFXM_OK) return err;
        err = priv->backendInterface.fpRegisterResource(&priv->backendInterface, &dispatchDescription->opticalFlowVector, priv->effectContextId, &regFlowVector);
        if (err != FFXM_OK) return err;
        if (dispatchDescription->opticalFlowSCD.resource) {
            err = priv->backendInterface.fpRegisterResource(&priv->backendInterface, &dispatchDescription->opticalFlowSCD, priv->effectContextId, &regFlowScd);
            if (err != FFXM_OK) return err;
        }
    }

    const uint32_t dispatchX = (priv->contextDescription.resolution.width + 7) / 8;
    const uint32_t dispatchY = (priv->contextDescription.resolution.height + 7) / 8;

    // Schedule optical flow prepare and search GPU jobs
    if (priv->backendInterface.fpScheduleGpuJob) {
        // 1. Prepare job (luminance pyramid & SCD)
        FfxmGpuJobDescription jobPrepare = {};
        jobPrepare.jobType = FFXM_GPU_JOB_COMPUTE;
        jobPrepare.computeJobDescriptor.pipeline = priv->pipelinePrepare;
        jobPrepare.computeJobDescriptor.dimensions[0] = dispatchX;
        jobPrepare.computeJobDescriptor.dimensions[1] = dispatchY;
        jobPrepare.computeJobDescriptor.dimensions[2] = 1;

        jobPrepare.computeJobDescriptor.srvTextures[0] = regColor;
        std::wcscpy(jobPrepare.computeJobDescriptor.srvTextureNames[0], L"r_input_color");

        jobPrepare.computeJobDescriptor.uavTextures[0] = priv->opticalFlowPyramid;
        jobPrepare.computeJobDescriptor.uavTextureMips[0] = 0;
        std::wcscpy(jobPrepare.computeJobDescriptor.uavTextureNames[0], L"rw_optical_flow_pyramid");

        jobPrepare.computeJobDescriptor.uavTextures[1] = dispatchDescription->opticalFlowSCD.resource ? regFlowScd : priv->sceneChangeHist;
        jobPrepare.computeJobDescriptor.uavTextureMips[1] = 0;
        std::wcscpy(jobPrepare.computeJobDescriptor.uavTextureNames[1], L"rw_scene_change_detection");

        jobPrepare.computeJobDescriptor.cbs[0] = priv->constantBuffer;
        std::wcscpy(jobPrepare.computeJobDescriptor.cbNames[0], L"cbOpticalFlow");

        FfxmErrorCode err = priv->backendInterface.fpScheduleGpuJob(&priv->backendInterface, &jobPrepare);
        if (err != FFXM_OK) return err;

        // 2. Search job (hierarchical optical flow vectors)
        FfxmGpuJobDescription jobSearch = {};
        jobSearch.jobType = FFXM_GPU_JOB_COMPUTE;
        jobSearch.computeJobDescriptor.pipeline = priv->pipelineSearch;
        jobSearch.computeJobDescriptor.dimensions[0] = dispatchX;
        jobSearch.computeJobDescriptor.dimensions[1] = dispatchY;
        jobSearch.computeJobDescriptor.dimensions[2] = 1;

        jobSearch.computeJobDescriptor.srvTextures[0] = priv->opticalFlowPyramid;
        std::wcscpy(jobSearch.computeJobDescriptor.srvTextureNames[0], L"r_optical_flow_pyramid");

        jobSearch.computeJobDescriptor.uavTextures[0] = regFlowVector;
        jobSearch.computeJobDescriptor.uavTextureMips[0] = 0;
        std::wcscpy(jobSearch.computeJobDescriptor.uavTextureNames[0], L"rw_optical_flow");

        jobSearch.computeJobDescriptor.cbs[0] = priv->constantBuffer;
        std::wcscpy(jobSearch.computeJobDescriptor.cbNames[0], L"cbOpticalFlow");

        err = priv->backendInterface.fpScheduleGpuJob(&priv->backendInterface, &jobSearch);
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

FfxmErrorCode ffxmOpticalflowContextDestroy(FfxmOpticalflowContext* context)
{
    FFXM_RETURN_ON_ERROR(context, FFXM_ERROR_INVALID_POINTER);
    auto* priv = reinterpret_cast<FfxmOpticalflowContext_Private*>(context);

    // Destroy compute pipelines
    if (priv->backendInterface.fpDestroyPipeline) {
        if (priv->pipelinePrepare.pipeline) {
            priv->backendInterface.fpDestroyPipeline(&priv->backendInterface, &priv->pipelinePrepare, priv->effectContextId);
            priv->pipelinePrepare.pipeline = nullptr;
        }
        if (priv->pipelineSearch.pipeline) {
            priv->backendInterface.fpDestroyPipeline(&priv->backendInterface, &priv->pipelineSearch, priv->effectContextId);
            priv->pipelineSearch.pipeline = nullptr;
        }
    }

    if (priv->backendInterface.fpDestroyResource) {
        if (priv->opticalFlowPyramid.internalIndex != -1) {
            priv->backendInterface.fpDestroyResource(&priv->backendInterface, priv->opticalFlowPyramid);
            priv->opticalFlowPyramid.internalIndex = -1;
        }
        if (priv->sceneChangeHist.internalIndex != -1) {
            priv->backendInterface.fpDestroyResource(&priv->backendInterface, priv->sceneChangeHist);
            priv->sceneChangeHist.internalIndex = -1;
        }
    }

    if (priv->backendInterface.fpDestroyBackendContext) {
        priv->backendInterface.fpDestroyBackendContext(&priv->backendInterface, priv->effectContextId);
    }

    std::memset(context, 0, sizeof(FfxmOpticalflowContext));
    return FFXM_OK;
}

} // namespace arm
