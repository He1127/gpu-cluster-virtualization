# 2026-10-21-01：补图、CPU/info 复跑与重编译

本批次没有重跑 300 秒计算和 120 秒显存读写测试。长测结果仍在 [2026-10-20-01](../2026-10-20-01/README.md)。

- `screenshots/01-environment-query.png`：10 月 21 日 15:08:51 的真实 PowerShell 环境查询图，直接取自最新 Word 报告。
- `logs/reproduce_cpu*`、`reproduce_info*`：10 月 21 日的 CPU 与 GPU 属性复跑记录。
- `build-logs`：当天重新构建日志、nvcc 版本与 SASS 导出；不是 10 月 20 日冻结构建日志。

原始 `reproduce_*_stdout.txt` 保留了 Windows PowerShell 输出的 UTF-16LE 编码；阅读时用支持 BOM 的编辑器，不要当作 UTF-8 强行解码。其余原始字节未因整理而转换。

重新构建的程序与正式长测 EXE 哈希不同，不能把当天的编译日志与旧 session 写成同一次运行。历史路径仅用于记录，新的运行入口见[实验说明](../../README.md)。
