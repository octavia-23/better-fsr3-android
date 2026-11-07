// Copyright © 2026 Arm Limited.
// SPDX-License-Identifier: MIT

#pragma once

#include "common.h"
#include "host/ffxm_gpu_timestamps.h"
#include "host/backends/vk/ffxm_vk.h"
#include "vk_context.h"

#include <vector>
#include <string>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace arm {

struct FrameGpuTiming {
    uint32_t frameIndex = 0;
    bool     valid = false;
    double   totalMs = 0.0;
    double   passMs[FFXM_PROFILING_PASS_COUNT] = {0.0};
    double   clearMs[FFXM_PROFILING_CLEAR_JOB_COUNT] = {0.0};
};

struct ProfilingStats {
    uint32_t totalMeasuredFrames = 0;
    uint32_t validFrames = 0;
    uint32_t unavailableFrames = 0;
    double   medianMs = 0.0;
    double   p95Ms = 0.0;
    double   p99Ms = 0.0;
    double   meanMs = 0.0;
    double   minMs = 0.0;
    double   maxMs = 0.0;
    double   stddevMs = 0.0;
};

struct GpuProfilerConfig {
    FfxmGpuProfilingMode mode = FFXM_GPU_PROFILING_MODE_DISABLED;
    uint32_t warmupFrames = 200;
    uint32_t targetMeasuredFrames = 500;
    uint32_t runIndex = 1;
    std::string outputDir = "";
    std::string filePrefix = "";
};

class GpuProfiler {
public:
    GpuProfiler() = default;

    void init(const GpuProfilerConfig& config)
    {
        config_ = config;
        resetState();
    }

    /// Read configuration from Android system property or configuration file if available,
    /// otherwise preserving default (DISABLED).
    void initFromEnvironmentOrDefaults()
    {
        GpuProfilerConfig cfg;
        cfg.mode = FFXM_GPU_PROFILING_MODE_DISABLED;
        cfg.warmupFrames = 200;
        cfg.targetMeasuredFrames = 500;
        cfg.runIndex = 1;
        cfg.outputDir = "";
        cfg.filePrefix = "";

#if defined(__ANDROID__)
        // Check Android system property: debug.fsr3.profiling
        char propVal[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.fsr3.profiling", propVal) > 0) {
            std::string s(propVal);
            if (s == "total" || s == "total_only" || s == "1") {
                cfg.mode = FFXM_GPU_PROFILING_MODE_TOTAL_ONLY;
            } else if (s == "pass" || s == "per_pass" || s == "diagnostic" || s == "2") {
                cfg.mode = FFXM_GPU_PROFILING_MODE_PER_PASS;
            } else {
                cfg.mode = FFXM_GPU_PROFILING_MODE_DISABLED;
            }
        }

        // Check run index property: debug.fsr3.run
        char runVal[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.fsr3.run", runVal) > 0) {
            int r = std::atoi(runVal);
            if (r >= 1 && r <= 10) {
                cfg.runIndex = static_cast<uint32_t>(r);
            }
        }

        // Check frames property: debug.fsr3.frames
        char framesVal[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.fsr3.frames", framesVal) > 0) {
            int f = std::atoi(framesVal);
            if (f > 0) {
                cfg.targetMeasuredFrames = static_cast<uint32_t>(f);
            }
        }

        // Check warmup property: debug.fsr3.warmup
        char warmupVal[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.fsr3.warmup", warmupVal) > 0) {
            int w = std::atoi(warmupVal);
            if (w >= 0) {
                cfg.warmupFrames = static_cast<uint32_t>(w);
            }
        }

        // Check prefix property: debug.fsr3.prefix
        char prefixVal[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.fsr3.prefix", prefixVal) > 0) {
            cfg.filePrefix = prefixVal;
        }

        // Check output directory property: debug.fsr3.dir
        char dirVal[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.fsr3.dir", dirVal) > 0) {
            cfg.outputDir = dirVal;
        }
#endif

        // Check if optional config file exists on device or host
        std::vector<std::string> cfgPaths = {
            "/data/local/tmp/fsr3_profiling.cfg",
            "/sdcard/fsr3_profiling.cfg",
            "fsr3_profiling.cfg"
        };
        for (const auto& path : cfgPaths) {
            std::ifstream f(path);
            if (f.is_open()) {
                std::string line;
                while (std::getline(f, line)) {
                    auto eq = line.find('=');
                    if (eq != std::string::npos) {
                        std::string k = line.substr(0, eq);
                        std::string v = line.substr(eq + 1);
                        // trim whitespace
                        k.erase(0, k.find_first_not_of(" \t\r\n"));
                        k.erase(k.find_last_not_of(" \t\r\n") + 1);
                        v.erase(0, v.find_first_not_of(" \t\r\n"));
                        v.erase(v.find_last_not_of(" \t\r\n") + 1);

                        if (k == "mode") {
                            if (v == "total" || v == "total_only" || v == "1") {
                                cfg.mode = FFXM_GPU_PROFILING_MODE_TOTAL_ONLY;
                            } else if (v == "pass" || v == "per_pass" || v == "diagnostic" || v == "2") {
                                cfg.mode = FFXM_GPU_PROFILING_MODE_PER_PASS;
                            } else {
                                cfg.mode = FFXM_GPU_PROFILING_MODE_DISABLED;
                            }
                        } else if (k == "warmup") {
                            cfg.warmupFrames = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                        } else if (k == "frames") {
                            cfg.targetMeasuredFrames = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                        } else if (k == "run") {
                            cfg.runIndex = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                        } else if (k == "output_dir" || k == "dir") {
                            cfg.outputDir = v;
                        } else if (k == "prefix" || k == "filename") {
                            cfg.filePrefix = v;
                        }
                    }
                }
                break;
            }
        }

        init(cfg);
    }

