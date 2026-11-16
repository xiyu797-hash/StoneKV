# Workload benchmark

Build a Release binary and run a new output directory (the runner refuses to reuse one):

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 4
python3 scripts/run_benchmark_workloads.py --binary build-release/stonekv_benchmark_workloads --output /tmp/stonekv-run-unique --count 200 --rounds 3 --threads 4 --seed 42
```

The runner records each command and exit status, latency of each API operation, wall time, process CPU and peak RSS, final database file size, and per-phase throughput plus nearest-rank P50/P95/P99. It separately tests `sync_writes=0` and `1`: unsynced acknowledgements are not comparable with durable acknowledgements; `Flush` is a durability boundary. Multi-thread figures include mutex contention and scheduler delays. Flush, compaction, close, and reopen have one sample per phase and their percentiles are that single observation. CPU and RSS are whole-process, repeated per phase, not phase-specific or index-only. Current binaries also record compaction counts, duration and output bytes, cache counters, and a background task count snapshot before close in `stats.json`; these instance-wide counters are repeated on every phase row, and the task snapshot is not a queue peak. Historical binaries omit these fields. Read/write amplification and peak background queue depth remain NOT RUN.

`evidence/linux_20261115/workloads_baseline/` compares the exact same workload program against archived commit `2318bb6`; its environment file refers to the runner checkout, not the compiled historical source, see `SOURCE.txt`. `workloads_p0/` uses the hardened P0 source; both are small shared-machine feasibility measurements, not independent isolated steady-state performance claims. Actual per-operation CSVs remain in `/tmp/stonekv_workloads_baseline_20261115` and `/tmp/stonekv_workloads_p0_20261115`; copies under evidence are included for reproducibility.

For the 20,000-key SST-format comparison, see `evidence/linux_20261115/sst_index_probe/REPORT.md`. The checked workload shows a higher p99, peak process RSS and reopen time for V3 despite fewer retained index entries; report regressions rather than interpreting the sparse metadata alone as a whole-process memory win.
