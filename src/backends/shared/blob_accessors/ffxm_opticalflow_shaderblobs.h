// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>
#include <host/ffxm_interface.h>
#include <host/ffxm_opticalflow.h>

namespace arm
{

#if defined(__cplusplus)
extern "C" {
#endif

// Get a shader blob for the specified Optical Flow pass and permutation index.
FfxmErrorCode opticalflowGetPermutationBlobByIndex(
    FfxmOpticalflowPass passId,
    uint32_t permutationOptions,
    FfxmShaderBlob* outBlob);

// Check if Wave64 is requested on this permutation
FfxmErrorCode opticalflowIsWave64(uint32_t permutationOptions, bool& isWave64);

#if defined(__cplusplus)
}
#endif

} // namespace arm