    void resetState()
    {
        samples_.clear();
        samples_.reserve(config_.targetMeasuredFrames);
        warmupRemaining_ = config_.warmupFrames;
        unavailableCount_ = 0;
        completed_ = false;
        reportGenerated_ = false;

        for (uint32_t i = 0; i < kMaxSlots; ++i) {
            slotStates_[i] = SlotState{};
        }
    }

    FfxmGpuProfilingMode mode() const { return config_.mode; }
    void setMode(FfxmGpuProfilingMode mode) { config_.mode = mode; }

    bool isComplete() const { return completed_; }
    size_t sampleCount() const { return samples_.size(); }
    const std::vector<FrameGpuTiming>& samples() const { return samples_; }
    const GpuProfilerConfig& config() const { return config_; }

    static uint32_t queryCountForMode(FfxmGpuProfilingMode mode)
    {
        switch (mode) {
        case FFXM_GPU_PROFILING_MODE_TOTAL_ONLY:
            return 2;
        case FFXM_GPU_PROFILING_MODE_PER_PASS:
            return 24; // 2 total + 14 compute pass + 8 clear jobs
        default:
            return 0;
        }
    }

    /// Called at beginFrame after the fence wait.
    /// 1. Reads available query results from previous submission on frameSlot.
    /// 2. If profiling active and not yet complete, resets the frameSlot query range in cmd
    ///    and populates outDesc.
    bool onBeginFrame(uint32_t frameSlot,
                      VkCommandBuffer cmd,
                      const VkContext& ctx,
                      arm::FfxmVkProfilingDescription* outDesc)
    {
        if (frameSlot >= kMaxSlots) {
            if (outDesc) *outDesc = FfxmVkProfilingDescription{};
            return false;
        }

        // 1. Process previous submission on this slot if pending
        SlotState& slot = slotStates_[frameSlot];
        if (slot.hasPendingSubmission && slot.submittedMode != FFXM_GPU_PROFILING_MODE_DISABLED) {
            collectSlotResults(frameSlot, slot, ctx);
            slot.hasPendingSubmission = false;
        }

        // 2. Prepare profiling descriptor for upcoming frame dispatch
        const bool active = (config_.mode != FFXM_GPU_PROFILING_MODE_DISABLED) &&
                            (!completed_) &&
                            (ctx.timestampValidBits() > 0) &&
                            (ctx.queryPool(frameSlot) != VK_NULL_HANDLE);

        if (!active) {
            if (outDesc) {
                outDesc->mode = FFXM_GPU_PROFILING_MODE_DISABLED;
                outDesc->queryPool = VK_NULL_HANDLE;
                outDesc->startQueryIndex = 0;
                outDesc->maxQueryCount = 0;
            }
            return false;
        }

        const uint32_t qCount = queryCountForMode(config_.mode);

        // Reset query range inside command buffer before recording timestamp writes
        ctx.cmdResetQueryPool(cmd, frameSlot, 0, qCount);

        if (outDesc) {
            outDesc->mode = config_.mode;
            outDesc->queryPool = ctx.queryPool(frameSlot);
            outDesc->startQueryIndex = 0;
            outDesc->maxQueryCount = qCount;
        }
        return true;
    }

