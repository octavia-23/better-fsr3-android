// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#include "ffxm_opticalflow_shaderblobs.h"
#include "host/ffxm_util.h"
#include <ffxm_opticalflow_prepare_pass_permutations.h>
#include <ffxm_opticalflow_search_pass_permutations.h>
#include <string.h>

namespace arm
{

static FfxmShaderBlob opticalflowGetPreparePassPermutationBlobByIndex(uint32_t permutationOptions)
{
    (void)permutationOptions;
    const int32_t tableIndex = g_ffxm_opticalflow_prepare_pass_IndirectionTable[0];
    return POPULATE_SHADER_BLOB_FFX(g_ffxm_opticalflow_prepare_pass_PermutationInfo, tableIndex);
}

static FfxmShaderBlob opticalflowGetSearchPassPermutationBlobByIndex(uint32_t permutationOptions)
{
    (void)permutationOptions;
    const int32_t tableIndex = g_ffxm_opticalflow_search_pass_IndirectionTable[0];
    return POPULATE_SHADER_BLOB_FFX(g_ffxm_opticalflow_search_pass_PermutationInfo, tableIndex);
}

FfxmErrorCode opticalflowGetPermutationBlobByIndex(
    FfxmOpticalflowPass passId,
    uint32_t permutationOptions,
    FfxmShaderBlob* outBlob)
{
    if (!outBlob) {
        return FFXM_ERROR_INVALID_POINTER;
    }

    switch (passId) {
    case FFXM_OPTICALFLOW_PASS_PREPARE:
    {
        FfxmShaderBlob blob = opticalflowGetPreparePassPermutationBlobByIndex(permutationOptions);
        memcpy(outBlob, &blob, sizeof(FfxmShaderBlob));
        return FFXM_OK;
    }
    case FFXM_OPTICALFLOW_PASS_SEARCH:
    {
        FfxmShaderBlob blob = opticalflowGetSearchPassPermutationBlobByIndex(permutationOptions);
        memcpy(outBlob, &blob, sizeof(FfxmShaderBlob));
        return FFXM_OK;
    }
    default:
        FFXM_ASSERT_MESSAGE(false, "Unknown optical flow pass id");
        memset(outBlob, 0, sizeof(FfxmShaderBlob));
        return FFXM_ERROR_INVALID_ARGUMENT;
    }
}

FfxmErrorCode opticalflowIsWave64(uint32_t permutationOptions, bool& isWave64)
{
    (void)permutationOptions;
    isWave64 = false;
    return FFXM_OK;
}

} // namespace arm
