// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#pragma once

#include "host/ffxm_frameinterpolation.h"
#include <cstdint>

namespace arm {

struct FfxmFrameInterpolationContext_Private {
    FfxmInterface                                backendInterface;
    FfxmFrameInterpolationContextDescription     contextDescription;
    uint32_t                                     effectContextId;
    uint32_t                                     frameIndex;
    bool                                         firstExecution;
    FfxmResourceInternal                         historyColor;
    FfxmResourceInternal                         interpolatedStaging;
};

static_assert(sizeof(FfxmFrameInterpolationContext_Private) <= sizeof(FfxmFrameInterpolationContext),
              "FfxmFrameInterpolationContext size is too small for private context");

} // namespace arm
