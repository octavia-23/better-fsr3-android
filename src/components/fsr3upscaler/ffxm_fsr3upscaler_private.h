// Copyright  © 2024 Advanced Micro Devices, Inc.
// Copyright  © 2026 Arm Limited.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include "host/ffxm_fsr3upscaler.h"

namespace arm
{

/// An enumeration of all the permutations that can be passed to the FSR3 Upscaler algorithm.
///
/// FSR3 Upscaler features are organized through a set of pre-defined compile
/// permutation options that need to be specified. Which shader blob
/// is returned for pipeline creation will be determined by what combination
/// of shader permutations are enabled.
///
/// @ingroup FSR3Upscaler
typedef enum Fsr3UpscalerShaderPermutationOptions
{
    FSR3UPSCALER_SHADER_PERMUTATION_USE_LANCZOS_TYPE       = (1 << 0),  ///< Off means reference, On means LUT
    FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT        = (1 << 1),  ///< Enables the HDR code path
    FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS = (1 << 2),  ///< Indicates low resolution motion vectors provided
    FSR3UPSCALER_SHADER_PERMUTATION_JITTER_MOTION_VECTORS  = (1 << 3),  ///< Indicates motion vectors were generated with jitter
    FSR3UPSCALER_SHADER_PERMUTATION_DEPTH_INVERTED         = (1 << 4),  ///< Indicates input resources were generated with inverted depth
    FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING      = (1 << 5),  ///< Enables a supplementary sharpening pass
    FSR3UPSCALER_SHADER_PERMUTATION_FORCE_WAVE64           = (1 << 6),  ///< doesn't map to a define, selects different table
    FSR3UPSCALER_SHADER_PERMUTATION_ALLOW_FP16             = (1 << 7),  ///< Enables fast math computations where possible
} Fsr3UpscalerShaderPermutationOptions;

// Constants for FSR3 Upscaler dispatches. Must be kept in sync with cbFSR3Upscaler in shaders.
typedef struct Fsr3UpscalerConstants {
    int32_t                     renderSize[2];
    int32_t                     previousFrameRenderSize[2];

    int32_t                     upscaleSize[2];
    int32_t                     previousFrameUpscaleSize[2];

    int32_t                     maxRenderSize[2];
    int32_t                     maxUpscaleSize[2];

    float                       deviceToViewDepth[4];

    float                       jitterOffset[2];
    float                       previousFrameJitterOffset[2];

    float                       motionVectorScale[2];
    float                       downscaleFactor[2];

    float                       motionVectorJitterCancellation[2];
    float                       tanHalfFOV;
    float                       jitterPhaseCount;

    float                       deltaTime;
    float                       deltaPreExposure;
    float                       viewSpaceToMetersFactor;
    float                       frameIndex;

    float                       velocityFactor;
    float                       reactivenessScale;
    float                       shadingChangeScale;
    float                       accumulationAddedPerFrame;
    float                       minDisocclusionAccumulation;
} Fsr3UpscalerConstants;

// Constant buffer structures for secondary passes.
typedef struct Fsr3UpscalerSpdConstants {
    uint32_t mips;
    uint32_t numWorkGroups;
    uint32_t workGroupOffset[2];
    uint32_t renderSize[2];
} Fsr3UpscalerSpdConstants;

typedef struct Fsr3UpscalerRcasConstants {
    uint32_t rcasConfig[4];
} Fsr3UpscalerRcasConstants;

typedef struct Fsr3UpscalerGenerateReactiveConstants {
    float    scale;
    float    cutoffThreshold;
    float    binaryValue;
    uint32_t flags;
} Fsr3UpscalerGenerateReactiveConstants;

struct FfxmDeviceCapabilities;
struct FfxmPipelineState;

/// FfxmFsr3UpscalerContext_Private
/// The complete private implementation state of the FSR3 Upscaler context.
typedef struct FfxmFsr3UpscalerContext_Private {
    FfxmFsr3UpscalerContextDescription  contextDescription;
    FfxmUInt32                          effectContextId;
    Fsr3UpscalerConstants               constants;
    FfxmDevice                          device;
    FfxmDeviceCapabilities              deviceCapabilities;
    FfxmPipelineState                   pipelinePrepareInputs;
    FfxmPipelineState                   pipelinePrepareReactivity;
    FfxmPipelineState                   pipelineShadingChange;
    FfxmPipelineState                   pipelineAccumulate;
    FfxmPipelineState                   pipelineAccumulateSharpen;
    FfxmPipelineState                   pipelineRCAS;
    FfxmPipelineState                   pipelineLumaPyramid;
    FfxmPipelineState                   pipelineGenerateReactive;
    FfxmPipelineState                   pipelineTcrAutogenerate;
    FfxmPipelineState                   pipelineShadingChangePyramid;
    FfxmPipelineState                   pipelineLumaInstability;
    FfxmPipelineState                   pipelineDebugView;
    FfxmConstantBuffer                  constantBuffers[FFXM_FSR3UPSCALER_CONSTANTBUFFER_COUNT];
    FfxmResourceInternal                srvResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_COUNT];
    FfxmResourceInternal                uavResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_COUNT];
    FfxmResourceInternal                rtResources[FFXM_FSR3UPSCALER_RESOURCE_IDENTIFIER_COUNT];
    bool                                firstExecution;
    uint32_t                            resourceFrameIndex;
    float                               previousJitterOffset[2];
    float                               preExposure;
    float                               previousFramePreExposure;
} FfxmFsr3UpscalerContext_Private;

// Compile-time assertions verifying that private context size and alignment fit the public opaque storage.
static_assert(sizeof(FfxmFsr3UpscalerContext) >= sizeof(FfxmFsr3UpscalerContext_Private),
    "FfxmFsr3UpscalerContext storage size is insufficient for FfxmFsr3UpscalerContext_Private");
static_assert(alignof(FfxmFsr3UpscalerContext) >= alignof(FfxmFsr3UpscalerContext_Private),
    "FfxmFsr3UpscalerContext alignment does not meet FfxmFsr3UpscalerContext_Private alignment requirements");

FfxmErrorCode fsr3UpscalerCreate(FfxmFsr3UpscalerContext_Private* context, const FfxmFsr3UpscalerContextDescription* contextDescription);
FfxmErrorCode fsr3UpscalerDestroy(FfxmFsr3UpscalerContext_Private* context);

} // namespace arm
