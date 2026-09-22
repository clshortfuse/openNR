"""Stage a native harness in a new directory; execute only with --execute."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess


def stage(api, candidate, core, output, cupti=None, evaluations=8, profile=False):
    root = Path(__file__).resolve().parents[2]
    executable_name = 'dlssnr-repro.exe' if api == 'd3d12' else 'dlssnr-d3d11-repro.exe'
    executable_candidates = [
        root / 'build/benchmarks/native/bin' / executable_name,
        root / 'build/tools/bin/Release' / executable_name,
    ]
    executable = next((path for path in executable_candidates if path.is_file()),
                      executable_candidates[0])
    files = [executable, candidate, core]
    if api == 'd3d12':
        if cupti is None:
            raise ValueError('Specify the installed CUPTI runtime DLL with --cupti')
        files.append(cupti)
    if not all(p.is_file() for p in files):
        raise ValueError('Missing executable or runtime input')
    if len({p.name.lower() for p in files}) != len(files):
        raise ValueError('Runtime input filenames collide')
    if evaluations < 1 or (profile and api != 'd3d12'):
        raise ValueError('Invalid evaluation count or unsupported profiling API')
    output.mkdir(parents=True, exist_ok=False)
    records = []
    for source in files:
        target = output / source.name
        shutil.copy2(source, target)
        digest = hashlib.sha256(source.read_bytes()).hexdigest().upper()
        if hashlib.sha256(target.read_bytes()).hexdigest().upper() != digest:
            raise ValueError(f'Copy mismatch: {source}')
        records.append(dict(source=str(source.resolve()), file=source.name, sha256=digest))
    command = [str((output / executable.name).resolve()), str((output / candidate.name).resolve()),
               str((output / core.name).resolve())]
    if api == 'd3d12':
        command.append(str(evaluations))
    record = dict(api=api, profile=profile, evaluations=evaluations, files=records,
                  command=command, system=platform.platform(), executed=False)
    (output / 'run-manifest.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('api', choices=['d3d12', 'd3d11'])
    parser.add_argument('--candidate', required=True, type=Path)
    parser.add_argument('--core', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--cupti', type=Path)
    parser.add_argument('--evaluations', type=int, default=8)
    parser.add_argument('--profile', action='store_true')
    parser.add_argument('--execute', action='store_true')
    args = parser.parse_args()
    record = stage(args.api, args.candidate, args.core, args.output, args.cupti, args.evaluations, args.profile)
    if args.execute:
        environment = {k: v for k, v in os.environ.items() if not k.startswith('RENODX_DLSSNR_REPRO_')}
        environment['RENODX_DLSSNR_REPRO_NO_MESSAGE_BOX'] = '1'
        if args.profile:
            environment['RENODX_DLSSNR_REPRO_PROFILE'] = '1'
        with (args.output / 'console.log').open('w', encoding='utf-8') as stream:
            result = subprocess.run(record['command'], cwd=args.output, env=environment,
                                    stdout=stream, stderr=subprocess.STDOUT)
        record.update(executed=True, exit_code=result.returncode)
        (args.output / 'run-manifest.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
        raise SystemExit(result.returncode)
    print(f'Staged only (no GPU execution): {args.output}')