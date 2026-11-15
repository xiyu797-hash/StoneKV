# 架构设计与崩溃一致性说明

## 数据路径

下图为原同步实现的历史示意。当前 Flush 使用有界 immutable + 后台工作线程；精确锁与 WAL 回收语义见 [BACKGROUND_FLUSH.md](BACKGROUND_FLUSH.md)。Compaction 由单一后台工作线程执行，读取与新写入可在合并 SST 构建期间继续；显式 Compact 等待该任务结束。

```text
              Put / Delete (单进程 DB::mutex)
                          |
                          v
                WAL append + fsync
                          |
                          v
                 MemTable(std::map)
                          |
                     达到阈值
                          v
                SST.tmp + fsync
                          |
                  rename + fsync(dir)
                          v
             MANIFEST.tmp + fsync
                          |
                  rename + fsync(dir)
                          v
               reset WAL + fsync
                          |
                    SST 数达阈值
                          v
             Multiway Full Compaction
                          |
                      New SST
                          |
                 MANIFEST atomic commit
                          |
                   Remove old SST
```

### 基本不变量

1. **已确认写入必须先进入 WAL**：默认每笔 Put/Delete 都等待 WAL 的 `fsync` 成功。
2. **不可变文件提交必须按顺序**：新 SST 的 `fsync` 和 rename 成功后才允许 MANIFEST 引用它；MANIFEST 的目录同步成功后才允许截断 WAL。
3. **恢复有界重放**：`durable_seq` 表示已进入 MANIFEST 管辖 SST 的最大序号；重启后 WAL 中 `seq <= durable_seq` 的记录跳过，以避免写入被双重应用。
4. **Compaction 先提交后回收**：新 SST 被 manifest 引用前，旧 SST 都仍在；成功提交后才删除旧文件，崩溃后孤儿文件由进程锁保护下清理。
5. **删除是记录**：Tombstone 参与读取覆盖，只有所有旧 SST 一并合并时才安全移除。
6. **仅一个 writer 实例**：`flock` 排除跨进程冲突；`std::mutex` 串行化内存与 MANIFEST 状态转移，后台 SST 生成在锁外执行。

## WAL 格式（小端）

```
Magic("SKW2")   4 bytes
Sequence       8 bytes
Kind           1 byte  (1=put,2=delete)
Key length     4 bytes
Value length   4 bytes
CRC32          4 bytes  (覆盖 seq+kind+klen+vlen+key+value)
Header CRC32   4 bytes  (覆盖 magic 与之前完整头)
Key            klen bytes
Value          vlen bytes
```

批量记录使用 `SKB2`，在旧批量头后追加独立头 CRC，批内载荷仍使用原有 CRC 逐批验证。读取支持 `SKW1`/`SKB1` 与 V2 混合，但旧二进制不能读取 V2 WAL；降级前必须先 Flush 并备份。V2 在信任长度前验证完整头 CRC，防止已写入头部长度遭损坏后被误判为撕裂尾部。旧 V1 没有独立头 CRC，无法区分合理长度字段损坏和真正撕裂。所有版本中，完整合法头之后发生的尾部物理截断也无法区分未确认写入的中断和已确认数据被截断；当前策略截断不完整尾部，并不保证检测后者。完整记录 CRC 错误则拒绝启动。

## SST 格式（小端）

新写入为 `STONEKV2`：8 字节 magic，8 字节记录数，然后重复 block：`u32 payload_length`、`u32 payload_crc32`、payload。普通 block 的目标大小为 64 KiB；单条大记录可单独占一个更大的 block，受 key/value 上限约束。payload 内记录仍为 `seq:u64, kind:u8, klen:u32, vlen:u32, record_crc32:u32, key, value`，记录和 block 均有 CRC。空 SST 有0条记录和无 block。打开时检查 block 长度、CRC、记录长度、记录 CRC、排序和尾部；点读时重新验证记录 CRC。

