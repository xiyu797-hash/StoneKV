# StoneKV 面试训练：必须会写 / 必须会讲 / 了解即可

## 必须会写

1. **WAL Record 编解码与 CRC32**：手写一个小端序字段写入函数；写 `seq/klen/vlen`，说明 CRC 覆盖范围，以及遇到部分写入时如何恢复。
2. **MemTable 基础操作**：用 `std::map<string,Record>` 维护 Put、Delete、Get，说明为何遍历有序，和 `unordered_map` 的复杂度区别。
3. **两路 SST 有序归并**：给两个键有序序列及 seq/tombstone，写出保留最新版本并输出升序的算法。
4. **简单线程安全接口**：用 `std::mutex` + `lock_guard` 保证 Put/Get 串行，说明锁作用域，以及为何性能仍有瓶颈。
5. **CTest 和故障恢复测试**：给定“写入后崩溃”，写验证重启仍可读的测试（可以使用 fork/_exit）。

## 必须会讲

**Q1：为什么 WAL 一定要先写，再更新 MemTable？** 先更新内存可能在 WAL 未落盘时崩溃，已返回成功的数据无法恢复。正确顺序为 WAL 追加 -> fsync（同步模式） -> 更新内存。这里的持久化保证以本地文件系统和正确 fsync 为前提。

**Q2：为什么每次 Put 默认 `fsync` 会慢？** 需要等待写缓冲同步到持久介质；逐条刷盘相比批量 group commit 吞吐显著较低。`sync_writes=false` 不可被描述为同等可靠模式。

**Q3：Bloom Filter 有误判吗？** 可能误报（不存在却判为可能存在），不应漏报；它减少不必要磁盘探测，但不会保证 key 一定存在。该实现每次重启重建 Bloom。

**Q4：为什么还需要 MANIFEST？** SST 已写到磁盘不等于逻辑可见；MANIFEST 给出“哪些 SST 被提交”的单一权威清单，从而提供失败前后的可恢复状态。

**Q5：如果 MANIFEST 提交成功但 WAL 尚未截断，是否会重复执行？** `durable_seq` 作为水位线，重放时跳过 `seq <= durable_seq` 的记录。

**Q6：压缩合并为何能清理 Tombstone？** 当前是全量 SST 合并，覆盖了所有历史磁盘版本，且 Flush 后 MemTable 为空；没有更老版本会重新出现。如果换成部分层 Compaction，就不能无条件清理墓碑。

**Q7：为什么要同时使用内存锁和文件锁？** `std::mutex` 保护线程共享状态；`flock` 排除不同进程同时修改同一目录。两者解决不同范围的竞争。

**Q8：该版本真正用了 RocksDB 的 LSM 树吗？** 只采用 LSM 关键思想（WAL/有序内存表/不可变 SST/Compaction）；没有多层 L0-Ln、异步刷盘、Block Cache，因此准确的名称是 *LSM-inspired*，不能说自己实现了完整 RocksDB。

**Q9：性能瓶颈在哪里？** 单个大锁、每写一次 fsync、SST 全量 key 索引的内存成本、Get 每次打开文件、同步 Compaction 和缺少块缓存；可以用 flamegraph/perf、fio、不同 key/value 分布量化。

**Q10：该项目真的经历过断电测试吗？** 没有。`fork + _exit` 测试了 5 个逻辑崩溃点，不能替代物理断电、存储控制器故障和长期运行测试。面试时必须如此回答。

## 建议手写练习

- 练习 A（15 分钟）：写一个 `encodeWal(seq, key, value)` 返回 `std::string` 的函数，包含 `put64`、`put32`、CRC。
- 练习 B（20 分钟）：写一个两路合并，从两个按 key 排序数组保留更大 seq；特别处理覆盖和删除。
- 练习 C（10 分钟）：解释 `Put` 的锁范围；如果锁缩小到只保护 `mem` 会不会出现 WAL sequence 乱序？
- 练习 D（15 分钟）：画图解释“创建 SST → 写 MANIFEST → 清空 WAL”的次序为何不能颠倒。
