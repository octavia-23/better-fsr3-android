import json
import csv
import math
import os

passes = ['PrepareInputs', 'LumaPyramid', 'ShadingChangePyramid', 'ShadingChange', 'PrepareReactivity', 'LumaInstability', 'Accumulate']
clears = ['ClearReconstructedDepth', 'ClearSpdAtomicCounterInitial', 'ClearSpdMips', 'ClearSpdAtomicCounterSecondary']

def compute_stats(values):
    n = len(values)
    if n == 0:
        return {}
    s = sorted(values)
    min_v = s[0]
    max_v = s[-1]
    if n % 2 == 1:
        med_v = s[n // 2]
    else:
        med_v = (s[(n // 2) - 1] + s[n // 2]) * 0.5
    def p(pct):
        idx = math.ceil((pct / 100.0) * n) - 1
        return s[max(0, min(idx, n - 1))]
    mean_v = sum(values) / n
    var_v = sum((x - mean_v) ** 2 for x in values) / n
    std_v = math.sqrt(var_v)
    return {
        'median_ms': round(med_v, 4),
        'p95_ms': round(p(95), 4),
        'p99_ms': round(p(99), 4),
        'mean_ms': round(mean_v, 4),
        'min_ms': round(min_v, 4),
        'max_ms': round(max_v, 4),
        'stddev_ms': round(std_v, 4)
    }

def aggregate_device(runs_dir):
    all_totals = []
    all_passes = {p: [] for p in passes}
    all_clears = {c: [] for c in clears}
    run_summaries = []

    for r in [1, 2, 3]:
        csv_file = os.path.join(runs_dir, f'run-0{r}', 'diagnostic.csv')
        with open(csv_file, 'r', encoding='utf-8-sig') as f:
            rows = list(csv.DictReader(f))
        assert len(rows) == 500, f"Expected 500 rows, got {len(rows)} in {csv_file}"
        
        totals = [float(row['total_ms']) for row in rows]
        all_totals.extend(totals)
        run_p = {}
        for p in passes:
            vals = [float(row[f'{p}_ms']) for row in rows]
            all_passes[p].extend(vals)
            run_p[p] = compute_stats(vals)
        run_c = {}
        for c in clears:
            vals = [float(row[f'{c}_ms']) for row in rows]
            all_clears[c].extend(vals)
            run_c[c] = compute_stats(vals)
            
        run_summaries.append({
            'runIndex': r,
            'total': compute_stats(totals),
            'passes': run_p,
            'clears': run_c
        })

    return {
        'total': compute_stats(all_totals),
        'passes': {p: compute_stats(all_passes[p]) for p in passes},
        'clears': {c: compute_stats(all_clears[c]) for c in clears},
        'runs': run_summaries
    }

def main():
    f6_data = aggregate_device('artifacts/gpu-profiling/f6')
    mi11x_data = aggregate_device('artifacts/gpu-profiling/mi11x-controlled')
    
    # Historical Mi 11X
    with open('artifacts/gpu-profiling/mi11x/fsr3_mi11x_diagnostic.json', 'r', encoding='utf-8-sig') as f:
        hist_mi11x = json.load(f)

    # Comparison Table
    rows = []
    # Total
    f6_tot = f6_data['total']['median_ms']
    mi_tot = mi11x_data['total']['median_ms']
    speedup_pct = round(((mi_tot - f6_tot) / mi_tot) * 100.0, 2)
    speedup_ratio = round(mi_tot / f6_tot, 2)
    rows.append({
        'Stage': 'TOTAL FSR 3.1.4 GPU TIME',
        'Type': 'Total',
        'Mi11X_Adreno650_Median_ms': mi_tot,
        'PocoF6_Adreno735_Median_ms': f6_tot,
        'Speedup_Percent': speedup_pct,
        'Speedup_Ratio': f"{speedup_ratio}x",
        'Mi11X_Pct_Total': 100.0,
        'PocoF6_Pct_Total': 100.0
    })

    # Passes
    for p in passes:
        f6_p = f6_data['passes'][p]['median_ms']
        mi_p = mi11x_data['passes'][p]['median_ms']
        pct = round(((mi_p - f6_p) / mi_p) * 100.0, 2)
        ratio = round(mi_p / f6_p, 2)
        rows.append({
            'Stage': p,
            'Type': 'Compute Pass',
            'Mi11X_Adreno650_Median_ms': mi_p,
            'PocoF6_Adreno735_Median_ms': f6_p,
            'Speedup_Percent': pct,
            'Speedup_Ratio': f"{ratio}x",
            'Mi11X_Pct_Total': round((mi_p / mi_tot) * 100.0, 2),
            'PocoF6_Pct_Total': round((f6_p / f6_tot) * 100.0, 2)
        })

    # Clears
    for c in clears:
        f6_c = f6_data['clears'][c]['median_ms']
        mi_c = mi11x_data['clears'][c]['median_ms']
        pct = round(((mi_c - f6_c) / mi_c) * 100.0, 2)
        ratio = round(mi_c / f6_c, 2) if f6_c > 0 else 0
        rows.append({
            'Stage': c,
            'Type': 'Clear Job',
            'Mi11X_Adreno650_Median_ms': mi_c,
            'PocoF6_Adreno735_Median_ms': f6_c,
            'Speedup_Percent': pct,
            'Speedup_Ratio': f"{ratio}x",
            'Mi11X_Pct_Total': round((mi_c / mi_tot) * 100.0, 2),
            'PocoF6_Pct_Total': round((f6_c / f6_tot) * 100.0, 2)
        })

    os.makedirs('artifacts/gpu-profiling/controlled-comparison', exist_ok=True)
    
    # Write CSV
    with open('artifacts/gpu-profiling/controlled-comparison/comparison.csv', 'w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print("Saved comparison.csv")

    # Write JSON
    comparison_json = {
        'metadata': {
            'benchmark': 'Controlled FSR 3.1.4 Vulkan Per-Pass GPU Profiling Benchmark',
            'renderResolution': '1200x540',
            'displayResolution': '2400x1080',
            'upscalingRatio': '2.00x',
            'accumulatePermutation': '0x00000085',
            'accumulateUseLut': True,
            'accumulateSrvCount': 8,
            'measuredFramesPerDevice': 1500,
            'warmupFramesPerRun': 200,
            'runsPerDevice': 3
        },
        'devices': {
            'poco_f6': {
                'model': 'POCO F6 (24069PC21I)',
                'codename': 'peridot',
                'soc': 'Snapdragon 8s Gen 3 (SM8635)',
                'gpu': 'Adreno (TM) 735',
                'vulkanApi': '1.3.128',
                'driverVersion': '0.762.1',
                'timestampValidBits': 48,
                'timestampPeriodNs': 52.0833
            },
            'mi_11x': {
                'model': 'Xiaomi Mi 11X (M2012K11AI)',
                'codename': 'aliothin',
                'soc': 'Snapdragon 870 (SM8250)',
                'gpu': 'Adreno (TM) 650',
                'vulkanApi': '1.1.295',
                'driverVersion': '0.512.530',
                'timestampValidBits': 48,
                'timestampPeriodNs': 52.0833
            }
        },
        'aggregateTelemetry': {
            'poco_f6_adreno_735': f6_data,
            'mi_11x_controlled_adreno_650': mi11x_data,
            'mi_11x_historical_reference': hist_mi11x
        },
        'comparisonTable': rows
    }

    with open('artifacts/gpu-profiling/controlled-comparison/comparison.json', 'w', encoding='utf-8') as f:
        json.dump(comparison_json, f, indent=2)
    print("Saved comparison.json")

    # Print summary
    print("\n================ CONTROLLED BENCHMARK COMPARISON SUMMARY ================")
    print(f"{'Stage':<32} | {'Mi 11X (650)':<14} | {'Poco F6 (735)':<14} | {'Speedup':<10} | {'Ratio':<6}")
    print("-" * 85)
    for r in rows:
        print(f"{r['Stage']:<32} | {r['Mi11X_Adreno650_Median_ms']:>10.4f} ms | {r['PocoF6_Adreno735_Median_ms']:>10.4f} ms | {r['Speedup_Percent']:>8.2f} % | {r['Speedup_Ratio']:>6}")
    print("=" * 85)

if __name__ == '__main__':
    main()
