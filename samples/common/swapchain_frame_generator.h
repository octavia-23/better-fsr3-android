// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#pragma once

#if defined(__ANDROID__)
#	define VK_USE_PLATFORM_ANDROID_KHR 1
#elif defined(_WIN32)
#	define VK_USE_PLATFORM_WIN32_KHR 1
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#endif
#include <vulkan/vulkan.h>

#include "swapchain_sync.h"
#include "host/ffxm_types.h"
#include "host/ffxm_error.h"
#include "host/ffxm_fsr3upscaler.h"
#include "host/ffxm_opticalflow.h"
#include "host/ffxm_frameinterpolation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace arm {

/// SwapchainFrameGenerator: Mobile Swapchain Proxy Presenter
/// Manages 2:1 presentation pacing (1 Real Frame : 1 Interpolated Frame).
/// Coordinates Optical Flow, Frame Interpolation, and Vulkan presentation cadence.
class SwapchainFrameGenerator {
public:
    struct Config {
        bool     enabled              = true;
        uint32_t targetDisplayFps     = 120;           // 120 Hz presentation (e.g. 60 FPS engine -> 120 FPS display)
        uint64_t targetFrameTimeNs    = 8333333ull;    // 1,000,000,000 / targetDisplayFps (~8.333 ms)
        bool     enableGpuPacingLock  = true;          // Prevent CPU from drifting > 1 VSYNC ahead
        float    interpolationTime    = 0.5f;          // Sub-frame interpolation ratio [0.0, 1.0]
    };

    struct Stats {
        uint64_t totalSourceFrames          = 0;
        uint64_t totalRealPresents          = 0;
        uint64_t totalInterpolatedPresents  = 0;
        uint64_t totalPresents              = 0;
        double   meanPresentationIntervalMs = 0.0;
        double   presentationVarianceMs2    = 0.0;
        double   presentationStdDevMs       = 0.0;
        double   presentationP99JitterMs    = 0.0;
        double   presentationMaxJitterMs    = 0.0;
        double   maxComputeOverheadMs       = 0.0;
        double   meanComputeOverheadMs      = 0.0;
        uint64_t totalPacingSleepNs         = 0;
        uint32_t pacingSleepCount           = 0;
    };

    struct TimingHooks {
        uint64_t (*getMonotonicTimeNs)() = nullptr;
        void (*sleepNs)(uint64_t ns) = nullptr;
    };

    struct FrameInput {
        FfxmCommandList commandList                   = nullptr;
        FfxmResource    currentRealFrameColor         = {};
        FfxmResource    dilatedDepth                  = {};
        FfxmResource    dilatedMotionVectors          = {};
        FfxmResource    reconstructedPrevNearestDepth = {};
        FfxmResource    interpolatedOutput            = {};
        FfxmResource    opticalFlowOutput             = {};
        bool            reset                         = false;
        float           frameTimeDeltaMs              = 16.666f;
    };

    struct FrameResult {
        bool          realFramePresented         = false;
        bool          interpolatedFramePresented = false;
        double        frameGenComputeOverheadMs  = 0.0;
        FfxmErrorCode status                     = FFXM_OK;
    };

    SwapchainFrameGenerator() = default;

    void init(SwapchainSync* sync,
              FfxmOpticalflowContext* ofContext = nullptr,
              FfxmFrameInterpolationContext* fiContext = nullptr,
              const Config& config = Config())
    {
        sync_ = sync;
        ofContext_ = ofContext;
        fiContext_ = fiContext;
        setConfig(config);
        resetState();
    }

    void setConfig(const Config& config)
    {
        config_ = config;
        if (config_.targetDisplayFps > 0) {
            config_.targetFrameTimeNs = 1000000000ull / config_.targetDisplayFps;
        } else {
            config_.targetFrameTimeNs = 8333333ull;
        }
    }

    const Config& config() const { return config_; }
    const Stats& stats() const { return stats_; }

    void setTimingHooks(const TimingHooks& hooks)
    {
        timingHooks_ = hooks;
    }

    void resetState()
    {
        stats_ = Stats();
        recentIntervalsNs_.clear();
        hasPreviousFrame_ = false;
        lastPresentTimeNs_ = 0;
        totalComputeOverheadMs_ = 0.0;
    }