读取兼容旧 `STONEKV1`（16字节 magic/记录数之后直接顺序记录）。无法识别的 magic/version 安全拒绝；旧二进制不能读取 V2 表，**即使 Flush 也不能把 V2 SST 变回 V1**，要降级需另行导出并用旧程序重建。格式更新前请备份；本项目未提供降级工具。

当前 V2 是 block framing 的第一阶段：启动仍扫描所有记录、重建全量 key-offset 索引及内存 Bloom；Bloom 尚未持久化，稀疏索引尚未实现。V2 block 使用基于载荷字节计量的 LRU，默认8 MiB、可配置0至256 MiB；超出容量的单块按需读取但不缓存。容量统计不包括容器元数据。V1 记录仍按原方式读取，不进入该 block cache。点查和扫描复用最多32个只读 SST 描述符（LRU），Compaction 为源 SST 使用独立读取句柄。不得将 V2 描述为已完成全部 block 索引与缓存优化。

## MANIFEST 格式及事务提交

```
STONEKV_MANIFEST_V1
<durable_seq> <next_file_id> <table_count>
<table_id_1>
...
crc32 <hex>
```

先写临时文件，`fsync(temp)`，原子 `rename`，`fsync(directory)`。CRC32 覆盖校验行之前的全部字节。不声称单机文件系统以外的跨节点事务。

### 崩溃窗口

| 崩溃位置 | 重启路径 | 是否丢失已确认 WAL 写入 |
| --- | --- | --- |
| SST 临时文件未提交 | MANIFEST 仍是旧版本，WAL 重放；清理孤儿 `.tmp` | 否（在 WAL 持久的前提下） |
| SST rename 后、MANIFEST 前 | 新 SST 未被引用，WAL 重放；清理孤儿 SST | 否 |
| MANIFEST rename 后、WAL truncate 前 | 新 SST 可见，按 `durable_seq` 跳过旧 WAL | 否 |
| WAL truncate 后 | 以 SST + MANIFEST 恢复 | 否 |
| Compaction 新 SST rename 后、MANIFEST 前 | 原 SST 仍被引用，丢弃新孤儿 SST | 否 |
| Compaction MANIFEST commit 后、旧 SST 删除前 | 新 SST 被引用，清理原 SST | 否 |

**假设：** Linux/POSIX 文件系统语义、正确实现 fsync/rename 的本地存储、非恶意修改文件；未模拟硬盘掉电和控制器缓存不兑现 fsync 的情形。`_exit(86)` 是进程级故障注入，不是物理断电测试。

## 复杂度分析

令 `M` 为 MemTable key 数，`T` 为 SST 数，`K_i` 为第 i 个 SST key 数。

- `Put`：`O(log M)` 插入（不计 WAL fsync）；阈值刷盘及 compaction 产生同步延迟尖峰。
- `Get`：MemTable `O(log M)`；每个 SST 先查 Bloom，再对 key-offset index 二分 `O(log K_i)`；命中后磁盘读取值。
- `Scan`：多路归并堆大小 `O(T+1)`，每个候选键 `O(log(T+1))`；另有 MemTable 范围构造和输出数据开销。
- `Full Compaction`：按多路归并遍历全部 SST 的索引，`O(total_entries * log T)`，以 key offset index 常驻内存和顺序写作为代价。
- 空间：MemTable 和**所有磁盘 key 的内存 index** + Bloom 常驻内存，数据值主要保留在磁盘文件；不是内存资源上限严格受控的生产存储引擎。

## 待迭代技术任务

1. 增加 SST block 组织、prefix compression、block cache、持久化稀疏索引。
2. immutable memtable + 后台 flush/compaction，锁分片、降低写放大和尾延迟。
3. 支持 size-tiered/leveled compaction，测试 write/read/space amplification。
4. 分离 WAL segment；支持操作批次、原子提交与一致性快照。
5. 使用 fio/perf/strace、page cache 预热、长稳测试、随机 kill 重启和 ext4/xfs 对照验证。
