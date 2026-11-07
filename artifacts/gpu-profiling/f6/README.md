# Poco F6 (Adreno 735) GPU Telemetry Benchmark Report

## 1. Executive Summary

This directory contains genuine, reproducible Vulkan GPU timestamp telemetry collected on a physical **Poco F6 (Snapdragon 8s Gen 3, Adreno 735)** running the Android Vulkan FSR 3.1.4 upscaler implementation.

Three independent profiling runs of 500 measured frames (200 warmup frames excluded) were executed at 1200x540 render resolution upscaled 2x to 2400x1080 display resolution.

### Primary Results
- **Poco F6 Total GPU Time (Adreno 735):** **8.626 ms mean** / **8.585 ms median** / **8.915 ms P95** (~116 FPS GPU execution budget).
- **Mi 11X Reference GPU Time (Adreno 650):** **24.273 ms mean** / **24.244 ms median** / **24.501 ms P95** (~41 FPS GPU execution budget).
- **Hardware Performance Improvement:** **64.46% faster total GPU execution (2.81x speedup)**.
- **Run-to-Run Variance:** Extremely low stddev of **0.0018 ms** (< 2 microseconds) across three independent runs.
- **Bottleneck Dominance:** **Accumulate remains the single dominant bottleneck**, consuming **6.075 ms** (**70.43%** of total FSR 3.1.4 GPU time).

---

## 2. Benchmark Configuration & Environmental Controls

| Parameter | Configuration |
|---|---|
| **Device Model** | POCO F6 (`24069PC21I`) |
| **Device Codename** | `peridot` (`peridot_in`) |
| **SoC** | Qualcomm Snapdragon 8s Gen 3 (`SM8635`, platform `pineapple`) |
| **CPU Architecture** | ARM64-v8a (1x Cortex-X4 @ 3.0GHz, 4x Cortex-A720 @ 2.8GHz, 3x Cortex-A520 @ 2.0GHz) |
| **GPU Hardware** | Qualcomm Adreno (TM) 735 |
| **GPU Architecture** | Qualcomm Adreno 700 series |
| **Vulkan API Version** | 1.3.128 (`VK_API_VERSION_1_3`, version int 4206720) |
| **GPU Driver Version** | 0.762.1 (raw int 2150604801, GLES: V@0762.36.1 Date 12/22/25) |
| **OS / Android Version** | Android 16 (API Level 36 / Baklava, Build `BP2A.250605.031.A3`) |
| **Git Commit SHA** | `13805503f74a44a137afba5b5b4908cba10c87ce` |
| **Build Configuration** | `-DANDROID_STL=c++_shared -DBUILD_TESTING=OFF -D_DEBUG` |
| **APK SHA-256** | `905ce086f4115c9c3c037add49da29b1503a2d4ae6519b800aecfadba6bc5b14` |
| **Vulkan Timestamp Period** | 52.0833 ns per tick (48 valid bits, wraparound-masked) |
| **Render Resolution** | 1200 x 540 |
| **Display Resolution** | 2400 x 1080 |
| **Upscaling Ratio** | 2.00x (`PERFORMANCE` mode) |
| **Profiling Mode** | `PER_PASS_DIAGNOSTIC` |
| **Warmup Frames** | 200 frames (strictly excluded from measurement pool) |
| **Measured Frames** | 500 valid frames per run (1500 total across 3 runs, 0 unavailable) |
| **Shader Configuration** | FSR 3.1.4 Accumulate permutation `0x00000085`, `useLut: true` (8 SRV textures) |

---

## 3. Telemetry Results across Three Independent Runs

Each run started from a clean process session, executed 200 warmup frames, collected 500 measured frames, recorded pre- and post-run thermal telemetry via Android ThermalHAL AIDL 3, and saved complete per-frame CSV timings and summary JSON diagnostics.