    FrameResult processFrame(const FrameInput& input)
    {
        FrameResult result{};
        stats_.totalSourceFrames++;

        if (!sync_) {
            result.status = FFXM_ERROR_INVALID_POINTER;
            return result;
        }

        // Disabled mode: simple 1:1 real frame presentation
        if (!config_.enabled) {
            uint32_t imgIndex = 0;
            if (!sync_->beginFrame(&imgIndex)) {
                result.status = FFXM_ERROR_BACKEND_API_ERROR;
                return result;
            }
            if (!sync_->endFrame(imgIndex)) {
                result.status = FFXM_ERROR_BACKEND_API_ERROR;
                return result;
            }
            stats_.totalRealPresents++;
            stats_.totalPresents++;
            result.realFramePresented = true;
            result.status = FFXM_OK;
            return result;
        }

        // Camera cut / reset frame or first frame: cannot interpolate without prior frame
        if (input.reset || !hasPreviousFrame_) {
            uint32_t imgIndex = 0;
            if (!sync_->beginFrame(&imgIndex)) {
                result.status = FFXM_ERROR_BACKEND_API_ERROR;
                return result;
            }
            if (!sync_->endFrame(imgIndex)) {
                result.status = FFXM_ERROR_BACKEND_API_ERROR;
                return result;
            }
            stats_.totalRealPresents++;
            stats_.totalPresents++;
            hasPreviousFrame_ = true;
            lastPresentTimeNs_ = nowNs();
            result.realFramePresented = true;
            result.status = FFXM_OK;
            return result;
        }

        // -------------------------------------------------------------
        // Clean 2:1 Presentation Sequence (1 Real : 1 Interpolated)
        // -------------------------------------------------------------

        // 1. Generate Interpolated Frame
        uint64_t tStartCompute = nowNs();

        // Dispatch Optical Flow if context provided
        if (ofContext_ && input.commandList && input.currentRealFrameColor.resource && input.opticalFlowOutput.resource) {
            FfxmOpticalflowDispatchDescription ofDesc = {};
            ofDesc.commandList = input.commandList;
            ofDesc.color = input.currentRealFrameColor;
            ofDesc.opticalFlowVector = input.opticalFlowOutput;
            ofDesc.reset = false;
            FfxmErrorCode err = ffxmOpticalflowContextDispatch(ofContext_, &ofDesc);
            if (err != FFXM_OK) {
                result.status = err;
                // Fall back gracefully to real-only presentation
                presentRealOnly(result);
                return result;
            }
        }

        // Dispatch Frame Interpolation if context provided
        if (fiContext_ && input.commandList &&
            input.currentRealFrameColor.resource &&
            input.interpolatedOutput.resource &&
            input.dilatedDepth.resource &&
            input.dilatedMotionVectors.resource &&
            input.reconstructedPrevNearestDepth.resource)
        {
            FfxmFrameInterpolationDispatchDescription fiDesc = {};
            fiDesc.commandList = input.commandList;
            fiDesc.currentBackBuffer = input.currentRealFrameColor;
            fiDesc.currentInterpolatedOutput = input.interpolatedOutput;
            fiDesc.opticalFlowVector = input.opticalFlowOutput;
            fiDesc.dilatedDepth = input.dilatedDepth;
            fiDesc.dilatedMotionVectors = input.dilatedMotionVectors;
            fiDesc.reconstructedPrevNearestDepth = input.reconstructedPrevNearestDepth;
            fiDesc.interpolationTime = config_.interpolationTime;
            fiDesc.reset = false;
            FfxmErrorCode err = ffxmFrameInterpolationContextDispatch(fiContext_, &fiDesc);
            if (err != FFXM_OK) {
                result.status = err;
                presentRealOnly(result);
                return result;
            }
        }

        uint64_t tEndCompute = nowNs();
        double overheadMs = (tEndCompute >= tStartCompute) ? (double)(tEndCompute - tStartCompute) * 1e-6 : 0.0;
        result.frameGenComputeOverheadMs = overheadMs;
        totalComputeOverheadMs_ += overheadMs;
        stats_.meanComputeOverheadMs = totalComputeOverheadMs_ / (double)(stats_.totalInterpolatedPresents + 1);
        if (overheadMs > stats_.maxComputeOverheadMs) {
            stats_.maxComputeOverheadMs = overheadMs;
        }

        // 2. Pace & Present Interpolated Frame (Slot 1 of 2:1 sequence)
        paceToNextCadenceTick();

        uint32_t interpImageIndex = 0;
        if (!sync_->beginFrame(&interpImageIndex)) {
            result.status = FFXM_ERROR_BACKEND_API_ERROR;
            return result;
        }
        if (!sync_->endFrame(interpImageIndex)) {
            result.status = FFXM_ERROR_BACKEND_API_ERROR;
            return result;
        }
        recordPresentationInterval();
        stats_.totalInterpolatedPresents++;
        stats_.totalPresents++;
        result.interpolatedFramePresented = true;

        // 3. Pace & Present Real Frame (Slot 2 of 2:1 sequence)
        paceToNextCadenceTick();

        uint32_t realImageIndex = 0;
        if (!sync_->beginFrame(&realImageIndex)) {
            result.status = FFXM_ERROR_BACKEND_API_ERROR;
            return result;
        }
        if (!sync_->endFrame(realImageIndex)) {
            result.status = FFXM_ERROR_BACKEND_API_ERROR;
            return result;
        }
        recordPresentationInterval();
        stats_.totalRealPresents++;
        stats_.totalPresents++;
        result.realFramePresented = true;

        result.status = FFXM_OK;
        return result;
    }

private:
    void presentRealOnly(FrameResult& result)
    {
        uint32_t img = 0;
        if (sync_->beginFrame(&img) && sync_->endFrame(img)) {
            stats_.totalRealPresents++;
            stats_.totalPresents++;
            recordPresentationInterval();
            result.realFramePresented = true;
        }
    }

