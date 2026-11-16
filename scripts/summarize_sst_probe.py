#!/usr/bin/env python3
"""Summarize sst_index_probe raw read traces and GNU time files without changing inputs."""
import argparse,csv,math,statistics
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('directory',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
rows=[]
for variant in ('baseline','sparse'):
 for r in (1,2,3):
  lines=(a.directory/f'{variant}_r{r}.csv').read_text().splitlines();start=lines.index('index,latency_ns');values=[int(x['latency_ns']) for x in csv.DictReader(lines[start:])]
  if len(values)!=5000:raise ValueError(f'{variant} round {r}: expected 5000 samples, got {len(values)}')
  values.sort();pct=lambda x:values[math.ceil(x*len(values))-1]/1000
  ts=dict(x.split('=',1) for x in (a.directory/f'{variant}_r{r}.time').read_text().splitlines())
  if ts['exit']!='0':raise ValueError(f'{variant} round {r}: nonzero exit')
  row={'variant':variant,'round':r,'reads':len(values),'open_us':int(lines[0].split('=')[1]),'p50_us':pct(.5),'p95_us':pct(.95),'p99_us':pct(.99),'mean_us':statistics.mean(values)/1000,'max_rss_kib':int(ts['max_rss_kib']),'wall_seconds':ts['wall_seconds']}
  rows.append(row)
with a.output.open('x',newline='') as f:
 w=csv.DictWriter(f,fieldnames=rows[0]);w.writeheader();w.writerows(rows)
print(f'PASS {len(rows)} rounds; raw trace rows validated; summary={a.output}')