    /// Called when frame submission to Vulkan queue succeeds.
    void onFrameSubmitted(uint32_t frameSlot, FfxmGpuProfilingMode mode, uint32_t frameNumber)
    {
        if (frameSlot < kMaxSlots) {
            slotStates_[frameSlot].hasPendingSubmission = true;
            slotStates_[frameSlot].submittedMode        = mode;
            slotStates_[frameSlot].queryCount           = queryCountForMode(mode);
            slotStates_[frameSlot].frameNumber          = frameNumber;
        }
    }

    /// Called if frame dispatch fails or frame is aborted.
    void onFrameAborted(uint32_t frameSlot)
    {
        if (frameSlot < kMaxSlots) {
            slotStates_[frameSlot].hasPendingSubmission = false;
        }
    }

    /// Called if swapchain is recreated or context is reset.
    void onContextReset()
    {
        for (uint32_t i = 0; i < kMaxSlots; ++i) {
            slotStates_[i].hasPendingSubmission = false;
        }
    }

    /// Compute statistics on collected samples.
    static ProfilingStats computeStats(std::vector<double> values, uint32_t totalSamples, uint32_t unavailableCount)
    {
        ProfilingStats s{};
        s.totalMeasuredFrames = totalSamples;
        s.validFrames = static_cast<uint32_t>(values.size());
        s.unavailableFrames = unavailableCount;
        if (values.empty()) return s;

        std::sort(values.begin(), values.end());
        const size_t n = values.size();
        s.minMs = values.front();
        s.maxMs = values.back();

        if (n % 2 == 1) {
            s.medianMs = values[n / 2];
        } else {
            s.medianMs = (values[(n / 2) - 1] + values[n / 2]) * 0.5;
        }

        auto pIndex = [n](double p) -> size_t {
            size_t idx = static_cast<size_t>(std::ceil((p / 100.0) * static_cast<double>(n)));
            if (idx == 0) return 0;
            return std::min(idx - 1, n - 1);
        };
        s.p95Ms = values[pIndex(95.0)];
        s.p99Ms = values[pIndex(99.0)];

        double sum = std::accumulate(values.begin(), values.end(), 0.0);
        s.meanMs = sum / static_cast<double>(n);

        double var = 0.0;
        for (double v : values) {
            double d = v - s.meanMs;
            var += d * d;
        }
        s.stddevMs = std::sqrt(var / static_cast<double>(n));
        return s;
    }

