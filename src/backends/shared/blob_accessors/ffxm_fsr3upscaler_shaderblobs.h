// Copyright (c) 2024-2026 Advanced Micro Devices, Inc. All rights reserved.
// Copyright (c) 2024-2026 Arm Limited. All rights reserved.
#pragma once

#include <stdint.h>
#include <host/ffxm_interface.h>
#include <host/ffxm_fsr3upscaler.h>

namespace arm
{

#if defined(__cplusplus)
extern "C" {
#endif



// Get a shader blob for the specified FSR3 pass and permutation index.
FfxmErrorCode fsr3UpscalerGetPermutationBlobByIndex(
    FfxmFsr3UpscalerPass passId,
    uint32_t permutationOptions,
    FfxmShaderBlob* outBlob);

// Check if Wave64 is requested on this permutation
FfxmErrorCode fsr3UpscalerIsWave64(uint32_t permutationOptions, bool& isWave64);

#if defined(__cplusplus)
}
#endif

} // namespace arm
