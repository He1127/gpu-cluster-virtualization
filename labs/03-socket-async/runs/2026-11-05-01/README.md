# 实验三正式运行记录

正式批次为 2026 年 11 月 5 日。83 个会话包括 72 组测量、6 组预热和 5 组功能或故障检查。数据来自 Windows 本机 `127.0.0.1` 回环测试。

- [逐轮吞吐 CSV](measurements.csv)
- [汇总 JSON](summary.json)和[终端汇总](summary.log)
- [运行环境与哈希](environment.json)
- [七张正式批次截图](screenshots/)
- [完整批次 ZIP](formal-run-complete.zip)

ZIP 保留整个原始 `2026-11-05-01` 目录。点击 ZIP 文件页的下载按钮，解压到一个单独的文件夹，再进入解压出的 `2026-11-05-01` 查看各用例。每个用例包含协调者日志、三份 worker 日志以及记录命令、PID、退出码和 EXE 哈希的 `session.json`。日志中的绝对路径是当时的运行环境。

## 可直接打开的会话

- [01-task](01-task/session.json)
- [02-fragmented](02-fragmented/session.json)
- [03-tail-down](03-tail-down/session.json)
- [03-tail-up](03-tail-up/session.json)
- [04-disconnect](04-disconnect/session.json)
- [bench-down-c262144-a1-r1](bench-down-c262144-a1-r1/session.json)
- [bench-down-c262144-a0-r1](bench-down-c262144-a0-r1/session.json)
- [bench-up-c262144-a1-r1](bench-up-c262144-a1-r1/session.json)
- [bench-up-c262144-a0-r1](bench-up-c262144-a0-r1/session.json)

`04-disconnect` 的协调者应以失败退出。这个用例的 `check=PASS` 表示成功检测断线。其他正常用例的四个进程都应退出 0。

## Word 图2的演示记录

Word 图2来自同日的一键演示，进程号和端口见[演示会话](../demo_20261105_231521_875/01-task/session.json)和[协调者日志](../demo_20261105_231521_875/01-task/server.log)。正式测量统计使用上方的正式批次。
