#!/usr/bin/env python3
"""Structured stats output includes actual DB sequence and bounded-cache fields."""
import json, pathlib, subprocess, sys, tempfile
with tempfile.TemporaryDirectory(prefix='stonekv-cli-json-') as tmp:
    db=str(pathlib.Path(tmp)/'db')
    def invoke(*args):
        return subprocess.run([sys.argv[1],db,*args],capture_output=True,text=True,check=True)
    invoke('put','key','value')
    data=json.loads(invoke('stats-json').stdout)
    assert data['event']=='stats' and data['latest_sequence']==1
    assert data['sstable_count']==0 and data['wal_bytes']>0
    for key in ('compaction_count','compaction_micros','compaction_output_bytes','background_tasks','block_cache_bytes','block_cache_hits','block_cache_misses','sst_full_index_entries','sst_sparse_blocks'):
        assert isinstance(data[key],int) and data[key]>=0,key
print('PASS stats-json is parseable and reflects committed data')
