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

#define FFXM_FRAMEINTERPOLATION_CONTEXT_SIZE 65536

typedef enum FfxmFrameInterpolationInitializationFlagBits {
    FFXM_FRAMEINTERPOLATION_ENABLE_HDR = (1 << 0),
    FFXM_FRAMEINTERPOLATION_ENABLE_HALF_PRECISION = (1 << 1)
} FfxmFrameInterpolationInitializationFlagBits;

typedef enum FfxmFrameInterpolationPass {
    FFXM_FRAMEINTERPOLATION_PASS_INTERPOLATE = 0, ///< Bidirectional frame interpolation pass
    FFXM_FRAMEINTERPOLATION_PASS_COUNT       = 1
} FfxmFrameInterpolationPass;

typedef struct FfxmFrameInterpolationContextDescription {
    uint32_t         flags;
    FfxmDimensions2D maxDisplaySize;
    FfxmInterface    backendInterface;
} FfxmFrameInterpolationContextDescription;

typedef struct FfxmFrameInterpolationDispatchDescription {
    FfxmCommandList  commandList;
    FfxmResource     currentBackBuffer;
    FfxmResource     currentInterpolatedOutput;
    FfxmResource     opticalFlowVector;
    FfxmResource     dilatedDepth;
    FfxmResource     dilatedMotionVectors;
    FfxmResource     reconstructedPrevNearestDepth;
    float            interpolationTime; // [0.0, 1.0], typical 0.5
    bool             reset;
} FfxmFrameInterpolationDispatchDescription;

typedef struct FFXM_ALIGNAS(8) FfxmFrameInterpolationContext {
    uint32_t data[FFXM_FRAMEINTERPOLATION_CONTEXT_SIZE];
} FfxmFrameInterpolationContext;

FFXM_API FfxmErrorCode ffxmFrameInterpolationContextCreate(
    FfxmFrameInterpolationContext* context,
    const FfxmFrameInterpolationContextDescription* contextDescription);

FFXM_API FfxmErrorCode ffxmFrameInterpolationContextDispatch(
    FfxmFrameInterpolationContext* context,
    const FfxmFrameInterpolationDispatchDescription* dispatchDescription);

FFXM_API FfxmErrorCode ffxmFrameInterpolationContextDestroy(
    FfxmFrameInterpolationContext* context);

#if defined(__cplusplus)
}
#endif

} // namespace arm
