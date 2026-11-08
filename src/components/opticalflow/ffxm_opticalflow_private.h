// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#pragma once

#include "host/ffxm_opticalflow.h"
#include <cstdint>

namespace arm {

struct FfxmOpticalflowContext_Private {
    FfxmInterface                      backendInterface;
    FfxmOpticalflowContextDescription  contextDescription;
    uint32_t                           effectContextId;
    uint32_t                           frameIndex;
    bool                               firstExecution;
    FfxmResourceInternal               opticalFlowPyramid;
    FfxmResourceInternal               sceneChangeHist;
};

static_assert(sizeof(FfxmOpticalflowContext_Private) <= sizeof(FfxmOpticalflowContext),
              "FfxmOpticalflowContext size is too small for private context");

} // namespace arm
