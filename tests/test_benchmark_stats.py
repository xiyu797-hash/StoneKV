#!/usr/bin/env python3
"""Exercise actual workloads and check that recorded counters reach the summary."""
import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile

runner, binary = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='stonekv-benchmark-stats-') as temporary:
    output = Path(temporary) / 'run'
    subprocess.run([sys.executable, runner, '--binary', binary, '--output', str(output),
                    '--count', '8', '--rounds', '1', '--threads', '2', '--seed', '42'], check=True)
    with (output / 'summary.csv').open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    for sync in (0, 1):
        stats = json.loads((output / f'sync{sync}_round1' / 'stats.json').read_text())
        assert stats['compaction_count'] >= 1
        assert stats['compaction_output_bytes'] > 0
        matching = [r for r in rows if r['sync'] == str(sync)]
        assert matching
        for row in matching:
            for key, value in stats.items():
                assert int(row[key]) == value
