"""Explicit benchmark/analysis entry point; never runs during the default build."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
COMMANDS = {
    'compare-captures': ROOT / 'analysis/compare_capture_sequences.py',
    'compare-launches': ROOT / 'analysis/compare_launch_records.py',
    'profile-report': ROOT / 'analysis/profile_report.py',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=[*COMMANDS, 'cuda-build', 'cuda-run'])
    args, remaining = parser.parse_known_args()
    if args.command in COMMANDS:
        return subprocess.run([sys.executable, str(COMMANDS[args.command]), *remaining]).returncode
    cuda = argparse.ArgumentParser(description='Explicit isolated CUDA differential tests, not E2E performance benchmarks')
    cuda.add_argument('test', choices=['fp8-mma', 'turing-split'])
    cuda.add_argument('--sm', type=int, default=89)
    cuda.add_argument('--nvcc', default='nvcc')
    cuda.add_argument('--dry-run', action='store_true')
    options = cuda.parse_args(remaining)
    minimum = 89 if options.test == 'fp8-mma' else 80
    if options.sm < minimum:
        cuda.error(f'This differential test includes a native reference requiring SM{minimum} or newer')
    directory = ROOT.parent / 'build/benchmarks' / f'{options.test}-sm{options.sm}'
    executable = directory / f'{options.test}.exe'
    source = ROOT / 'cuda' / ('fp8_mma_split_test.cu' if options.test == 'fp8-mma' else 'turing-mma-split-test.cu')
    command = ([options.nvcc, '-std=c++17', '-O3', f'-arch=sm_{options.sm}', str(source), '-o', str(executable)]
               if args.command == 'cuda-build' else [str(executable)])
    if options.dry_run:
        print(json.dumps(command))
        return 0
    if args.command == 'cuda-build':
        directory.mkdir(parents=True, exist_ok=True)
    elif not executable.is_file():
        cuda.error('Build this test/architecture first')
    result = subprocess.run(command, cwd=directory, capture_output=True, text=True)
    (directory / f'{args.command}.log').write_text(result.stdout + result.stderr, encoding='utf-8')
    print(result.stdout + result.stderr, end='')
    return result.returncode


if __name__ == '__main__':
    sys.exit(main())