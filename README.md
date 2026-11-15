# StoneKV v1.0 — C++17/Linux 本地持久化 KV 引擎

> 后台 Flush：一个有界 immutable MemTable 由工作线程生成 SST；写入背压、WAL 回收与关闭语义见 [后台 Flush 设计](docs/BACKGROUND_FLUSH.md)。当前 Compaction 仍为同步实现；分段 WAL、后台 Compaction 和 block cache 尚未实现。

> 一个可编译、可运行、可故障注入和可测试的 **LSM-inspired（LSM 思想）教学/求职项目**，不是 RocksDB 替代品。项目由独立文件格式、WAL + fsync、MemTable、SSTable、Bloom Filter、原子 MANIFEST、同步全量 Compaction 构成。请先运行测试，再根据自己的理解决定是否写入简历。

## 1. 项目能力

| 组件 | 状态 | 实现与边界 |
| --- | --- | --- |
| API | ✅ | `Put / Get / Delete / Scan / Flush / Compact / GetStats`，二进制键值 |
| WAL | ✅ | `SKW1` 记录、单调 seq、CRC32、尾部不完整记录截断；中间 CRC 错误拒绝开启 |
| 持久化 | ✅ | 默认每次写入后 `fsync(WAL)`；SST 与 MANIFEST 均 `fsync + rename + fsync(dir)` |
| MemTable | ✅ | `std::map` 有序索引，超过内存阈值同步刷盘 |
| SSTable | ✅ | Immutable sorted file，记录级 CRC、内存全量 **key-offset 索引**，按需读取值 |
| Bloom Filter | ✅ | 每文件内存常驻、约 10 bits/key、7 轮哈希（启动时重建，非持久化） |
| 读取 / 范围查询 | ✅ | `Get` 从新到旧查；`Scan` 多路有序归并、尊重覆盖与 Tombstone |
| Compaction | ✅ | 基于多路归并的**同步全量合并**，完成后清理旧 SST；非 leveled compaction |
| 一致性与线程 | ✅ | `std::mutex` 单实例序列化；OS `flock` 防止两个进程同时打开 |
| 故障测试 | ✅ | 5 个命名崩溃注入点、WAL 尾部恢复、WAL/SST 损坏拒绝、随机模型核对 |
| 工具 | ✅ | CLI、参数化基准测试、CTest、ASan/UBSan、Linux CI 模板 |

**未实现：** 分布式副本、事务/批量原子提交、MVCC/快照、后台 Compaction、真正的分层 LSM、分块 Block Cache、压缩、动态内存上限、跨进程并发共享、Windows 原生文件 I/O、网络接口、全路径断电测试。

## 1.1 工程增强分支说明（待合并）

- `WriteBatch`：在一条 `SKB2` WAL 记录中提交最多 4096 个操作、最多 32 MiB 编码载荷；重放时整批验证 CRC 后才应用。同一键多次操作以最后一次为准。**保证记录重放原子性，不提供快照隔离/MVCC**。
- 全量 Compaction 使用每个 SST 一个只读文件句柄，避免为每个 key 单独 open/close；尚未在服务器上形成配对性能结论。
- 新增 5 个批量写入边界/损坏/崩溃测试，CI 覆盖 GCC/Clang Release 及 GCC ASan/UBSan。
- 服务器复现实验见 [`docs/SERVER_TASKBOOK.md`](docs/SERVER_TASKBOOK.md) 和 `scripts/server_validation.sh`。

## 2. 三条命令开始（Linux / WSL2）

需要 **Linux、CMake ≥3.16、支持 C++17 的 g++/clang++**：

```bash
cd StoneKV
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j 4
ctest --test-dir build-release --output-on-failure
```

手动试用（会在 `/tmp/stonekv-demo` 创建数据目录；已有数据不会自动清除）：

```bash
./build-release/stonekv_cli /tmp/stonekv-demo put name StoneKV
./build-release/stonekv_cli /tmp/stonekv-demo get name
./build-release/stonekv_cli /tmp/stonekv-demo put city Hangzhou
./build-release/stonekv_cli /tmp/stonekv-demo scan
./build-release/stonekv_cli /tmp/stonekv-demo del name
./build-release/stonekv_cli /tmp/stonekv-demo flush
./build-release/stonekv_cli /tmp/stonekv-demo stats
./build-release/stonekv_cli /tmp/stonekv-demo compact
```

每次 CLI 命令会独立打开/关闭同一个数据目录，以便演示持久化；生产程序可直接在一个进程中长期使用 C++ API。

### C++ 集成示例

```cpp
#include "stonekv/db.hpp"
#include <iostream>

int main() {
    stonekv::DB db("./data"); // 默认每次 Put / Delete fsync WAL
    db.Put("user:1", "Alice");
    db.Put("user:2", "Bob");
    if (auto val = db.Get("user:1")) std::cout << *val << '\n';
    for (auto& [k, v] : db.Scan("user:", "user;", 100))
        std::cout << k << " -> " << v << '\n';
    db.Delete("user:1");
    db.Flush();
}
```

