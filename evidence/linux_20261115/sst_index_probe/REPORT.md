# SST before/after probe (2026-11-15, Linux)

Original source `2318bb65bc0d13b562dc1cee755355e6690c142f` versus V3 sparse/cache `65d33c6`; identical `tools/sst_index_probe.cpp`, GCC 13.3, `-O3 -DNDEBUG -pthread`, distinct database directories on the same filesystem. Each database contains 20,000 identical 61-byte keys and 256-byte values, one SST, sync writes disabled then explicit Flush. Three alternating read rounds (baseline/sparse, sparse/baseline, baseline/sparse); each opens the DB and performs 5,000 seeded random Get calls (seed42). Application block cache starts empty each process; OS page cache was not dropped and server is shared. Output traces include each API latency; `summary.csv` is nearest-rank per-round percentiles. Disk bytes count all DB regular files. Do not infer power-loss durability, sustained throughput, or phase-isolated index memory.

| Metric | original V1/full index | V3 sparse index, default 8MiB block cache |
|---|---:|---:|
| On-disk DB bytes | 6,760,063 | 6,793,951 |
| Retained index entries | full per-key index (20,000 records in fixture) | 0 full-index entries; 104 sparse block entries |
| Process peak RSS | 6,536–6,736 KiB | 10,484–10,736 KiB |
| Open time | 19.728–20.551 ms | 29.137–30.016 ms |
| Random Get p50 | 1.773–1.883 µs | 1.062–1.093 µs |
| Random Get p95 | 2.074–2.134 µs | 1.533–1.663 µs |
| Random Get p99 | 2.214–2.264 µs | 112.661–116.518 µs |
| Random Get mean | 1.791–1.908 µs | 3.391–3.524 µs |

The V3 process RSS is higher in this small read run despite fewer retained index entries, because peak RSS includes block-cache payload and all other allocations. First accesses to blocks pay reading and CRC costs; they contribute to the tail. This is a measured regression in p99/mean/open/RSS for this short workload, not evidence that all workloads regress. V3 scans all records for validation during open and Scan/Compaction temporarily materialize key indexes; data set, cache, concurrency and scale materially affect results. Raw per-operation CSV and GNU time files are retained; DB fixtures remain under `/tmp/stonekv_sst_probe_20261115/` and were not deleted.

This paragraph describes the measured commit `65d33c6`. Commit `0f1f028` subsequently changed range scans to stream sparse blocks; compaction still temporarily materializes key indexes. See the later workload report for its measured scan results.
