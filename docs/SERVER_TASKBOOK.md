# StoneKV server validation & production-gap taskbook

> Historical taskbook for the earlier synchronous implementation. For the
> 2026-11-16 branch status and measured evidence, see
> [the current validation report](../evidence/linux_20261115/REPORT_20261116.md).
> The status table below describes that earlier snapshot, not the current branch.

**Scope:** This branch adds atomic WAL `WriteBatch`, reusable SST file handles during full compaction, five batch tests, and 3-job CI. It does **not** turn StoneKV into a production-ready replacement for RocksDB. Run all experiments on disposable data directories only.

## Execute on Ubuntu server (foreground)

```bash
# Prefer a fresh directory, not an existing production database.
git clone https://github.com/xiyu797-hash/StoneKV.git
cd StoneKV
# After reviewing and checking out the hardening branch / merged commit:
git log -1 --oneline
./scripts/server_validation.sh
```

The script creates `/tmp/stonekv-validation.XXXXXX` and prints its path; it does not delete user data. It produces `environment.txt`, GCC/Clang Release build and CTest logs, GCC ASan+UBSan logs, and three repeat benchmarks for each of `sync_writes=0` and `sync_writes=1`. A missing Clang compiler is marked SKIPPED. Failures terminate with a nonzero status.

**Run on a dedicated machine or record contention.** Do not put sensitive data in benchmark directories. Publish logs only after reviewing any host information. Use `iostat`, `pidstat`, `perf stat`, `strace -c` on a separate run as available; their overhead can change results. Record kernel/CPU/memory/storage/fs type; test ext4/xfs if supported.

## Acceptance criteria and next milestones

| Gate | Evidence / Pass condition | Current status |
|---|---|---|
| Build & correctness | GCC/Clang Release CTest, ASan+UBSan CTest pass | New code local validation; CI/server pending |
| Atomic batch | One WAL record per batch; strict CRC; duplicates last-write-wins; crash after fsync replays entire batch | Implemented with 5 tests |
| Crash test | Repeated kill/abrupt process exit at every failpoint; validate exact model state after reopen | Limited fixed-point tests; randomized fault campaign pending |
| Disk fault | ENOSPC, EIO, short reads/writes via fault-injecting filesystem, no silent data loss | NOT RUN |
| Benchmark | Multiple repetitions, fixed seeds, explicit sync mode, compare same hardware/FS/build | Runner added; server measurements pending |
| Operational API | Health metrics, logs, backup/restore, documented recovery tooling | NOT IMPLEMENTED |
| Scale | File descriptor budget, memory index cap, compaction latency, disk amplification metrics | NOT IMPLEMENTED |
| Maintenance | Background compaction, SST block index/cache, snapshot/read isolation | NOT IMPLEMENTED |

## Critical measurement rules

- `sync_writes=0` is a throughput experiment **without per-operation durable acknowledgment**. Never compare it to `sync_writes=1` as equivalent durability.
- Full-compaction FD reuse is an implementation improvement; its speedup requires a controlled before/after comparison on the **same machine and workload**. Use a baseline checkout in a separate directory, with an identical compiler and flags. Do not infer a speedup from unrelated benchmark runs.
- Single-process `flock` and a global mutex serialize all operations. No scalable concurrent reads or transactions are claimed.
- Atomic `WriteBatch` here means *all-or-nothing replay of one WAL record* and a single sequence number. It does not implement snapshot isolation, MVCC, or cross-process transactions.
- The fault injector uses process exits, not sudden power loss; full power-loss durability is not proved.

## Remaining engineering order

1. Long-duration crash campaign with a persisted reference model and checksummed event log.
2. Inject ENOSPC/EIO and interrupted `fsync/rename`, verify poison/reopen rules.
3. Redesign SST as data blocks + sparse persistent index + Bloom/filter blocks, test binary keys and backward compatibility.
4. Immutable MemTable and background flush/compaction with bounded queue, shutdown protocol, read visibility correctness.
5. Add block cache, benchmark read/write/space amplification and p50/p99 tail latency.
6. Implement explicit file format versioning/migration and offline consistency checker (`fsck`).
7. Only then consider business integration, service API, security hardening and production deployability.
