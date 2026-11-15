# 简历用语与验证边界

## 当前可基于源码/自动化测试陈述的事实

- C++17/Linux 实现 WAL、MemTable（std::map）、不可变 SST、Bloom Filter 与基于多路归并的同步全量 Compaction。
- 采用 `fsync + rename + fsync(directory)` 提交 SST/MANIFEST，使用 `durable_seq` 支撑崩溃后的 WAL 去重重放。
- 提供 `Put/Get/Delete/Scan` 接口、CLI、基准测试、独占文件锁和线程安全封装。
- 单机测试包含 23 项核心用例（以实际本机测试日志为准），以及五种可复现的进程崩溃注入时序。

## **暂时不要写入简历**的表述

- “自主从零设计实现了工业级高性能数据库”——没有工业级长期运行和大规模测试证据。
- “比 RocksDB 快 N 倍”——没有同配置的 RocksDB/LevelDB 测试。
- “完整 Leveled LSM Tree / SST Block Cache / 高吞吐异步 Compaction”——此版本未实现。
- “硬盘断电后 100% 不丢数据”——仅验证进程级 `_exit` 故障，不是完整断电验证。
- “千万 QPS”——不能把无 fsync 的小样本/缓存命中峰值当作持续吞吐。

## 等你亲自运行/理解之后，可参考的简历条目

**StoneKV｜Linux C++ 持久化 KV 存储引擎**（C++17 / POSIX IO / WAL / CRC32 / SSTable / CMake）

- 构建基于 WAL + 有序 MemTable + 不可变 SSTable 的本地 KV 存储，支持二进制键值、范围扫描和 Tombstone 覆盖逻辑；利用内存 key-offset 索引和 Bloom Filter 减少无效磁盘读取。
- 设计 SST/MANIFEST 的 `fsync + 原子 rename` 提交顺序，利用 WAL 序号水位在重启后恢复未刷盘记录；实现多路归并全量 Compaction 及旧文件回收。
- 基于 CTest 设计随机模型一致性、CRC 损坏拒绝、尾部 WAL 截断、并发读写和 5 类进程级故障注入；记录不同同步策略下的实验数据（**请添加你自己在目标环境测得的真实数据**）。

使用前务必自己克隆/解压、编译、运行完整测试、画出关键恢复流程，并逐行读懂代码。代码生成和测试通过本身不等于你独立实现了全部模块；面试应准确陈述自己的实际工作。
