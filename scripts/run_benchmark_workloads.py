#!/usr/bin/env python3
"""Run independent StoneKV workloads. Never deletes or reuses output directories."""
import argparse
import csv
import datetime
import json
import math
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


def percentile(values, p):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(p * len(ordered)) - 1)]


def disk_bytes(root):
    return sum(p.stat().st_size for p in root.rglob('*') if p.is_file())


def run():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='new directory; existing paths rejected')
    parser.add_argument('--count', type=int, default=1000)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--seed', type=int, default=42)
    parser.add_argument('--threads', type=int, default=4)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error('binary must be an executable file')
    if not 4 <= args.count <= 1000000 or not 1 <= args.rounds <= 20 or not 1 <= args.threads <= 64 or args.seed < 0:
        parser.error('invalid count, rounds, threads, or seed')
    timer = shutil.which('/usr/bin/time')
    if not timer:
        parser.error('/usr/bin/time is required for process CPU and maximum RSS')
    root = args.output.absolute()
    root.mkdir(parents=False, exist_ok=False)
    def git(*cmd):
        p = subprocess.run(['git', *cmd], cwd=Path(__file__).resolve().parents[1], text=True, capture_output=True)
        return {'exit_code': p.returncode, 'stdout': p.stdout, 'stderr': p.stderr}
    env = {'timestamp': datetime.datetime.now().astimezone().isoformat(), 'platform': platform.platform(),
           'machine': platform.machine(), 'python': sys.version, 'cpu_count': os.cpu_count(),
           'disk_free_bytes_at_start': shutil.disk_usage(root).free,
           'git_head': git('rev-parse', 'HEAD'), 'git_status': git('status', '--short', '--branch'),
           'binary': str(binary), 'count': args.count, 'seed': args.seed, 'rounds': args.rounds, 'threads': args.threads,
           'notes': ['Each mode/round gets a new DB; input seed and parameters fixed.',
                     'sync=0 has no durable acknowledgement before Flush; never combine with sync=1.',
                     'Latency is API wall time; concurrent samples include lock contention and scheduling.',
                     'P50/P95/P99 use nearest-rank on per-operation samples, not averaged round percentiles.',
                     'Read amplification, write amplification, background queue depth and compaction bytes: NOT RUN (not exposed by API).',
                     'CPU percent is whole-process user+system divided by wall time; 100% means one core. It is repeated on phase rows, not measured per phase. A 0.00s CPU or wall measurement is reported as unavailable due to timer precision.',
                     'Whole-process max RSS and final DB file bytes are not index-only memory or peak disk usage.']}
    (root/'environment.json').write_text(json.dumps(env, indent=2)+'\n', encoding='utf8')
    summary_rows=[]
    for round_no in range(args.rounds):
        # Alternate sync mode order between rounds to reduce time-order bias.
        for sync in ([0,1] if round_no % 2 == 0 else [1,0]):
            tag=f'sync{sync}_round{round_no+1}'
            sample_dir=root/tag
            timer_file=root/(tag+'.time')
            cmd=[str(binary),str(sample_dir),str(args.count),str(sync),str(args.seed),str(args.threads)]
            timed=[timer,'-o',str(timer_file),'-f','wall_seconds=%e\nuser_seconds=%U\nsystem_seconds=%S\nmax_rss_kib=%M\nexit=%x',*cmd]
            (root/(tag+'.command.json')).write_text(json.dumps(timed,indent=2)+'\n',encoding='utf8')
            with (root/(tag+'.stdout')).open('w') as stdout,(root/(tag+'.stderr')).open('w') as stderr:
                result=subprocess.run(timed,stdout=stdout,stderr=stderr,check=False)
            (root/(tag+'.exit')).write_text(str(result.returncode)+'\n',encoding='ascii')
            print(f'{tag} exit={result.returncode}',flush=True)
            if result.returncode != 0:
                raise RuntimeError(f'{tag} failed; inspect logs, no further runs')
            timings={}
            for line in timer_file.read_text().splitlines():
                if '=' in line:
                    k,v=line.split('=',1);timings[k]=v
            wall=float(timings['wall_seconds']);user=float(timings['user_seconds']);system=float(timings['system_seconds'])
            cpu=100*(user+system)/wall if wall>0 and user+system>0 else None
            with (sample_dir/'latencies.csv').open(newline='') as f:
                latency={}
                for row in csv.DictReader(f): latency.setdefault(row['phase'],[]).append(int(row['latency_ns']))
            with (sample_dir/'phases.csv').open(newline='') as f: phases=list(csv.DictReader(f))
            for phase in phases:
                name=phase['phase']; values=latency[name]; count=int(phase['operations']); wall_ns=int(phase['wall_ns'])
                if len(values)!=count or wall_ns <= 0 or any(v < 0 for v in values):raise RuntimeError(f'invalid samples: {tag} {name}')
                summary_rows.append({'sync':sync,'round':round_no+1,'phase':name,'operations':count,'wall_ms':wall_ns/1e6,
                    'ops_per_second':count*1e9/wall_ns,'p50_us':percentile(values,.50)/1000,
                    'p95_us':percentile(values,.95)/1000,'p99_us':percentile(values,.99)/1000,
                    'process_cpu_single_core_percent':cpu,'process_max_rss_kib':int(timings['max_rss_kib']),
                    'final_db_file_bytes':disk_bytes(sample_dir/'db')})
            with (root/'summary.csv').open('w',newline='') as f:
                writer=csv.DictWriter(f,fieldnames=list(summary_rows[0]));writer.writeheader();writer.writerows(summary_rows)
    print(f'PASS: {root}',flush=True)
    return 0


if __name__=='__main__':
    try: sys.exit(run())
    except (OSError,RuntimeError,ValueError,KeyError) as exc:
        print(f'ERROR: {exc}',file=sys.stderr);sys.exit(1)
