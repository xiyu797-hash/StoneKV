# 架构设计与崩溃一致性说明

## 数据路径

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
6. **仅一个 writer 实例**：`flock` 排除跨进程冲突；`std::mutex` 将所有进程内操作串行化。

## WAL 格式（小端）

```
Magic("SKW1")   4 bytes
Sequence       8 bytes
Kind           1 byte  (1=put,2=delete)
Key length     4 bytes
Value length   4 bytes
CRC32          4 bytes  (覆盖 seq+kind+klen+vlen+key+value)
Key            klen bytes
Value          vlen bytes
```

从 offset 0 扫描；不足一个 Header 或 Value 的最后记录视为意外中断，截断尾部后保留前面完整记录；若完整记录校验失败，拒绝启动（避免掩盖磁盘损坏）。

## SST 格式（小端）

```
Magic("STONEKV1")  8 bytes
Entry count        8 bytes
Repeated entry:
    Sequence       8 bytes
    Kind           1 byte
    Key length     4 bytes
    Value length   4 bytes
    CRC32          4 bytes (覆盖上述17字节+key+value)
    Key            klen bytes
    Value          vlen bytes
```

每个 SST 文件的键严格递增。启动时扫描 SST 校验 CRC 并重建**所有 key 的内存 offset index**和 Bloom Filter（布隆过滤器并未落盘）。ReadAt 按 offset 读取 Value 并再次验证 CRC。此实现没有 block cache；频繁随机读需要 open/read/close，读密集性能远不及成熟存储引擎。

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
