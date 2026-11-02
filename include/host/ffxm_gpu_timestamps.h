// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>
#include <stdbool.h>

#if defined(__cplusplus)
extern "C" {
#endif

/// Measurement modes for Vulkan GPU timestamp profiling.
typedef enum FfxmGpuProfilingMode {
    FFXM_GPU_PROFILING_MODE_DISABLED   = 0,  ///< Profiling disabled (production default)
    FFXM_GPU_PROFILING_MODE_TOTAL_ONLY = 1,  ///< Total FSR3 GPU job execution measurement
    FFXM_GPU_PROFILING_MODE_PER_PASS   = 2,  ///< Per-pass compute and clear diagnostic measurement
} FfxmGpuProfilingMode;

/// Query indexing scheme for Vulkan timestamp query pools.
/// Query pool size is FFXM_PROFILING_MAX_QUERIES (32 queries per frame-in-flight slot).
#define FFXM_PROFILING_QUERY_TOTAL_START         0
#define FFXM_PROFILING_QUERY_TOTAL_END           1

#define FFXM_PROFILING_PASS_COUNT                7
#define FFXM_PROFILING_QUERY_PASS_START(passIdx) (2 + ((passIdx) * 2))
#define FFXM_PROFILING_QUERY_PASS_END(passIdx)   (2 + ((passIdx) * 2) + 1)

#define FFXM_PROFILING_CLEAR_JOB_COUNT           4
#define FFXM_PROFILING_QUERY_CLEAR_START(clrIdx) (16 + ((clrIdx) * 2))
#define FFXM_PROFILING_QUERY_CLEAR_END(clrIdx)   (16 + ((clrIdx) * 2) + 1)

#define FFXM_PROFILING_MAX_QUERIES               32

/// Canonical names of the seven FSR 3.1.4 compute passes.
static const char* const kFfxmProfilingPassNames[FFXM_PROFILING_PASS_COUNT] = {
    "PrepareInputs",
    "LumaPyramid",
    "ShadingChangePyramid",
    "ShadingChange",
    "PrepareReactivity",
    "LumaInstability",
    "Accumulate"
};

/// Canonical names of the clear operations within the FSR 3.1.4 dispatch.
static const char* const kFfxmProfilingClearJobNames[FFXM_PROFILING_CLEAR_JOB_COUNT] = {
    "ClearReconstructedDepth",
    "ClearSpdAtomicCounterInitial",
    "ClearSpdMips",
    "ClearSpdAtomicCounterSecondary"
};

/// Result of timestamp delta calculation.
typedef struct FfxmGpuTimestampCalculationResult {
    bool     valid;
    uint64_t deltaTicks;
    double   durationNs;
    double   durationMs;
    double   durationUs;
} FfxmGpuTimestampCalculationResult;

/// Calculate elapsed duration between two Vulkan timestamps.
///
/// Handles:
/// - Counter wraparound across the queue's timestampValidBits.
/// - 64-bit valid bits case without undefined shifts.
/// - Graceful rejection if timestampValidBits == 0 or query availability is false.
/// - Scaling by device's timestampPeriod (in nanoseconds per tick).
static inline FfxmGpuTimestampCalculationResult ffxmCalculateGpuTimestampDelta(
    uint64_t startTick,
    uint64_t endTick,
    uint32_t validBits,
    float timestampPeriod,
    bool startAvailable,
    bool endAvailable)
{
    FfxmGpuTimestampCalculationResult result;
    result.valid = false;
    result.deltaTicks = 0;
    result.durationNs = 0.0;
    result.durationMs = 0.0;
    result.durationUs = 0.0;

    if (!startAvailable || !endAvailable || validBits == 0 || validBits > 64 || timestampPeriod <= 0.0f) {
        return result;
    }

    const uint64_t mask = (validBits == 64) ? ~0ULL : ((1ULL << validBits) - 1ULL);
    const uint64_t s = startTick & mask;
    const uint64_t e = endTick & mask;
    const uint64_t delta = (e - s) & mask;

    result.valid = true;
    result.deltaTicks = delta;
    result.durationNs = (double)delta * (double)timestampPeriod;
    result.durationMs = result.durationNs * 1e-6;
    result.durationUs = result.durationNs * 1e-3;
    return result;
}

#if defined(__cplusplus)
}
#endif
