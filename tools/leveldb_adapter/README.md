# Optional LevelDB comparison adapter

This benchmark-only header lets the **same** `tools/benchmark_workloads.cpp` use LevelDB. LevelDB is not a StoneKV dependency and is built separately. The adapter uses LevelDB's internal `DBImpl::TEST_CompactMemTable` to create the workload's Flush boundary; `CompactRange` and StoneKV's full compaction have different algorithms and must not be compared as equivalent operations. LevelDB internal APIs may change; use the revision below.

On the reported server, LevelDB `29465588d3dfdc80fba5e198037582b8fdb5dd60` was fetched in an isolated directory and built with Release, tests and benchmark targets disabled, no Snappy or Zstd. Example commands (choose new directories):

```sh
git clone https://github.com/google/leveldb.git /tmp/leveldb-reference
git -C /tmp/leveldb-reference checkout 29465588d3dfdc80fba5e198037582b8fdb5dd60
cmake -S /tmp/leveldb-reference -B /tmp/leveldb-reference-build -DCMAKE_BUILD_TYPE=Release -DLEVELDB_BUILD_TESTS=OFF -DLEVELDB_BUILD_BENCHMARKS=OFF
cmake --build /tmp/leveldb-reference-build --parallel 2
g++ -std=c++17 -O3 -DNDEBUG -pthread -DLEVELDB_PLATFORM_POSIX=1 -I tools/leveldb_adapter -I /tmp/leveldb-reference/include -I /tmp/leveldb-reference -I /tmp/leveldb-reference-build/include tools/benchmark_workloads.cpp /tmp/leveldb-reference-build/libleveldb.a -o /tmp/leveldb-workload
python3 scripts/run_leveldb_comparison.py --stonekv /tmp/stonekv-release/stonekv_benchmark_workloads --leveldb /tmp/leveldb-workload --output /tmp/stonekv-leveldb-comparison --count 200 --rounds 3 --threads 4 --seed 42
```

Both binaries run identical operations/values/seeds, a nominal 128 KiB write buffer, 8 MiB block cache, 10-bit/key Bloom, no compression and the same sync flag; neither has a separate prewarm phase. In sync mode both request a synchronized WAL write; the implementations and filesystem calls differ, and this is not a proof of identical power-loss behavior. LevelDB retains its automatic leveled compaction policy while StoneKV uses full compaction with trigger 1000 for this workload; file/table formats, checksums, cache internals and thresholds are not identical. Flush, compaction and final space are therefore descriptive only, not matched implementations. The runner alternates database order across rounds, each against a new directory, but runs sync=0 then sync=1 within each invocation. Raw per-operation times and process CPU/RSS are retained. Do not infer general throughput ratios from the short shared-machine run.
