"""Generate Speedscope timelines and hot-entry summaries from existing GPU profiles."""
import argparse
from collections import defaultdict
import csv
import json
from pathlib import Path
import re


def reports(rows, trace):
    rows = [r for r in rows if r['kernel'] != 'dropped_records' and re.fullmatch(r'evaluation-\d+', r['phase'])]
    steady = [r for r in rows if r['phase'] != 'evaluation-001' and int(r['gpu_timestamp_count']) > 0]
    if not steady:
        raise ValueError('No steady-state GPU timestamps')
    groups = defaultdict(lambda: [0, 0.0])
    for row in steady:
        group = groups[int(row['runtime_module']), row['kernel']]
        group[0] += int(row['gpu_timestamp_count'])
        group[1] += float(row['gpu_total_us'])
    total = sum(v[1] for v in groups.values())
    if total <= 0:
        raise ValueError('Steady-state timestamp total is zero')
    timestamps = sum(int(r['gpu_timestamp_count']) for r in rows)
    launches = sum(int(r['nvapi_launch_count']) for r in rows)
    events = [e for e in trace['traceEvents'] if e['ph'] == 'X'
              and re.fullmatch(r'evaluation-\d+', e.get('args', {}).get('phase', '')) and float(e['dur']) >= 0]
    if not events or len(events) != timestamps or timestamps != launches:
        raise ValueError('Trace/timestamp/NVAPI launch mismatch')
    summary = dict(schema=2, classification='D3D12 GPU timestamp profile',
                   e2eProfileFormat='Speedscope evented', excludesEvaluation1FromHotRanking=True,
                   rawArtifactsRequired=False, timestampLaunches=timestamps, nvapiLaunches=launches,
                   launchCountMatches=True, steadyStateTimestampLaunches=sum(int(r['gpu_timestamp_count']) for r in steady),
                   steadyStateGpuTotalUs=round(total, 3), runtimeModuleCount=len({k[0] for k in groups}),
                   uniqueKernelCount=len({k[1] for k in groups}),
                   interpretation='Time Order preserves launch order. Use an uninstrumented run for final frame time.',
                   hotEntries=[dict(runtimeModule=k[0], kernel=k[1], calls=v[0], totalUs=round(v[1], 3),
                                    meanUs=round(v[1] / v[0], 3), percent=round(100 * v[1] / total, 4))
                               for k, v in sorted(groups.items(), key=lambda item: -item[1][1])[:30]])
    frames = []
    indices = {}
    phases = defaultdict(list)
    for event in events:
        phases[event['args']['phase']].append(event)
    profiles = []
    for phase, phase_events in phases.items():
        phase_events.sort(key=lambda e: float(e['ts']))
        origin = float(phase_events[0]['ts'])
        duration = max(float(e['ts']) - origin + float(e['dur']) for e in phase_events)
        ordered = [('O', 0, 'DLSS-NR GPU evaluation'), ('O', 0, phase)]
        previous_end = 0
        for event in phase_events:
            start = float(event['ts']) - origin
            end = start + float(event['dur'])
            if start + 0.000001 < previous_end:
                raise ValueError(f'Overlapping launch intervals in {phase}')
            module = event.get('cat') or 'runtime-module-unknown'
            ordered.extend([('O', start, module), ('O', start, event['name']),
                            ('C', end, event['name']), ('C', end, module)])
            previous_end = end
        ordered.extend([('C', duration, phase), ('C', duration, 'DLSS-NR GPU evaluation')])
        converted = []
        for kind, at, name in ordered:
            if name not in indices:
                indices[name] = len(frames)
                frames.append({'name': name})
            converted.append(dict(type=kind, at=at, frame=indices[name]))
        profiles.append(dict(type='evented', name=phase, unit='microseconds', startValue=0,
                             endValue=duration, events=converted))
    speedscope = {'$schema': 'https://www.speedscope.app/file-format-schema.json',
                 'name': 'DLSS-NR end-to-end GPU kernel timelines', 'exporter': 'OpenNR',
                 'activeProfileIndex': next((i for i, p in enumerate(profiles) if p['name'] == 'evaluation-002'), 0),
                 'shared': {'frames': frames}, 'profiles': profiles}
    return summary, speedscope


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('output', type=Path, help='New report directory; existing directories are refused')
    args = parser.parse_args()
    with (args.capture / 'dlssnr-kernels.csv').open(encoding='utf-8-sig', newline='') as stream:
        rows = list(csv.DictReader(stream))
    summary, speedscope = reports(rows, json.loads((args.capture / 'dlssnr-trace.json').read_text(encoding='utf-8-sig')))
    summary.update(sourceCsv='dlssnr-kernels.csv', sourceTrace='dlssnr-trace.json',
                   e2eProfile='dlssnr-kernels.speedscope.json')
    args.output.mkdir(parents=True, exist_ok=False)
    for name, value in [('dlssnr-profile-summary.json', summary), ('dlssnr-kernels.speedscope.json', speedscope)]:
        (args.output / name).write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    print(args.output)