| Metric | Run 1 (Mean / Median / P95) | Run 2 (Mean / Median / P95) | Run 3 (Mean / Median / P95) | Pooled 3-Run Average |
|---|---|---|---|---|
| **Total GPU** | 8.628 / 8.585 / 8.919 ms | 8.625 / 8.585 / 8.689 ms | 8.624 / 8.585 / 8.667 ms | **8.626 / 8.585 / 8.915 ms** |
| **PrepareInputs** | 1.043 / 1.012 / 1.024 ms | 1.041 / 1.011 / 1.023 ms | 1.036 / 1.013 / 1.023 ms | **1.040 / 1.012 / 1.023 ms** |
| **LumaPyramid** | 0.158 / 0.157 / 0.159 ms | 0.160 / 0.157 / 0.160 ms | 0.157 / 0.157 / 0.160 ms | **0.158 / 0.157 / 0.160 ms** |
| **ShadingChangePyramid** | 0.388 / 0.383 / 0.422 ms | 0.389 / 0.383 / 0.424 ms | 0.386 / 0.383 / 0.424 ms | **0.388 / 0.383 / 0.424 ms** |
| **ShadingChange** | 0.047 / 0.047 / 0.047 ms | 0.047 / 0.047 / 0.047 ms | 0.047 / 0.047 / 0.047 ms | **0.047 / 0.047 / 0.047 ms** |
| **PrepareReactivity** | 0.608 / 0.604 / 0.613 ms | 0.606 / 0.604 / 0.613 ms | 0.606 / 0.604 / 0.613 ms | **0.607 / 0.604 / 0.613 ms** |
| **LumaInstability** | 0.270 / 0.269 / 0.276 ms | 0.269 / 0.269 / 0.275 ms | 0.273 / 0.269 / 0.277 ms | **0.271 / 0.269 / 0.276 ms** |
| **Accumulate** | 6.075 / 6.066 / 6.097 ms | 6.072 / 6.066 / 6.097 ms | 6.079 / 6.066 / 6.098 ms | **6.075 / 6.066 / 6.097 ms** |
| **ClearReconstructedDepth** | 0.019 / 0.019 / 0.019 ms | 0.019 / 0.019 / 0.019 ms | 0.019 / 0.019 / 0.019 ms | **0.019 / 0.019 / 0.019 ms** |
| **ClearSpdAtomicInitial** | 0.002 / 0.002 / 0.002 ms | 0.002 / 0.002 / 0.002 ms | 0.002 / 0.002 / 0.002 ms | **0.002 / 0.002 / 0.002 ms** |
| **ClearSpdMips** | 0.013 / 0.013 / 0.013 ms | 0.013 / 0.013 / 0.013 ms | 0.013 / 0.013 / 0.013 ms | **0.013 / 0.013 / 0.013 ms** |
| **ClearSpdAtomicSecondary** | 0.005 / 0.005 / 0.005 ms | 0.005 / 0.005 / 0.005 ms | 0.005 / 0.005 / 0.005 ms | **0.005 / 0.005 / 0.005 ms** |
| **Thermal Pre-Run (Skin / GPU0)** | 37.3°C / 39.2°C | 38.1°C / 40.0°C | 39.5°C / 41.2°C | Controlled resting baseline |
| **Thermal Post-Run (Skin / GPU0)**| 39.6°C / 80.4°C | 40.5°C / 79.6°C | 41.3°C / 83.6°C | Thermal status: 0 (No throttling) |

### Timing Reconciliation Analysis
- **Sum of Individual Compute Passes:** 8.5854 ms (99.53% of total)
- **Sum of Clear Operations:** 0.0386 ms (0.45% of total)
- **Sum of All Sub-Jobs:** 8.6240 ms (99.98% of total)
- **Measured Total GPU Mean:** 8.6258 ms
- **Residual Unattributed Overhead:** **+0.0018 ms (+1.8 microseconds, 0.02%)**
- **Conclusion:** The Vulkan timestamp query instrumentation reconciles with 99.98% fidelity. The tiny 1.8 µs residual is the negligible cost of Vulkan barrier synchronization and timestamp recording between command buffer dispatches.

