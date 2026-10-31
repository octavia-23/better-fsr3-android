// Copyright (c) 2024-2026 Advanced Micro Devices, Inc. All rights reserved.
// Copyright (c) 2024-2026 Arm Limited. All rights reserved.

#include "ffxm_fsr3upscaler_shaderblobs.h"
#include "host/ffxm_util.h"
#include "fsr3upscaler/ffxm_fsr3upscaler_private.h"
#include <ffxm_fsr3upscaler_prepare_inputs_pass_permutations.h>
#include <ffxm_fsr3upscaler_luma_pyramid_pass_permutations.h>
#include <ffxm_fsr3upscaler_shading_change_pyramid_pass_permutations.h>
#include <ffxm_fsr3upscaler_shading_change_pass_permutations.h>
#include <ffxm_fsr3upscaler_prepare_reactivity_pass_permutations.h>
#include <string.h>

namespace arm
{

#define POPULATE_PERMUTATION_KEY(options, key) \
    key.index = 0; \
    key.FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE = FFXM_CONTAINS_FLAG(options, FSR3UPSCALER_SHADER_PERMUTATION_USE_LANCZOS_TYPE); \
    key.FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT = FFXM_CONTAINS_FLAG(options, FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT); \
    key.FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS = FFXM_CONTAINS_FLAG(options, FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS); \
    key.FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS = FFXM_CONTAINS_FLAG(options, FSR3UPSCALER_SHADER_PERMUTATION_JITTER_MOTION_VECTORS); \
    key.FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH = FFXM_CONTAINS_FLAG(options, FSR3UPSCALER_SHADER_PERMUTATION_DEPTH_INVERTED); \
    key.FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING = FFXM_CONTAINS_FLAG(options, FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING);

static FfxmShaderBlob fsr3UpscalerGetPrepareInputsPassPermutationBlobByIndex(uint32_t permutationOptions)
{
    ffxm_fsr3upscaler_prepare_inputs_pass_PermutationKey key;
    POPULATE_PERMUTATION_KEY(permutationOptions, key);

    const int32_t tableIndex = g_ffxm_fsr3upscaler_prepare_inputs_pass_IndirectionTable[key.index];
    return POPULATE_SHADER_BLOB_FFX(g_ffxm_fsr3upscaler_prepare_inputs_pass_PermutationInfo, tableIndex);
}

static FfxmShaderBlob fsr3UpscalerGetLumaPyramidPassPermutationBlobByIndex(uint32_t permutationOptions)
{
    ffxm_fsr3upscaler_luma_pyramid_pass_PermutationKey key;
    POPULATE_PERMUTATION_KEY(permutationOptions, key);

    const int32_t tableIndex = g_ffxm_fsr3upscaler_luma_pyramid_pass_IndirectionTable[key.index];
    return POPULATE_SHADER_BLOB_FFX(g_ffxm_fsr3upscaler_luma_pyramid_pass_PermutationInfo, tableIndex);
}

static FfxmShaderBlob fsr3UpscalerGetShadingChangePyramidPassPermutationBlobByIndex(uint32_t permutationOptions)
{
    ffxm_fsr3upscaler_shading_change_pyramid_pass_PermutationKey key;
    POPULATE_PERMUTATION_KEY(permutationOptions, key);

    const int32_t tableIndex = g_ffxm_fsr3upscaler_shading_change_pyramid_pass_IndirectionTable[key.index];
    return POPULATE_SHADER_BLOB_FFX(g_ffxm_fsr3upscaler_shading_change_pyramid_pass_PermutationInfo, tableIndex);
}

static FfxmShaderBlob fsr3UpscalerGetShadingChangePassPermutationBlobByIndex(uint32_t permutationOptions)
{
    ffxm_fsr3upscaler_shading_change_pass_PermutationKey key;
    POPULATE_PERMUTATION_KEY(permutationOptions, key);

    const int32_t tableIndex = g_ffxm_fsr3upscaler_shading_change_pass_IndirectionTable[key.index];
    return POPULATE_SHADER_BLOB_FFX(g_ffxm_fsr3upscaler_shading_change_pass_PermutationInfo, tableIndex);
}

static FfxmShaderBlob fsr3UpscalerGetPrepareReactivityPassPermutationBlobByIndex(uint32_t permutationOptions)
{
    ffxm_fsr3upscaler_prepare_reactivity_pass_PermutationKey key;
    POPULATE_PERMUTATION_KEY(permutationOptions, key);

    const int32_t tableIndex = g_ffxm_fsr3upscaler_prepare_reactivity_pass_IndirectionTable[key.index];
    return POPULATE_SHADER_BLOB_FFX(g_ffxm_fsr3upscaler_prepare_reactivity_pass_PermutationInfo, tableIndex);
}

FfxmErrorCode fsr3UpscalerGetPermutationBlobByIndex(
    FfxmFsr3UpscalerPass passId,
    uint32_t permutationOptions,
    FfxmShaderBlob* outBlob)
{
    if (!outBlob) {
        return FFXM_ERROR_INVALID_POINTER;
    }

    switch (passId) {
    case FFXM_FSR3UPSCALER_PASS_PREPARE_INPUTS:
    {
        FfxmShaderBlob blob = fsr3UpscalerGetPrepareInputsPassPermutationBlobByIndex(permutationOptions);
        memcpy(outBlob, &blob, sizeof(FfxmShaderBlob));
        return FFXM_OK;
    }
    case FFXM_FSR3UPSCALER_PASS_LUMA_PYRAMID:
    {
        FfxmShaderBlob blob = fsr3UpscalerGetLumaPyramidPassPermutationBlobByIndex(permutationOptions);
        memcpy(outBlob, &blob, sizeof(FfxmShaderBlob));
        return FFXM_OK;
    }
    case FFXM_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID:
    {
        FfxmShaderBlob blob = fsr3UpscalerGetShadingChangePyramidPassPermutationBlobByIndex(permutationOptions);
        memcpy(outBlob, &blob, sizeof(FfxmShaderBlob));
        return FFXM_OK;
    }
    case FFXM_FSR3UPSCALER_PASS_SHADING_CHANGE:
    {
        FfxmShaderBlob blob = fsr3UpscalerGetShadingChangePassPermutationBlobByIndex(permutationOptions);
        memcpy(outBlob, &blob, sizeof(FfxmShaderBlob));
        return FFXM_OK;
    }
    case FFXM_FSR3UPSCALER_PASS_PREPARE_REACTIVITY:
    {
        FfxmShaderBlob blob = fsr3UpscalerGetPrepareReactivityPassPermutationBlobByIndex(permutationOptions);
        memcpy(outBlob, &blob, sizeof(FfxmShaderBlob));
        return FFXM_OK;
    }
    default:
        memset(outBlob, 0, sizeof(FfxmShaderBlob));
        return FFXM_ERROR_INVALID_ARGUMENT;
    }
}

FfxmErrorCode fsr3UpscalerIsWave64(uint32_t permutationOptions, bool& isWave64)
{
    isWave64 = FFXM_CONTAINS_FLAG(permutationOptions, FSR3UPSCALER_SHADER_PERMUTATION_FORCE_WAVE64);
    return FFXM_OK;
}

} // namespace arm
