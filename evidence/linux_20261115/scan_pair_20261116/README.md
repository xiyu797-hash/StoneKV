# Range scan allocation regression probe

The original `975c0e7` code copied the entire active MemTable into a merge map even when every key was outside the requested range. The 300-unrelated-key allocation regression in `tests/scan_bounds_test.cpp` failed before the fix (232 versus 832 allocations) and passed afterwards (232 versus 232). The separate paired timing probe creates a new DB for each run with 200 persisted `s...` records and 200 out-of-range active `r...` records, then checks 5,000 scan results (limit 16). Sync writes are off and Flush is explicit. The OS cache was not cleared and other server tasks ran. It reports total wall time, not percentiles or storage throughput.

Commands from the repository root (use unique output paths; never reuse an existing database):

```sh
git show 975c0e7:src/db.cpp > /tmp/stonekv_scan_before_20261116.cpp
g++ -std=c++17 -O3 -DNDEBUG -pthread -I include /tmp/stonekv_scan_before_20261116.cpp evidence/linux_20261115/scan_pair_20261116/probe.cpp -o /tmp/stonekv_scan_before_20261116
g++ -std=c++17 -O3 -DNDEBUG -pthread -I include src/db.cpp evidence/linux_20261115/scan_pair_20261116/probe.cpp -o /tmp/stonekv_scan_after_20261116
# Run one process per fresh database; order in summary.csv is before/after, after/before, before/after.
```

Raw command arrays, stdout, stderr and exit codes are in the JSON files, and timings in `summary.csv`. The `-pg` gprof diagnostic was separate from timing; sampling symbols are affected by compiler optimization and are not treated as precise percentages. Its call graph shows repeated `parseSparseBlock` and map operations, motivating the narrower allocation regression; no checksum checks were removed. Builds include `src/db.cpp` at the exact tested commits; `src/db.cpp` at the report's HEAD must match `6a94f1d` to reproduce the after binary.
