// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#include "ffxm_opticalflow_private.h"
#include "host/ffxm_assert.h"
#include "host/ffxm_util.h"
#include <cstring>

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

    // Register dynamic resources with backend if available
    if (priv->backendInterface.fpRegisterResource) {
        FfxmResourceInternal regRes = {};
        FfxmErrorCode err = priv->backendInterface.fpRegisterResource(&priv->backendInterface, &dispatchDescription->color, priv->effectContextId, &regRes);
        if (err != FFXM_OK) return err;
        err = priv->backendInterface.fpRegisterResource(&priv->backendInterface, &dispatchDescription->opticalFlowVector, priv->effectContextId, &regRes);
        if (err != FFXM_OK) return err;
    }

    // Schedule optical flow pyramid compute job
    if (priv->backendInterface.fpScheduleGpuJob) {
        FfxmGpuJobDescription job = {};
        job.jobType = FFXM_GPU_JOB_COMPUTE;
        job.computeJobDescriptor.dimensions[0] = (priv->contextDescription.resolution.width + 7) / 8;
        job.computeJobDescriptor.dimensions[1] = (priv->contextDescription.resolution.height + 7) / 8;
        job.computeJobDescriptor.dimensions[2] = 1;
        FfxmErrorCode err = priv->backendInterface.fpScheduleGpuJob(&priv->backendInterface, &job);
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
