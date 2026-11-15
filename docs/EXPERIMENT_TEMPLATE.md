# StoneKV 性能与可靠性实验记录模板

- 测试日期/时区：
- Git commit / ZIP SHA256：
- 系统/内核/文件系统及挂载参数：
- CPU / 内存：
- 磁盘型号 / 虚拟化 / 容器：
- GCC/Clang、CMake、Release 编译参数：
- 数据规模 / Key 分布 / Value 长度 / MemTable 阈值：
- 写入策略 sync_writes（0/1）：
- 预热轮数、重复轮数、样本数：
- 进程/文件系统缓存状态：
- 正确性结果、CRC 故障注入结果：
- 写入/读取吞吐（P50/P99），尾延迟（P50/P99）：
- Flush/Compaction 暂停时间与写放大：
- 对照组（同硬件同设置，例如 LevelDB/RocksDB）：
- 原始 CSV、命令、输出日志位置：
- 结论与局限：

不要对不同 fsync 策略、不同数据规模、不同预热条件、不同设备的数字直接作性能百分比比较。
