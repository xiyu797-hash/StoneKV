# 简历表述与验证边界

实际经历必须由本人核对，不能把代码生成或参与测试直接写成本人独立设计和实现全部模块。以下是源码和测试能支持的项目事实，详细数据与条件见 [服务器报告](../evidence/linux_20261115/REPORT_20261116.md)。

- Linux C++17 本地 KV 原型，包含 WAL、WriteBatch、MemTable、SST、Bloom Filter、MANIFEST、删除标记和范围扫描。
- 基于单后台工作线程、单 immutable 槽与写入背压完成后台 Flush 和全量 Compaction；SST 使用持久化稀疏索引、校验块、有限容量缓存和有限文件句柄复用。
- WAL 使用记录校验及序列号，测试了进程异常退出、模拟写入与 fsync 故障后的恢复。2000 轮 SIGKILL 实验验证了 34,595 次已确认写入；它不证明断电或任意硬件故障下零丢失。
- GCC Release 与 ASan/UBSan 的 CTest 各 11/11 通过；本机 TSan 因运行环境内存映射失败无法给出结果，远端 [GitHub Actions](https://github.com/xiyu797-hash/StoneKV/actions/runs/43787090194) 的 GCC/Clang、ASan/UBSan 和 TSan 任务均通过。

不要声称达到 RocksDB 生产成熟度或比 RocksDB 快；目前没有 RocksDB 对照。LevelDB 仅有报告所述的小规模共享服务器对照，范围扫描明显慢于 LevelDB，不能推断所有场景或隐藏退化。`sync_writes=0` 的吞吐不等价于每次写入都持久化。一次进程级崩溃实验和模拟 I/O 故障也不等于真实断电、文件系统、磁盘故障或长期线上可靠性验证。