    /// Format and save aggregated report, and log summary to logcat.
    bool saveReports(const VkContext& ctx, VkExtent2D renderExtent, VkExtent2D displayExtent)
    {
        if (reportGenerated_) return true;
        reportGenerated_ = true;

        std::vector<double> totalTimings;
        std::vector<double> passTimings[FFXM_PROFILING_PASS_COUNT];
        std::vector<double> clearTimings[FFXM_PROFILING_CLEAR_JOB_COUNT];

        for (const auto& s : samples_) {
            if (s.valid) {
                totalTimings.push_back(s.totalMs);
                if (config_.mode == FFXM_GPU_PROFILING_MODE_PER_PASS) {
                    for (uint32_t p = 0; p < FFXM_PROFILING_PASS_COUNT; ++p) {
                        passTimings[p].push_back(s.passMs[p]);
                    }
                    for (uint32_t c = 0; c < FFXM_PROFILING_CLEAR_JOB_COUNT; ++c) {
                        clearTimings[c].push_back(s.clearMs[c]);
                    }
                }
            }
        }

        const uint32_t totalSamples = static_cast<uint32_t>(samples_.size());
        ProfilingStats totalStats = computeStats(totalTimings, totalSamples, unavailableCount_);

        ProfilingStats passStats[FFXM_PROFILING_PASS_COUNT] = {};
        ProfilingStats clearStats[FFXM_PROFILING_CLEAR_JOB_COUNT] = {};
        if (config_.mode == FFXM_GPU_PROFILING_MODE_PER_PASS) {
            for (uint32_t p = 0; p < FFXM_PROFILING_PASS_COUNT; ++p) {
                passStats[p] = computeStats(passTimings[p], totalSamples, unavailableCount_);
            }
            for (uint32_t c = 0; c < FFXM_PROFILING_CLEAR_JOB_COUNT; ++c) {
                clearStats[c] = computeStats(clearTimings[c], totalSamples, unavailableCount_);
            }
        }

        // 1. Log human-readable summary
        logSummary(ctx, renderExtent, displayExtent, totalStats, passStats, clearStats);

        // 2. Generate CSV string
        std::string csvData = generateCsv(config_.mode);

        // 3. Generate JSON string
        std::string jsonData = generateJson(ctx, renderExtent, displayExtent, totalStats, passStats, clearStats);

        // 4. Save to files
        std::string baseFilename;
        if (!config_.filePrefix.empty()) {
            if (config_.filePrefix == "diagnostic" ||
                (config_.filePrefix.length() >= 10 &&
                 config_.filePrefix.rfind("diagnostic") == config_.filePrefix.length() - 10)) {
                baseFilename = config_.filePrefix;
            } else {
                std::string runSuffix = (config_.mode == FFXM_GPU_PROFILING_MODE_TOTAL_ONLY)
                    ? ("_total_run" + std::to_string(config_.runIndex))
                    : "_diagnostic";
                baseFilename = config_.filePrefix + runSuffix;
            }
        } else {
            std::string devName = ctx.deviceName();
            std::string devPrefix = "fsr3_gpu";
            if (devName.find("650") != std::string::npos) {
                devPrefix = "fsr3_mi11x";
            } else if (devName.find("735") != std::string::npos) {
                devPrefix = "fsr3_f6";
            }
            std::string runSuffix = (config_.mode == FFXM_GPU_PROFILING_MODE_TOTAL_ONLY)
                ? ("_total_run" + std::to_string(config_.runIndex))
                : "_diagnostic";
            baseFilename = devPrefix + runSuffix;
        }

        std::vector<std::string> targetDirs;
        if (!config_.outputDir.empty()) {
            targetDirs.push_back(config_.outputDir);
        }
#if defined(__ANDROID__)
        targetDirs.push_back("/data/user/0/com.arm.asr.sample/files");
#endif
        targetDirs.push_back("/data/local/tmp");
        targetDirs.push_back("/sdcard/Download");
        targetDirs.push_back(".");

        bool saved = false;
        for (const auto& dir : targetDirs) {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);

            std::string csvPath = dir + "/" + baseFilename + ".csv";
            std::string jsonPath = dir + "/" + baseFilename + ".json";

            std::ofstream fCsv(csvPath);
            if (fCsv.is_open()) {
                fCsv << csvData;
                fCsv.close();

                std::ofstream fJson(jsonPath);
                if (fJson.is_open()) {
                    fJson << jsonData;
                    fJson.close();
                    LOGI("GPU profiling results successfully saved to: %s and %s", csvPath.c_str(), jsonPath.c_str());
                    saved = true;
                    break;
                }
            }
        }

        if (!saved) {
            LOGW("Could not save profiling reports to filesystem; raw JSON printed below:");
            LOGI("%s", jsonData.c_str());
        }

        return saved;
    }