    uint64_t nowNs() const
    {
        if (timingHooks_.getMonotonicTimeNs) {
            return timingHooks_.getMonotonicTimeNs();
        }
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    void doSleepNs(uint64_t ns)
    {
        if (ns == 0) return;
        stats_.totalPacingSleepNs += ns;
        stats_.pacingSleepCount++;
        if (timingHooks_.sleepNs) {
            timingHooks_.sleepNs(ns);
        } else {
            std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
        }
    }

    void paceToNextCadenceTick()
    {
        if (!config_.enableGpuPacingLock || config_.targetFrameTimeNs == 0 || lastPresentTimeNs_ == 0) {
            return;
        }
        uint64_t currentTimeNs = nowNs();
        uint64_t elapsedNs = (currentTimeNs > lastPresentTimeNs_) ? (currentTimeNs - lastPresentTimeNs_) : 0;
        if (elapsedNs < config_.targetFrameTimeNs) {
            uint64_t sleepNs = config_.targetFrameTimeNs - elapsedNs;
            doSleepNs(sleepNs);
        }
    }

    void recordPresentationInterval()
    {
        uint64_t currentTimeNs = nowNs();
        if (lastPresentTimeNs_ > 0) {
            uint64_t deltaNs = (currentTimeNs >= lastPresentTimeNs_) ? (currentTimeNs - lastPresentTimeNs_) : 0;
            double deltaMs = (double)deltaNs * 1e-6;

            recentIntervalsNs_.push_back(deltaNs);
            if (recentIntervalsNs_.size() > 240) {
                recentIntervalsNs_.erase(recentIntervalsNs_.begin());
            }

            size_t n = recentIntervalsNs_.size();
            double sumMs = 0.0;
            std::vector<double> jittersMs;
            jittersMs.reserve(n);
            double targetMs = (double)config_.targetFrameTimeNs * 1e-6;

            for (uint64_t intNs : recentIntervalsNs_) {
                double intMs = (double)intNs * 1e-6;
                sumMs += intMs;
                double jitter = std::fabs(intMs - targetMs);
                jittersMs.push_back(jitter);
            }

            stats_.meanPresentationIntervalMs = sumMs / (double)n;

            double sumSqDiff = 0.0;
            for (uint64_t intNs : recentIntervalsNs_) {
                double intMs = (double)intNs * 1e-6;
                double diff = intMs - stats_.meanPresentationIntervalMs;
                sumSqDiff += diff * diff;
            }
            stats_.presentationVarianceMs2 = sumSqDiff / (double)n;
            stats_.presentationStdDevMs = std::sqrt(stats_.presentationVarianceMs2);

            std::sort(jittersMs.begin(), jittersMs.end());
            if (!jittersMs.empty()) {
                size_t idx99 = (size_t)std::ceil(0.99 * n) - 1;
                stats_.presentationP99JitterMs = jittersMs[(std::min)(idx99, n - 1)];
                stats_.presentationMaxJitterMs = jittersMs.back();
            }
        }
        lastPresentTimeNs_ = nowNs();
    }

    SwapchainSync*                 sync_                  = nullptr;
    FfxmOpticalflowContext*        ofContext_             = nullptr;
    FfxmFrameInterpolationContext* fiContext_             = nullptr;
    Config                         config_;
    Stats                          stats_;
    TimingHooks                    timingHooks_;
    bool                           hasPreviousFrame_      = false;
    uint64_t                       lastPresentTimeNs_     = 0;
    double                         totalComputeOverheadMs_= 0.0;
    std::vector<uint64_t>          recentIntervalsNs_;
};

} // namespace arm