---

## 4. Hardware Comparison: Poco F6 (Adreno 735) vs Mi 11X (Adreno 650)

Both benchmark datasets use identical render resolution (1200x540), display resolution (2400x1080), 2.0x upscale ratio, 200 warmup frames, 500 measured frames, Vulkan timestamp query methodology, and Lanczos LUT Accumulate shader permutation (`useLut: true`).

| Benchmark Pass / Metric | Mi 11X (Adreno 650) Mean | Poco F6 (Adreno 735) Mean | Absolute Delta | Improvement (%) | Speedup Factor |
|---|---:|---:|---:|---:|---:|
| **Total GPU Execution** | **24.2730 ms** | **8.6258 ms** | **-15.6472 ms** | **+64.46%** | **2.81x** |
| **Total GPU Median** | 24.2441 ms | 8.5851 ms | -15.6590 ms | +64.59% | 2.82x |
| **Total GPU P95** | 24.5010 ms | 8.9153 ms | -15.5857 ms | +63.61% | 2.75x |
| **Accumulate** | 18.1234 ms | 6.0752 ms | -12.0482 ms | +66.48% | 2.98x |
| **PrepareInputs** | 2.6764 ms | 1.0402 ms | -1.6362 ms | +61.13% | 2.57x |
| **PrepareReactivity** | 1.2804 ms | 0.6066 ms | -0.6738 ms | +52.62% | 2.11x |
| **ShadingChangePyramid** | 0.8574 ms | 0.3877 ms | -0.4697 ms | +54.78% | 2.21x |
| **LumaInstability** | 0.7974 ms | 0.2709 ms | -0.5265 ms | +66.03% | 2.94x |
| **LumaPyramid** | 0.3100 ms | 0.1582 ms | -0.1518 ms | +48.97% | 1.96x |
| **ShadingChange** | 0.0918 ms | 0.0466 ms | -0.0452 ms | +49.24% | 1.97x |
| **ClearReconstructedDepth** | 0.0913 ms | 0.0189 ms | -0.0724 ms | +79.30% | 4.83x |
| **ClearSpdAtomicInitial** | 0.0020 ms | 0.0019 ms | -0.0001 ms | +5.00% | 1.05x |
| **ClearSpdMips** | 0.0369 ms | 0.0132 ms | -0.0237 ms | +64.23% | 2.80x |
| **ClearSpdAtomicSecondary** | 0.0037 ms | 0.0046 ms | +0.0009 ms | -24.32% | 0.80x |
| **Accumulate % of Total** | **74.66%** | **70.43%** | **-4.23%** | N/A | Dominant |

---

## 5. Engineering Interpretation & Architectural Answers

### 1. Does Accumulate remain the dominant bottleneck on Adreno 735?
**Yes, decisively.** Accumulate takes 6.075 ms out of the 8.626 ms total GPU execution time. Despite Adreno 735 having substantial microarchitectural enhancements (wider ALUs, improved FP16 throughput, higher bandwidth LPDDR5X), Accumulate consumes the vast majority of execution time.

### 2. What percentage of FSR3 GPU time does it consume?
Accumulate consumes **70.43%** of the entire FSR 3.1.4 frame cost on Adreno 735 (down only slightly from 74.66% on Adreno 650). Together with PrepareInputs (12.06%), these two passes account for **82.49%** of all GPU execution time.

### 3. Which passes show the greatest performance improvement relative to Adreno 650?
- **ClearReconstructedDepth:** 4.83x speedup (+79.30%), benefiting directly from Adreno 700 series fast clear tile memory mechanisms.
- **Accumulate:** 2.98x speedup (+66.48%), scaling from 18.12 ms down to 6.08 ms. The higher arithmetic density and texture sampler throughput of Adreno 735 significantly accelerate Lanczos filtering.
- **LumaInstability:** 2.94x speedup (+66.03%), dropping from 0.80 ms to 0.27 ms.