private:
    struct SlotState {
        bool                 hasPendingSubmission = false;
        FfxmGpuProfilingMode submittedMode = FFXM_GPU_PROFILING_MODE_DISABLED;
        uint32_t             queryCount = 0;
        uint32_t             frameNumber = 0;
    };

    struct QueryResultWithAvailability {
        uint64_t timestamp;
        uint64_t available;
    };

    static constexpr uint32_t kMaxSlots = 4;

    GpuProfilerConfig           config_{};
    SlotState                   slotStates_[kMaxSlots]{};
    std::vector<FrameGpuTiming> samples_{};
    uint32_t                    warmupRemaining_ = 0;
    uint32_t                    unavailableCount_ = 0;
    bool                        completed_ = false;
    bool                        reportGenerated_ = false;

    void collectSlotResults(uint32_t frameSlot, const SlotState& slot, const VkContext& ctx)
    {
        QueryResultWithAvailability results[FFXM_PROFILING_MAX_QUERIES] = {};
        const uint32_t qCount = slot.queryCount;
        if (qCount == 0) return;

        VkResult res = ctx.getQueryPoolResults(
            frameSlot,
            0,
            qCount,
            sizeof(results),
            results,
            sizeof(QueryResultWithAvailability),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT
        );

        if (res != VK_SUCCESS && res != VK_NOT_READY) {
            unavailableCount_++;
            return;
        }

        const uint32_t validBits = ctx.timestampValidBits();
        const float period = ctx.timestampPeriod();

        // Check total timestamp pair
        const bool startAvail = (results[FFXM_PROFILING_QUERY_TOTAL_START].available != 0);
        const bool endAvail   = (results[FFXM_PROFILING_QUERY_TOTAL_END].available != 0);

        FfxmGpuTimestampCalculationResult totalRes = ffxmCalculateGpuTimestampDelta(
            results[FFXM_PROFILING_QUERY_TOTAL_START].timestamp,
            results[FFXM_PROFILING_QUERY_TOTAL_END].timestamp,
            validBits,
            period,
            startAvail,
            endAvail
        );

        if (!totalRes.valid) {
            if (warmupRemaining_ == 0) {
                unavailableCount_++;
            }
            return;
        }

        if (warmupRemaining_ > 0) {
            warmupRemaining_--;
            return;
        }

        if (samples_.size() >= config_.targetMeasuredFrames) {
            return;
        }

        FrameGpuTiming timing{};
        timing.frameIndex = slot.frameNumber;
        timing.valid      = true;
        timing.totalMs    = totalRes.durationMs;

        if (slot.submittedMode == FFXM_GPU_PROFILING_MODE_PER_PASS) {
            for (uint32_t p = 0; p < FFXM_PROFILING_PASS_COUNT; ++p) {
                const uint32_t sq = FFXM_PROFILING_QUERY_PASS_START(p);
                const uint32_t eq = FFXM_PROFILING_QUERY_PASS_END(p);
                FfxmGpuTimestampCalculationResult pr = ffxmCalculateGpuTimestampDelta(
                    results[sq].timestamp,
                    results[eq].timestamp,
                    validBits,
                    period,
                    results[sq].available != 0,
                    results[eq].available != 0
                );
                timing.passMs[p] = pr.valid ? pr.durationMs : 0.0;
            }

            for (uint32_t c = 0; c < FFXM_PROFILING_CLEAR_JOB_COUNT; ++c) {
                const uint32_t sq = FFXM_PROFILING_QUERY_CLEAR_START(c);
                const uint32_t eq = FFXM_PROFILING_QUERY_CLEAR_END(c);
                FfxmGpuTimestampCalculationResult cr = ffxmCalculateGpuTimestampDelta(
                    results[sq].timestamp,
                    results[eq].timestamp,
                    validBits,
                    period,
                    results[sq].available != 0,
                    results[eq].available != 0
                );
                timing.clearMs[c] = cr.valid ? cr.durationMs : 0.0;
            }
        }

        samples_.push_back(timing);

        if (samples_.size() >= config_.targetMeasuredFrames) {
            completed_ = true;
            LOGI("GPU Profiling run completed (%zu frames collected)", samples_.size());
        }
    }

    void logSummary(const VkContext& ctx,
                    VkExtent2D renderExtent,
                    VkExtent2D displayExtent,
                    const ProfilingStats& totalStats,
                    const ProfilingStats passStats[FFXM_PROFILING_PASS_COUNT],
                    const ProfilingStats clearStats[FFXM_PROFILING_CLEAR_JOB_COUNT])
    {
        const uint32_t driverMajor = VK_API_VERSION_MAJOR(ctx.driverVersion());
        const uint32_t driverMinor = VK_API_VERSION_MINOR(ctx.driverVersion());
        const uint32_t driverPatch = VK_API_VERSION_PATCH(ctx.driverVersion());

        const uint32_t apiMajor = VK_API_VERSION_MAJOR(ctx.apiVersion());
        const uint32_t apiMinor = VK_API_VERSION_MINOR(ctx.apiVersion());
        const uint32_t apiPatch = VK_API_VERSION_PATCH(ctx.apiVersion());

        const char* modeStr = (config_.mode == FFXM_GPU_PROFILING_MODE_TOTAL_ONLY) ? "TOTAL_ONLY" : "PER_PASS_DIAGNOSTIC";

        LOGI("================ FSR 3.1.4 GPU PROFILING REPORT ================");
        LOGI("Device: %s (Vulkan %u.%u.%u, Driver %u.%u.%u)",
             ctx.deviceName().c_str(), apiMajor, apiMinor, apiPatch, driverMajor, driverMinor, driverPatch);
        LOGI("Timestamp: %.3f ns/tick, %u valid bits", ctx.timestampPeriod(), ctx.timestampValidBits());
        LOGI("Resolution: Render %ux%u -> Display %ux%u",
             renderExtent.width, renderExtent.height, displayExtent.width, displayExtent.height);
        LOGI("Mode: %s (Run %u)", modeStr, config_.runIndex);
        LOGI("Frames: %u measured (%u warmup), %u valid, %u unavailable",
             totalStats.totalMeasuredFrames, config_.warmupFrames, totalStats.validFrames, totalStats.unavailableFrames);

        LOGI("--- TOTAL FSR3 GPU TIME ---");
        LOGI("  Median: %.3f ms", totalStats.medianMs);
        LOGI("  p95:    %.3f ms", totalStats.p95Ms);
        LOGI("  p99:    %.3f ms", totalStats.p99Ms);
        LOGI("  Mean:   %.3f ms (stddev: %.3f ms)", totalStats.meanMs, totalStats.stddevMs);
        LOGI("  Min:    %.3f ms | Max: %.3f ms", totalStats.minMs, totalStats.maxMs);

        if (config_.mode == FFXM_GPU_PROFILING_MODE_PER_PASS) {
            LOGI("--- INDIVIDUAL COMPUTE PASSES ---");
            for (uint32_t p = 0; p < FFXM_PROFILING_PASS_COUNT; ++p) {
                LOGI("  Pass %u [%s]: Median %.3f ms | p95 %.3f ms | Mean %.3f ms",
                     p, kFfxmProfilingPassNames[p], passStats[p].medianMs, passStats[p].p95Ms, passStats[p].meanMs);
            }

            LOGI("--- INDIVIDUAL CLEAR OPERATIONS ---");
            for (uint32_t c = 0; c < FFXM_PROFILING_CLEAR_JOB_COUNT; ++c) {
                LOGI("  Clear %u [%s]: Median %.3f ms | p95 %.3f ms | Mean %.3f ms",
                     c, kFfxmProfilingClearJobNames[c], clearStats[c].medianMs, clearStats[c].p95Ms, clearStats[c].meanMs);
            }
        }
        LOGI("=================================================================");
    }

    std::string generateCsv(FfxmGpuProfilingMode mode) const
    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(4);

        if (mode == FFXM_GPU_PROFILING_MODE_TOTAL_ONLY) {
            ss << "frame,total_ms,valid\n";
            for (const auto& s : samples_) {
                ss << s.frameIndex << "," << s.totalMs << "," << (s.valid ? 1 : 0) << "\n";
            }
        } else {
            ss << "frame,total_ms";
            for (uint32_t p = 0; p < FFXM_PROFILING_PASS_COUNT; ++p) {
                ss << "," << kFfxmProfilingPassNames[p] << "_ms";
            }
            for (uint32_t c = 0; c < FFXM_PROFILING_CLEAR_JOB_COUNT; ++c) {
                ss << "," << kFfxmProfilingClearJobNames[c] << "_ms";
            }
            ss << ",valid\n";

            for (const auto& s : samples_) {
                ss << s.frameIndex << "," << s.totalMs;
                for (uint32_t p = 0; p < FFXM_PROFILING_PASS_COUNT; ++p) {
                    ss << "," << s.passMs[p];
                }
                for (uint32_t c = 0; c < FFXM_PROFILING_CLEAR_JOB_COUNT; ++c) {
                    ss << "," << s.clearMs[c];
                }
                ss << "," << (s.valid ? 1 : 0) << "\n";
            }
        }
        return ss.str();
    }

    std::string generateJson(const VkContext& ctx,
                             VkExtent2D renderExtent,
                             VkExtent2D displayExtent,
                             const ProfilingStats& totalStats,
                             const ProfilingStats passStats[FFXM_PROFILING_PASS_COUNT],
                             const ProfilingStats clearStats[FFXM_PROFILING_CLEAR_JOB_COUNT]) const
    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(4);

        const char* modeStr = (config_.mode == FFXM_GPU_PROFILING_MODE_TOTAL_ONLY) ? "total_only" : "per_pass_diagnostic";

        ss << "{\n";
        ss << "  \"device\": {\n";
        ss << "    \"name\": \"" << ctx.deviceName() << "\",\n";
        ss << "    \"driverVersion\": " << ctx.driverVersion() << ",\n";
        ss << "    \"apiVersion\": " << ctx.apiVersion() << ",\n";
        ss << "    \"timestampValidBits\": " << ctx.timestampValidBits() << ",\n";
        ss << "    \"timestampPeriodNs\": " << ctx.timestampPeriod() << "\n";
        ss << "  },\n";

        ss << "  \"workload\": {\n";
        ss << "    \"effect\": \"FSR 3.1.4\",\n";
        ss << "    \"mode\": \"" << modeStr << "\",\n";
        ss << "    \"runIndex\": " << config_.runIndex << ",\n";
        ss << "    \"renderExtent\": {\"width\": " << renderExtent.width << ", \"height\": " << renderExtent.height << "},\n";
        ss << "    \"displayExtent\": {\"width\": " << displayExtent.width << ", \"height\": " << displayExtent.height << "},\n";
        ss << "    \"warmupFrames\": " << config_.warmupFrames << ",\n";
        ss << "    \"targetMeasuredFrames\": " << config_.targetMeasuredFrames << ",\n";
        ss << "    \"validFrames\": " << totalStats.validFrames << ",\n";
        ss << "    \"unavailableFrames\": " << totalStats.unavailableFrames << "\n";
        ss << "  },\n";

        auto emitStatsJson = [&](const std::string& indent, const ProfilingStats& st) {
            ss << indent << "\"median_ms\": " << st.medianMs << ",\n";
            ss << indent << "\"p95_ms\": " << st.p95Ms << ",\n";
            ss << indent << "\"p99_ms\": " << st.p99Ms << ",\n";
            ss << indent << "\"mean_ms\": " << st.meanMs << ",\n";
            ss << indent << "\"min_ms\": " << st.minMs << ",\n";
            ss << indent << "\"max_ms\": " << st.maxMs << ",\n";
            ss << indent << "\"stddev_ms\": " << st.stddevMs << "\n";
        };

        ss << "  \"total\": {\n";
        emitStatsJson("    ", totalStats);
        ss << "  }";

        if (config_.mode == FFXM_GPU_PROFILING_MODE_PER_PASS) {
            ss << ",\n  \"passes\": {\n";
            for (uint32_t p = 0; p < FFXM_PROFILING_PASS_COUNT; ++p) {
                ss << "    \"" << kFfxmProfilingPassNames[p] << "\": {\n";
                emitStatsJson("      ", passStats[p]);
                ss << "    }" << (p + 1 < FFXM_PROFILING_PASS_COUNT ? ",\n" : "\n");
            }
            ss << "  },\n";

            ss << "  \"clears\": {\n";
            for (uint32_t c = 0; c < FFXM_PROFILING_CLEAR_JOB_COUNT; ++c) {
                ss << "    \"" << kFfxmProfilingClearJobNames[c] << "\": {\n";
                emitStatsJson("      ", clearStats[c]);
                ss << "    }" << (c + 1 < FFXM_PROFILING_CLEAR_JOB_COUNT ? ",\n" : "\n");
            }
            ss << "  }\n";
        } else {
            ss << "\n";
        }

        ss << "}\n";
        return ss.str();
    }
};

} // namespace arm
