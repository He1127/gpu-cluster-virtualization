# 2026-10-20-01：正式七项测试

此批次保存 CPU、GPU 信息、向量、传输、60 秒预热、300 秒计算、120 秒显存读写的历史证据。`logs` 中七个前缀各有 session、stdout、stderr、metrics、telemetry 和 telemetry_stderr；另含环境与汇总。

| 前缀 | 对应测试 |
|---|---|
| `01_cpu` | 四线程整数求和 |
| `02_info` | GPU 属性 |
| `03_vector` | 1,048,576 元素向量加法 |
| `04_transfer` | 64 MiB × 12 次传输 |
| `05_warmup60` | 60 秒 FP32 计算预热 |
| `06_compute300` | 300 秒 FP32 计算 |
| `07_memory120` | 三个 128 MiB 数组、120 秒读写 |

七项 session 的退出码均为 0、结束原因为 completed；stdout 均有 RESULT=PASS。空的 stderr 是保留的原始文件，不是缺失材料。metrics CSV 是程序输出；telemetry CSV 是约每秒采样的 nvidia-smi 数据。测量口径见[实验说明](../../README.md)。

历史可执行文件 SHA256：`0c050a9bfbb2e1674ab07955ddf28f848d38d36259db08f2a312b801375715d5`。`frozen_build_manifest.json` 是原目录结构的构建快照，内含的 `src/build/bin` 路径和原脚本哈希不应被当成整理后仓库的路径。`summary.json` 中与“current executable”相关的字段是 10 月 20 日的历史判断；重新编译产生新哈希是正常现象。

`screenshots` 保存报告中的原始运行截图；CPU 图的发布校正说明见实验根目录中的 `reports/发布说明.md`。图片中的绝对路径属于历史环境，复现时请使用仓库说明中的相对命令。查看完整结果优先读日志，截图仅展示部分内容。
