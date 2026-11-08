// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#pragma once

#include "host/ffxm_opticalflow.h"
#include <cstdint>

namespace arm {

struct OpticalflowConstants {
    int32_t  iResolution[2];
    float    fInvResolution[2];
    uint32_t uFrameIndex;
    float    fSceneChangeThreshold;
    float    padding[2];
};

struct FfxmOpticalflowContext_Private {
    FfxmInterface                      backendInterface;
    FfxmOpticalflowContextDescription  contextDescription;
    uint32_t                           effectContextId;
    uint32_t                           frameIndex;
    bool                               firstExecution;
    FfxmResourceInternal               opticalFlowPyramid;
    FfxmResourceInternal               sceneChangeHist;
    FfxmPipelineState                  pipelinePrepare;
    FfxmPipelineState                  pipelineSearch;
    FfxmConstantBuffer                 constantBuffer;
};

static_assert(sizeof(FfxmOpticalflowContext_Private) <= sizeof(FfxmOpticalflowContext),
              "FfxmOpticalflowContext size is too small for private context");

} // namespace arm
