# Linux 验证记录 2026-11-15

基线 `2318bb65bc0d13b562dc1cee755355e6690c142f` 的干净源码由 git archive 导出并在独立 `/tmp/stonekv_original_2318bb6_build_20261115` Release 构建。`baseline_original_rebuilt.txt` 保留 configure/build/CTest 与内部测试的命令及退出码。`baseline_original_tests.txt` 是初次尝试从不存在的构建路径执行失败，保留失败记录，不代表基线结果。

第一项 P0 修改 `13b26c3`：已有数据库丢失 WAL 拒绝打开，新数据库先创建并 fsync WAL 和其目录再提交 MANIFEST；MANIFEST 校验和后拒绝额外数据。两个新增回归均在修复前失败，`red_manifest.txt` 为其中 MANIFEST 的红灯记录；修复后测试保留所有原始 28 项。
