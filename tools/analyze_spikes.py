import csv
import json
import os
import math

def load_csv(path):
    with open(path, 'r', encoding='utf-8-sig') as f:
        reader = csv.DictReader(f)
        rows = []
        for r in reader:
            row = {
                'frame': int(r['frame']),
                'valid': int(r['valid']),
                'total_ms': float(r['total_ms']),
                'PrepareInputs': float(r['PrepareInputs_ms']),
                'LumaPyramid': float(r['LumaPyramid_ms']),
                'ShadingChangePyramid': float(r['ShadingChangePyramid_ms']),
                'ShadingChange': float(r['ShadingChange_ms']),
                'PrepareReactivity': float(r['PrepareReactivity_ms']),
                'LumaInstability': float(r['LumaInstability_ms']),
                'Accumulate': float(r['Accumulate_ms']),
                'ClearReconstructedDepth': float(r['ClearReconstructedDepth_ms']),
                'ClearSpdAtomicCounterInitial': float(r['ClearSpdAtomicCounterInitial_ms']),
                'ClearSpdMips': float(r['ClearSpdMips_ms']),
                'ClearSpdAtomicCounterSecondary': float(r['ClearSpdAtomicCounterSecondary_ms'])
            }
            rows.append(row)
        return rows

def analyze_f6():
    runs = {}
    for r in [1, 2, 3]:
        path = f'artifacts/gpu-profiling/f6/run-0{r}/diagnostic.csv'
        rows = load_csv(path)
        
        totals = [row['total_ms'] for row in rows]
        sorted_totals = sorted(totals)
        median_total = sorted_totals[len(sorted_totals) // 2]
        
        spikes = []
        prev_spike_frame = None
        intervals = []
        
        for idx, row in enumerate(rows):
            is_spike = False
            spike_reason = []
            
            # Check individual passes
            if row['PrepareInputs'] > 1.3: # normal ~1.01
                is_spike = True
                spike_reason.append(f"PrepareInputs={row['PrepareInputs']:.3f}")
            if row['ShadingChangePyramid'] > 0.6: # normal ~0.38
                is_spike = True
                spike_reason.append(f"ShadingChangePyramid={row['ShadingChangePyramid']:.3f}")
            if row['Accumulate'] > 6.5: # normal ~6.08
                is_spike = True
                spike_reason.append(f"Accumulate={row['Accumulate']:.3f}")
            if row['LumaPyramid'] > 0.25: # normal ~0.15
                is_spike = True
                spike_reason.append(f"LumaPyramid={row['LumaPyramid']:.3f}")
            if row['total_ms'] > median_total + 0.35:
                is_spike = True
                
            if is_spike:
                interval = (row['frame'] - prev_spike_frame) if prev_spike_frame is not None else None
                if interval is not None:
                    intervals.append(interval)
                spikes.append({
                    'row_idx': idx,
                    'frame': row['frame'],
                    'total_ms': row['total_ms'],
                    'delta_over_median_ms': round(row['total_ms'] - median_total, 4),
                    'interval_from_prev': interval,
                    'PrepareInputs': row['PrepareInputs'],
                    'ShadingChangePyramid': row['ShadingChangePyramid'],
                    'Accumulate': row['Accumulate'],
                    'LumaPyramid': row['LumaPyramid'],
                    'reasons': spike_reason
                })
                prev_spike_frame = row['frame']
                
        runs[f"run-0{r}"] = {
            'median_total_ms': median_total,
            'spike_count': len(spikes),
            'intervals': intervals,
            'mean_interval': round(sum(intervals) / len(intervals), 2) if intervals else 0,
            'spikes': spikes
        }
        
    return runs

def analyze_mi11x():
    path = 'artifacts/gpu-profiling/mi11x/fsr3_mi11x_diagnostic.csv'
    rows = load_csv(path)
    totals = [row['total_ms'] for row in rows]
    sorted_totals = sorted(totals)
    median_total = sorted_totals[len(sorted_totals) // 2]
    
    spikes = []
    prev_spike_frame = None
    intervals = []
    
    for idx, row in enumerate(rows):
        # On Mi 11X, median is 24.24 ms. Check if any frame > 24.6 ms
        if row['total_ms'] > median_total + 0.35:
            interval = (row['frame'] - prev_spike_frame) if prev_spike_frame is not None else None
            if interval is not None:
                intervals.append(interval)
            spikes.append({
                'row_idx': idx,
                'frame': row['frame'],
                'total_ms': row['total_ms'],
                'delta_ms': round(row['total_ms'] - median_total, 4),
                'interval': interval,
                'PrepareInputs': row['PrepareInputs'],
                'ShadingChangePyramid': row['ShadingChangePyramid'],
                'Accumulate': row['Accumulate']
            })
            prev_spike_frame = row['frame']
            
    return {
        'median_total_ms': median_total,
        'spike_count': len(spikes),
        'intervals': intervals,
        'spikes': spikes
    }

if __name__ == '__main__':
    f6_analysis = analyze_f6()
    mi11x_analysis = analyze_mi11x()
    output = {
        'f6': f6_analysis,
        'mi11x_historical': mi11x_analysis
    }
    with open('artifacts/gpu-profiling/f6_spike_analysis.json', 'w') as f:
        json.dump(output, f, indent=2)
    print("Spike analysis completed successfully!")
    for r in [1, 2, 3]:
        run_key = f"run-0{r}"
        data = f6_analysis[run_key]
        print(f"F6 {run_key}: {data['spike_count']} spikes, mean interval: {data['mean_interval']} frames. Intervals: {data['intervals']}")
    print(f"Mi 11X Historical: {mi11x_analysis['spike_count']} spikes, intervals: {mi11x_analysis['intervals']}")