## 3. 完整测试及性能实验

```bash
# Release + CTest
./scripts/verify_linux.sh

# Release + ASan/UBSan
./scripts/verify_linux.sh --asan

# benchmark：请使用一个不存在或空的实验目录，绝不覆盖已有数据
./build-release/stonekv_bench /tmp/stonekv-bench-001 20000 128 0 42
./build-release/stonekv_bench /tmp/stonekv-bench-sync 3000 128 1 42
```

CSV 输出：`operation,count,elapsed_ms,ops_per_sec`。`sync_writes=0` 用于分析数据结构/磁盘写入开销，但**不是可靠写入确认**，不能与 `sync_writes=1` 比较后宣称真实落盘吞吐。单机不同设备、缓存、并发和内核会产生很大波动；必须记录 CPU/磁盘/内核、预热、样本数和 P50/P99，不能将单次结果写为稳定生产性能。

在 Windows 11 上推荐使用 WSL2 Ubuntu：

```powershell
wsl --install -d Ubuntu
```

安装完成后在 Ubuntu 终端中 `sudo apt update && sudo apt install -y build-essential cmake`，再将项目放到 WSL 文件系统（例如 `~/StoneKV`）运行。**此版本不支持原生 PowerShell + MSYS2/MinGW 构建**，因为使用 `flock / fsync / pread / pwrite / fork` 等 POSIX 接口。

## 4. 数据目录及 API 语义

```text
data/
├── LOCK                  # flock，单进程独占
├── MANIFEST              # 已提交 SST 文件 ID + durable_seq + CRC
├── active.wal            # 未刷盘或尚未清理的 WAL 记录
└── SST-000000000001.sst  # 不可变 SST；Compaction 后代际替换
```

- Key 1..4096 bytes；Value 0..8 MiB；支持 `\0`、二进制字节，空 value 与 NotFound 严格区分。
- `Scan(begin,end,limit)` 按字节序升序返回半开区间 `[begin,end)`；`end=""` 表示不设置上界；默认上限 100，最大 100000。
- `Delete` 写 Tombstone，并不会立即移除文件里的旧值；全量 Compaction 安全删除被覆盖旧版本。
- 新写入使用 `SKW2`/`SKB2`，头部有独立 CRC；可重放旧版 `SKW1`/`SKB1`。旧二进制无法读取包含 V2 记录的 WAL，降级前必须先完成 Flush 并备份。旧 V1 的长度损坏与未完成尾部不可可靠区分；即使 V2 的头完整，已确认数据后续被物理截断与未确认写入的撕裂尾部也不可仅凭该格式区分。
- 若打开时发现 WAL **尾部不完整**，只截断最后不完整片段；已完整的记录 CRC 错误则抛异常，**不静默跳过**。SST CRC 错误亦拒绝打开。
- 操作返回成功时：默认 `sync_writes=true` 表示 WAL 已 `fsync`，但并非跨设备/主机复制或断电级别的全系统保证。
- `sync_writes=false` 适合基准测试/临时数据，不提供每次操作的可靠落盘确认。`Flush()` 成功会确保此前的 MemTable 已持久化。
- I/O 错误可能使当前调用处于“提交不确定”状态，实例会被标记为不可继续写入；应关闭并重新打开排查。
- 此存储不允许同一目录被两个 DB 实例或进程同时打开。内部粗粒度锁保证线程安全，不意味着高并发吞吐。

## 5. 阅读路线

1. `include/stonekv/db.hpp`：先理解 API/异常与 options。
2. `src/db.cpp` 搜 `appendWal/replayWal`：WAL 二进制格式、校验、恢复。
3. 搜 `flushLocked/saveManifest`：刷盘顺序和崩溃一致性。
4. 搜 `loadSST/readAt`：SST 索引、Bloom、CRC。
5. 搜 `compactLocked/scan`：多路归并与 Tombstone。
6. `tests/test_db.cpp`：逐条测试，重点看 5 个 `crash_*` 测试。
7. 阅读 [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)、[`docs/INTERVIEW_GUIDE.md`](docs/INTERVIEW_GUIDE.md)、[`docs/RESUME_CLAIMS.md`](docs/RESUME_CLAIMS.md)。

## 6. 注意事项

StoneKV 是一个**独立可运行的基础版存储引擎**。它可以作为 Linux C++ 求职时展示文件 IO、异常恢复、数据结构、并发保护的起点，但没有可复现的复杂工作负载、长时间断电实验和真实业务接入之前，不应宣称工业级数据库或达到 RocksDB 水平。项目完成不等于已经掌握，建议先手写 WAL 编码、恢复和两路 SST 归并，再参加相关面试。
