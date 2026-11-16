#!/usr/bin/env python3
"""Structured stats output includes actual DB sequence and bounded-cache fields."""
import json, os, pathlib, subprocess, sys, tempfile
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
    env = dict(os.environ, STONEKV_LOG_JSON='1')
    ok = subprocess.run([sys.argv[1],db,'get','key'],capture_output=True,text=True,env=env)
    assert ok.returncode == 0 and ok.stdout == 'value\n'
    events = [json.loads(line) for line in ok.stderr.splitlines()]
    assert [e['event'] for e in events] == ['operation_start','operation_end']
    assert events[-1]['exit_code'] == 0 and events[-1]['duration_us'] >= 0
    assert all(e['operation'] == 'get' and isinstance(e['time_unix_ms'],int) for e in events)
    assert 'key' not in ok.stderr and 'value' not in ok.stderr
    missing = subprocess.run([sys.argv[1],db,'get','missing'],capture_output=True,text=True,env=env)
    assert missing.returncode == 4 and missing.stdout == 'NOT_FOUND\n'
    assert json.loads(missing.stderr.splitlines()[-1])['exit_code'] == 4
    bad = subprocess.run([sys.argv[1],db,'no_such_operation'],capture_output=True,text=True,env=env)
    assert bad.returncode == 2
    events = [json.loads(line) for line in bad.stderr.splitlines()]
    assert [e['event'] for e in events] == ['operation_start','operation_error','operation_end']
    assert events[-1]['exit_code'] == 2 and events[-1]['operation'] == 'invalid'
    invalid_env = dict(os.environ,STONEKV_LOG_JSON='bad')
    invalid = subprocess.run([sys.argv[1],db,'get','key'],capture_output=True,text=True,env=invalid_env)
    assert invalid.returncode == 2 and 'STONEKV_LOG_JSON' in invalid.stderr
print('PASS stats-json is parseable and reflects committed data')
