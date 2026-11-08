// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#pragma once

#include "ffxm_types.h"
#include "ffxm_error.h"
#include "ffxm_interface.h"

namespace arm {

#if !defined(FFXM_ALIGNAS)
#if defined(__cplusplus)
#define FFXM_ALIGNAS(x) alignas(x)
#elif defined(_MSC_VER)
#define FFXM_ALIGNAS(x) __declspec(align(x))
#elif defined(__GNUC__) || defined(__clang__)
#define FFXM_ALIGNAS(x) __attribute__((aligned(x)))
#else
#define FFXM_ALIGNAS(x)
#endif
#endif

#if defined(__cplusplus)
extern "C" {
#endif

#define FFXM_OPTICALFLOW_CONTEXT_SIZE 65536

typedef enum FfxmOpticalflowInitializationFlagBits {
    FFXM_OPTICALFLOW_ENABLE_HDR = (1 << 0),
    FFXM_OPTICALFLOW_ENABLE_HALF_PRECISION = (1 << 1)
} FfxmOpticalflowInitializationFlagBits;

typedef enum FfxmOpticalflowPass {
    FFXM_OPTICALFLOW_PASS_PREPARE = 0, ///< Prepares luminance pyramid and scene change metrics
    FFXM_OPTICALFLOW_PASS_SEARCH  = 1, ///< Hierarchical optical flow vector search
    FFXM_OPTICALFLOW_PASS_COUNT   = 2
} FfxmOpticalflowPass;

typedef struct FfxmOpticalflowContextDescription {
    uint32_t         flags;
    FfxmDimensions2D resolution;
    FfxmInterface    backendInterface;
} FfxmOpticalflowContextDescription;

typedef struct FfxmOpticalflowDispatchDescription {
    FfxmCommandList  commandList;
    FfxmResource     color;
    FfxmResource     opticalFlowVector;
    FfxmResource     opticalFlowSCD;
    bool             reset;
} FfxmOpticalflowDispatchDescription;

typedef struct FFXM_ALIGNAS(8) FfxmOpticalflowContext {
    uint32_t data[FFXM_OPTICALFLOW_CONTEXT_SIZE];
} FfxmOpticalflowContext;

FFXM_API FfxmErrorCode ffxmOpticalflowContextCreate(
    FfxmOpticalflowContext* context,
    const FfxmOpticalflowContextDescription* contextDescription);

FFXM_API FfxmErrorCode ffxmOpticalflowContextDispatch(
    FfxmOpticalflowContext* context,
    const FfxmOpticalflowDispatchDescription* dispatchDescription);

FFXM_API FfxmErrorCode ffxmOpticalflowContextDestroy(
    FfxmOpticalflowContext* context);

#if defined(__cplusplus)
}
#endif

} // namespace arm
