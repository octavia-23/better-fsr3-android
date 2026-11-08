// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#include "ffxm_frameinterpolation_shaderblobs.h"
#include "host/ffxm_util.h"
#include <ffxm_frameinterpolation_pass_permutations.h>
#include <string.h>

namespace arm
{

static FfxmShaderBlob frameinterpolationGetInterpolatePassPermutationBlobByIndex(uint32_t permutationOptions)
{
    (void)permutationOptions;
    const int32_t tableIndex = g_ffxm_frameinterpolation_pass_IndirectionTable[0];
    return POPULATE_SHADER_BLOB_FFX(g_ffxm_frameinterpolation_pass_PermutationInfo, tableIndex);
}

FfxmErrorCode frameinterpolationGetPermutationBlobByIndex(
    FfxmFrameInterpolationPass passId,
    uint32_t permutationOptions,
    FfxmShaderBlob* outBlob)
{
    if (!outBlob) {
        return FFXM_ERROR_INVALID_POINTER;
    }

    switch (passId) {
    case FFXM_FRAMEINTERPOLATION_PASS_INTERPOLATE:
    {
        FfxmShaderBlob blob = frameinterpolationGetInterpolatePassPermutationBlobByIndex(permutationOptions);
        memcpy(outBlob, &blob, sizeof(FfxmShaderBlob));
        return FFXM_OK;
    }
    default:
        FFXM_ASSERT_MESSAGE(false, "Unknown frame interpolation pass id");
        memset(outBlob, 0, sizeof(FfxmShaderBlob));
        return FFXM_ERROR_INVALID_ARGUMENT;
    }
}

FfxmErrorCode frameinterpolationIsWave64(uint32_t permutationOptions, bool& isWave64)
{
    (void)permutationOptions;
    isWave64 = false;
    return FFXM_OK;
}

} // namespace arm