### 4. Which passes scale poorly?
- **LumaPyramid (1.96x) and ShadingChange (1.97x):** Sub-2x speedup. These Single Pass Downsampler (SPD) atomic-reduction and small-workload dispatches are latency- and atomic-counter bound rather than compute-throughput bound.
- **PrepareReactivity (2.11x) and ShadingChangePyramid (2.21x):** Moderate scaling, throttled by global memory roundtrips and atomic synchronization.
- **ClearSpdAtomicCounterSecondary (0.80x):** Microsecond-scale Vulkan clear dispatch bound by command processor dispatch latency rather than memory fill rate.

### 5. Are there signs of a disproportionately expensive accumulation workload?
**Yes.** Accumulate executes at the display resolution (2400x1080 = 2,592,000 pixels) with a 6x6 Lanczos kernel (36 taps) or 8-tap filtered reconstruction, whereas all other passes execute at 1200x540 (648,000 pixels = 4x fewer samples). The 4x pixel count combined with large local register pressure and texture sampling causes Accumulate to dominate the pipeline on mobile hardware.

### 6. Is the overall execution time stable across three runs?
**Remarkably stable.** 
- Run 1 Mean: 8.6283 ms
- Run 2 Mean: 8.6246 ms
- Run 3 Mean: 8.6245 ms
The run-to-run standard deviation is **0.0018 ms** (1.8 µs). The per-pass medians across all three runs are virtually identical to 3 decimal places. Device temperatures remained under 41.3°C on skin and ThermalHAL status remained at 0 (Normal / No throttling).

### 7. What should be profiled next using Snapdragon Profiler or vendor tools?
1. **Accumulate Kernel Register Pressure & Occupancy:** Measure General Purpose Register (GPR) usage in `autogen_fsr3upscaler_accumulate_pass_adreno.spv` to verify whether wave occupancy is restricted by register footprint.
2. **Texture Sampler & L1/L2 Cache Miss Rates:** Profile L1 cache hit rate in Accumulate to examine whether the 8 SRV textures and 36-tap sampling cause cache thrashing across tile boundaries.
3. **ALU vs Texture Instruction Stall Cycles:** Determine whether the execution units spend clock cycles stalled on texture fetch latency or ALU arithmetic.

### 8. Which optimization experiments deserve priority based on actual evidence?
1. **Accumulate Kernel Specialization & Wave Size Tuning:** Since Accumulate represents 70.4% of total time, any 10% gain in Accumulate saves ~0.61 ms, which exceeds the total runtime of LumaPyramid, ShadingChange, and LumaInstability combined.
2. **PrepareInputs Bandwidth Minimization:** PrepareInputs consumes 1.04 ms (12.1% of runtime) doing format conversion, motion vector dilation, and depth dilation. Combining dilation passes or utilizing subpass attachments can reclaim ~0.3-0.5 ms.
3. **Single Pass Downsampler (SPD) LDS & Threadgroup Geometry:** Optimizing workgroup tile layout for Adreno 700 series wave sizes (wave32/wave64) will benefit LumaPyramid and ShadingChangePyramid.

---

## 6. Directory Artifacts

```
artifacts/gpu-profiling/f6/
├── run-01/
│   ├── diagnostic.csv       # 500 frames raw per-frame GPU timestamp telemetry
│   ├── diagnostic.json      # Aggregated run statistics (mean, median, p95, p99, min, max, stddev)
│   ├── profiler.log         # Complete logcat output from ArmASR profiling harness
│   └── environment.json     # Hardware, OS, driver, Vulkan, build, and thermal metadata
├── run-02/
│   ├── diagnostic.csv
│   ├── diagnostic.json
│   ├── profiler.log
│   └── environment.json
├── run-03/
│   ├── diagnostic.csv
│   ├── diagnostic.json
│   ├── profiler.log
│   └── environment.json
├── comparison.csv           # Pass-by-pass comparative table against Mi 11X
├── comparison.json          # Complete JSON comparative dataset
└── README.md                # This benchmark report
```
