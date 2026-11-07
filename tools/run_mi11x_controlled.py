import subprocess
import time
import json
import os
import csv
import math

import sys

SERIAL = os.environ.get("ADB_SERIAL", sys.argv[1] if len(sys.argv) > 1 else "")
BASE_DIR = "artifacts/gpu-profiling/mi11x-controlled"

def adb(cmd, check=True):
    full_cmd = ["adb"]
    if SERIAL:
        full_cmd += ["-s", SERIAL]
    full_cmd += cmd
    res = subprocess.run(full_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=check)
    return res.stdout.strip()

def adb_bytes(cmd):
    full_cmd = ["adb"]
    if SERIAL:
        full_cmd += ["-s", SERIAL]
    full_cmd += cmd
    res = subprocess.run(full_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    return res.stdout

def get_thermal_telemetry():
    telemetry = {}
    # Battery temp
    try:
        bt = adb(["shell", "dumpsys battery | grep -i temperature"])
        for line in bt.splitlines():
            if "temperature:" in line.lower():
                val = line.split(":")[-1].strip()
                telemetry["batteryTempC"] = float(val) / 10.0
    except Exception as e:
        telemetry["batteryTempError"] = str(e)

    # Thermal zone 0
    try:
        tz = adb(["shell", "cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null"])
        if tz:
            telemetry["thermalZone0TempC"] = float(tz.strip()) / 1000.0
    except Exception as e:
        pass

    # Thermal status
    try:
        ts = adb(["shell", "dumpsys thermalservice | grep -i 'Thermal Status:'"])
        if ts:
            telemetry["thermalStatus"] = ts.strip()
    except Exception as e:
        pass

    # GPU cur_freq
    try:
        gf = adb(["shell", "cat /sys/class/kgsl/kgsl-3d0/devfreq/cur_freq 2>/dev/null"])
        if gf:
            telemetry["gpuFreqHz"] = int(gf.strip())
            telemetry["gpuFreqMHz"] = int(gf.strip()) / 1e6
    except Exception as e:
        pass

    return telemetry

def compute_stats(values):
    n = len(values)
    if n == 0:
        return {}
    s_vals = sorted(values)
    min_v = s_vals[0]
    max_v = s_vals[-1]
    if n % 2 == 1:
        median_v = s_vals[n // 2]
    else:
        median_v = (s_vals[(n // 2) - 1] + s_vals[n // 2]) * 0.5
    
    def p_idx(p):
        idx = math.ceil((p / 100.0) * n)
        if idx == 0: return 0
        return min(idx - 1, n - 1)
        
    p95_v = s_vals[p_idx(95.0)]
    p99_v = s_vals[p_idx(99.0)]
    mean_v = sum(values) / n
    var_v = sum((v - mean_v) ** 2 for v in values) / n
    stddev_v = math.sqrt(var_v)
    return {
        'median_ms': round(median_v, 4),
        'p95_ms': round(p95_v, 4),
        'p99_ms': round(p99_v, 4),
        'mean_ms': round(mean_v, 4),
        'min_ms': round(min_v, 4),
        'max_ms': round(max_v, 4),
        'stddev_ms': round(stddev_v, 4)
    }

def main():
    print(f"=== Starting Controlled Profiling on Mi 11X (Serial: {SERIAL}) ===")
    
    # Verify device
    model = adb(["shell", "getprop", "ro.product.model"])
    device = adb(["shell", "getprop", "ro.product.device"])
    print(f"Target device: {model} ({device})")
    assert "M2012K11AI" in model or "alioth" in device, f"Unexpected device: {model}"

    # Verify APK SHA-256 on device
    pm_path = adb(["shell", "pm", "path", "com.arm.asr.sample"])
    apk_path_on_dev = pm_path.replace("package:", "").strip()
    sha_out = adb(["shell", "sha256sum", apk_path_on_dev])
    dev_sha256 = sha_out.split()[0].strip()
    print(f"Installed APK SHA-256: {dev_sha256}")
    expected_sha = "905ce086f4115c9c3c037add49da29b1503a2d4ae6519b800aecfadba6bc5b14"
    assert dev_sha256 == expected_sha, f"SHA mismatch! Expected {expected_sha}, got {dev_sha256}"

    # Ensure display resolution
    wm_size = adb(["shell", "wm", "size"])
    print(f"Display size: {wm_size}")

    for r in [1, 2, 3]:
        run_name = f"run-0{r}"
        run_dir = os.path.join(BASE_DIR, run_name)
        os.makedirs(run_dir, exist_ok=True)
        print(f"\n--- Preparing {run_name} ---")

        # Pre-thermal check
        pre_thermal = get_thermal_telemetry()
        print(f"Pre-run thermal: {pre_thermal}")

        # Set properties & config
        dev_out_dir = f"/data/user/0/com.arm.asr.sample/files/{run_name}"
        adb(["shell", "setprop", "debug.fsr3.profiling", "diagnostic"])
        adb(["shell", "setprop", "debug.fsr3.run", str(r)])
        adb(["shell", "setprop", "debug.fsr3.warmup", "200"])
        adb(["shell", "setprop", "debug.fsr3.frames", "500"])
        adb(["shell", "setprop", "debug.fsr3.prefix", "diagnostic"])
        adb(["shell", "setprop", "debug.fsr3.dir", dev_out_dir])

        cfg_content = f"mode=diagnostic\nwarmup=200\nframes=500\nrun={r}\nprefix=diagnostic\noutput_dir={dev_out_dir}\n"
        adb(["shell", f"echo '{cfg_content}' > /data/local/tmp/fsr3_profiling.cfg && chmod 666 /data/local/tmp/fsr3_profiling.cfg"])
        adb(["shell", f"run-as com.arm.asr.sample mkdir -p files/{run_name}"])

        # Stop app and clear logcat
        adb(["shell", "am", "force-stop", "com.arm.asr.sample"])
        time.sleep(1)
        adb(["logcat", "-c"])

        # Launch app
        print(f"Launching app for {run_name}...")
        adb(["shell", "am", "start", "-n", "com.arm.asr.sample/android.app.NativeActivity"])

        # Poll for completion
        completed = False
        start_wait = time.time()
        max_wait = 45 # seconds
        while time.time() - start_wait < max_wait:
            time.sleep(3)
            logs = adb(["logcat", "-d", "-s", "ArmASR"], check=False)
            if "GPU Profiling run completed" in logs:
                print(f"{run_name} completed in ~{round(time.time() - start_wait, 1)} seconds!")
                completed = True
                break

        if not completed:
            print("WARNING: Max wait time elapsed, dumping current logs...")
            print(logs[-500:])

        # Post-thermal check
        post_thermal = get_thermal_telemetry()
        print(f"Post-run thermal: {post_thermal}")

        # Stop app
        adb(["shell", "am", "force-stop", "com.arm.asr.sample"])

        # Pull profiler log
        full_log = adb(["logcat", "-d", "-s", "ArmASR"], check=False)
        with open(os.path.join(run_dir, "profiler.log"), "w", encoding="utf-8") as f:
            f.write(full_log)

        # Pull raw diagnostic.json and diagnostic.csv via binary
        for ext in ["json", "csv"]:
            dev_file = f"files/{run_name}/diagnostic.{ext}"
            loc_file = os.path.join(run_dir, f"diagnostic.{ext}")
            content = adb_bytes(["exec-out", "run-as", "com.arm.asr.sample", "cat", dev_file])
            with open(loc_file, "wb") as f:
                f.write(content)
            print(f"Pulled {loc_file} ({len(content)} bytes)")

        # Validate CSV and JSON
        with open(os.path.join(run_dir, "diagnostic.csv"), "r", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            rows = list(reader)
        print(f"Verified {len(rows)} CSV rows in {run_name}")
        assert len(rows) == 500, f"Expected 500 rows, got {len(rows)}"
        assert all(row["valid"] == "1" for row in rows), "Found invalid row in CSV!"

        with open(os.path.join(run_dir, "diagnostic.json"), "r", encoding="utf-8") as f:
            diag_json = json.load(f)

        totals = [float(row["total_ms"]) for row in rows]
        calc_stats = compute_stats(totals)
        print(f"Total GPU time: Median={calc_stats['median_ms']} ms, Mean={calc_stats['mean_ms']} ms, Min={calc_stats['min_ms']} ms, Max={calc_stats['max_ms']} ms, StdDev={calc_stats['stddev_ms']} ms")

        # Build environment.json
        env_data = {
            "device": {
                "model": "Xiaomi Mi 11X",
                "modelNumber": "M2012K11AI",
                "codename": "aliothin",
                "productName": "aliothin",
                "brand": "Mi",
                "manufacturer": "Xiaomi",
                "soc": {
                    "model": "SM8250",
                    "platform": "kona",
                    "marketingName": "Snapdragon 870",
                    "manufacturer": "Qualcomm",
                    "abi": "arm64-v8a",
                    "cpuCores": 8,
                    "cpuTopology": "1x Cortex-A77 @ 3.2GHz + 3x Cortex-A77 @ 2.42GHz + 4x Cortex-A55 @ 1.8GHz"
                },
                "gpu": {
                    "name": "Adreno (TM) 650",
                    "vendor": "Qualcomm",
                    "architecture": "Adreno 600 series",
                    "vulkanDriverVersionRaw": diag_json["device"]["driverVersion"],
                    "vulkanDriverVersion": "0.512.530",
                    "vulkanApiVersionRaw": diag_json["device"]["apiVersion"],
                    "vulkanApiVersion": "1.1.295",
                    "vulkanInstanceVersion": "1.4.0",
                    "timestampValidBits": diag_json["device"]["timestampValidBits"],
                    "timestampPeriodNs": diag_json["device"]["timestampPeriodNs"],
                    "features": {
                        "shaderFloat16": True,
                        "storage16Bit": True,
                        "spirv14": True,
                        "storageExtendedFormats": True,
                        "storageWriteWithoutFormat": True,
                        "storageFormatsSupported": True,
                        "sampledFormatsSupported": True
                    }
                },
                "os": {
                    "androidVersion": "17",
                    "sdkLevel": 37,
                    "securityPatch": "2026-10-01",
                    "buildId": "CP2A.260605.016",
                    "buildFingerprint": "Mi/aliothin/aliothin:17/CP2A.260605.016/OS4.0.5.0.XPCCNXM:user/release-keys"
                }
            },
            "build": {
                "gitCommitSha": "13805503f74a44a137afba5b5b4908cba10c87ce",
                "gitBranch": "main",
                "buildType": "debug",
                "buildConfiguration": "-DANDROID_STL=c++_shared -DBUILD_TESTING=OFF -D_DEBUG",
                "apkPath": "samples/android/app/build/outputs/apk/debug/app-debug.apk",
                "apkSha256": dev_sha256,
                "packageName": "com.arm.asr.sample"
            },
            "benchmark": {
                "runIndex": r,
                "effect": "FSR 3.1.4",
                "mode": "per_pass_diagnostic",
                "renderResolution": "1200x540",
                "displayResolution": "2400x1080",
                "upscalingRatio": "2.00x",
                "jitterPhaseCount": 32,
                "swapchainImageCount": 5,
                "swapchainFormat": 37,
                "warmupFrames": 200,
                "measuredFrames": 500,
                "accumulatePermutation": "0x00000085",
                "accumulateUseLut": True,
                "accumulateSrvCount": 8
            },
            "telemetry": {
                "preRun": pre_thermal,
                "postRun": post_thermal
            }
        }

        with open(os.path.join(run_dir, "environment.json"), "w", encoding="utf-8") as f:
            json.dump(env_data, f, indent=2)
        print(f"Saved {os.path.join(run_dir, 'environment.json')}")

        if r < 3:
            print("Cooling down for 10 seconds before next run...")
            time.sleep(10)

    # Clean up device properties
    adb(["shell", "rm", "-f", "/data/local/tmp/fsr3_profiling.cfg"])
    adb(["shell", "setprop", "debug.fsr3.profiling", "0"])
    print("\n=== Finished Controlled Mi 11X Profiling Campaign successfully! ===")

if __name__ == "__main__":
    main()
