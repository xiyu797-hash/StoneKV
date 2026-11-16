#!/usr/bin/env python3
import json
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='stonekv-io-profile-') as tmp:
    output = Path(tmp) / 'run'
    subprocess.run([sys.argv[1], str(output), '8', '1', '42', '2'], check=True)
    data = json.loads((output / 'io_profile.json').read_text())
    # Eight 13-byte keys, 128-byte values; scans return 8+...+1 rows.
    assert data['logical_returned_key_value_bytes'] == (8 + 4 + 2 + 36) * 141
    assert data['logical_mutation_key_value_bytes'] == (3 * 8 + 4) * 141 + 2 * 13
    assert data['syscall_pread_bytes'] > 0
    assert data['syscall_write_pwrite_bytes'] > data['logical_mutation_key_value_bytes']
