#!/usr/bin/env python3
"""Alternate independent StoneKV and LevelDB workload runs; never reuse output."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--stonekv', type=Path, required=True)
parser.add_argument('--leveldb', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--count', type=int, default=200)
parser.add_argument('--rounds', type=int, default=3)
parser.add_argument('--threads', type=int, default=4)
parser.add_argument('--seed', type=int, default=42)
args = parser.parse_args()
if not 1 <= args.rounds <= 20:
    parser.error('rounds must be in 1..20')
root = args.output.absolute()
root.mkdir(parents=False, exist_ok=False)
runner = Path(__file__).resolve().with_name('run_benchmark_workloads.py')
binaries = {'stonekv': args.stonekv.resolve(strict=True), 'leveldb': args.leveldb.resolve(strict=True)}
for round_no in range(1, args.rounds+1):
    order = ['stonekv', 'leveldb'] if round_no % 2 else ['leveldb', 'stonekv']
    for engine in order:
        tag=f'{engine}_round{round_no}'
        command=[sys.executable, str(runner), '--binary', str(binaries[engine]), '--output', str(root/tag),
                 '--count', str(args.count), '--rounds', '1', '--threads', str(args.threads), '--seed', str(args.seed)]
        result=subprocess.run(command, capture_output=True, text=True, check=False)
        (root/(tag+'.json')).write_text(json.dumps({'command':command, 'exit':result.returncode,
                                                    'stdout':result.stdout, 'stderr':result.stderr}, indent=2)+'\n')
        print(f'{tag} exit={result.returncode}',flush=True)
        if result.returncode:
            sys.exit(result.returncode)
print(f'PASS: {root}')
