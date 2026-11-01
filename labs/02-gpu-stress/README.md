# 02 CPU/GPU 接口与单卡压力测试

用 C 主机程序调用 Windows API 和 CUDA Driver API，验证 CPU 多线程、GPU 向量运算及数据传输，再对 RTX 4060 Laptop GPU 进行定时计算和显存读写测试。

- [图文操作与结果](steps.md)
- [60 项主要接口：入参、出参与返回值](reference/api-reference.md)（25 项 Win32、35 项 CUDA；参考目录中的接口并非全部在本实验执行）
- [Word 实验报告](reports/CPU与GPU调用接口及单卡压力测试.docx)
- [2026-10-20 正式七项测试](runs/2026-10-20-01/README.md)
- [2026-10-21 环境补图与重新编译记录](runs/2026-10-21-01/README.md)

## 文件怎么找

```text
02-gpu-stress/
├─ README.md
├─ steps.md
├─ src/          api_stress.c 主机程序、kernels.cu 设备核函数
├─ scripts/      构建、带监控运行、结果汇总
├─ reference/    主要接口说明（Markdown / JSON）
├─ reports/      Word 报告
└─ runs/         按实际运行日期保留日志与原图
```

构建生成的 `bin`、`build` 和临时 `results` 不入库。完成一次新运行后，将结果存入新的 `runs/批次/logs`，不要覆盖历史记录。

## 1. 环境与构建

历史测试环境为 Windows 11 x64、i9-14900HX（24 核 / 32 逻辑处理器）、RTX 4060 Laptop GPU（8188 MiB、计算能力 8.9、24 SM）、NVIDIA 驱动 560.94、CUDA Toolkit 12.6、Visual Studio 2022 x64 工具链。

需要 VS 2022 的“使用 C++ 的桌面开发”、MSVC v143、Windows SDK、NVIDIA 驱动和 CUDA Toolkit 12.6。汇总脚本另需 Python 3.8+，无第三方包依赖。

下载仓库后解压到**不含空格的路径**，例如 `E:\QC\gpu-cluster-virtualization`。本构建脚本的 nvcc 临时目录在项目内，当前已验证的构建条件是路径无空格。打开 `labs/02-gpu-stress` 文件夹，在资源管理器地址栏输入 `powershell` 后回车，后面的命令都在这个实验目录执行。

```powershell
.\scripts\build.cmd
$LASTEXITCODE
```

出现 `BUILD PASS` 且退出码为 `0` 后继续。脚本自动加载已安装的 VS 2022 x64 编译环境：`cl /TC /O2` 编译 C 主机程序，nvcc 将 CUDA C++ 核函数编译成 `sm_89` cubin，并导出 PTX、SASS。生成 `bin/api_stress.exe` 和 `build/kernels.cubin`；构建日志在 `results`。

若 Toolkit 安装位置不同，构建前设置 `$env:LAB_CUDA='实际 CUDA Toolkit 目录'`。换 GPU 型号时需核对并调整架构参数；本脚本默认 RTX 4060 Laptop GPU。

## 2. 先做短时检查

以下命令读取 GPU 0，由脚本保存 stdout、stderr、指标 CSV、监控 CSV 和 session JSON。使用一个新的批次目录；这里只检查能否正常运行，不能代替正式长测。

```powershell
$qcRun='.\runs\my-smoke-01\logs'
.\scripts\run_monitored.ps1 -Mode cpu -Label cpu -OutputDirectory $qcRun
.\scripts\run_monitored.ps1 -Mode info -Label info -OutputDirectory $qcRun
.\scripts\run_monitored.ps1 -Mode vector -Label vector -OutputDirectory $qcRun
.\scripts\run_monitored.ps1 -Mode compute -Seconds 5 -MiB 64 -Label compute5 -OutputDirectory $qcRun
.\scripts\run_monitored.ps1 -Mode memory -Seconds 5 -MiB 128 -Label memory5 -OutputDirectory $qcRun
```

输出应包含 `RESULT=PASS`、`CUDA_API_errors=0`，session 中 `exit_code=0`、`stop_reason=completed`。标签已存在时脚本会拒绝覆盖；重新运行请改批次或标签。若 Windows 提示脚本执行策略限制，可在当前 PowerShell 中执行 `Set-ExecutionPolicy -Scope Process Bypass`，再重试脚本；该设置只影响当前进程。

## 3. 完整复现实验的七项命令

插好电源，放稳电脑并保持通风。正常驱动设置下运行，不需要改功率、频率或 Windows 超时配置。下面是**复现方法**，已有正式结果仍来自 2026-10-20，并没有因整理仓库而重跑一轮。

```powershell
$qcRun='.\runs\my-full-01\logs'
.\scripts\run_monitored.ps1 -Mode cpu -Label 01_cpu -OutputDirectory $qcRun
.\scripts\run_monitored.ps1 -Mode info -Label 02_info -OutputDirectory $qcRun
.\scripts\run_monitored.ps1 -Mode vector -Label 03_vector -OutputDirectory $qcRun
.\scripts\run_monitored.ps1 -Mode transfer -MiB 64 -Label 04_transfer -OutputDirectory $qcRun
.\scripts\run_monitored.ps1 -Mode compute -Seconds 60 -MiB 64 -Label 05_warmup60 -OutputDirectory $qcRun
.\scripts\run_monitored.ps1 -Mode compute -Seconds 300 -MiB 64 -Label 06_compute300 -OutputDirectory $qcRun
.\scripts\run_monitored.ps1 -Mode memory -Seconds 120 -MiB 128 -Label 07_memory120 -OutputDirectory $qcRun
python .\scripts\summarize_results.py $qcRun
```

监控脚本默认约每秒采样一次；启动前温度达到 82℃、运行中采样达到阈值、监控失效或超过“目标时长 + 120 秒”，会拒绝启动或终止测试进程并记录原因。采样保护不是硬件温控的替代品。

手动结束 `compute` 或 `memory` 时，可另开 PowerShell，**先进入同一实验根目录**，再创建对应的停止文件，例如 `New-Item '.\runs\my-full-01\logs\06_compute300.stop' -ItemType File`。程序检测到后会输出 `STOP requested`、`RESULT=FAIL`，退出码为 1；这时 session 的 `stop_reason` 仍可能是 `completed`（监控没有强制终止）。须结合输出、退出码和实际时长判断，不能单看这个字段计为通过。其他四种模式不轮询停止文件。

汇总完整七项时输出 `summary.json` 与 `08_summary.txt`。只汇总短测或自定义标签时，加 `--discover`：

```powershell
python .\scripts\summarize_results.py '.\runs\my-smoke-01\logs' --discover
```

汇总脚本只读取已有证据，不补造缺项，默认拒绝覆盖已存在的汇总文件。不要在两个历史批次上执行覆盖操作。

## 4. 每种模式写了什么

| 模式 | 实现与校验 |
|---|---|
| `cpu` | 4 个 Win32 工作线程分块求和，等待完成后合并，和单线程参考结果比较 |
| `info` | `cuInit`、`cuDeviceGet*` 查询 GPU 名称、显存、SM 数与计算能力 |
| `vector` | 分配显存、复制输入、启动 1,048,576 元素向量加法、下载结果并逐元素比较 |
| `transfer` | 本次参数为 64 MiB 数据，分别测试可分页内存、页锁定内存 H→D / D→H，以及 D→D，12 次重复 |
| `compute` | 262,144 线程，每线程 8 条独立 FP32 FMA 链、512 次循环；4 种种子轮换，约每秒下载并检查整份输出 |
| `memory` | 本次参数为三个各 128 MiB 的数组执行 `C=A+2B`，两读一写；约每秒下载并检查整份输出 |

普通 C 运算编译成 CPU 指令，由当前线程执行；操作系统调度线程。GPU 路径是“C 主机程序 → CUDA Driver API → NVIDIA 驱动及 Windows 设备管理 → GPU”。程序使用已有驱动，没有编写内核驱动。

CUDA 主要顺序：初始化 → 设备/上下文 → 模块/核函数 → 显存 → 上传 → 发射核函数 → 同步 → 下载与校验 → 释放。可分页输入上传后显式同步，再使用非阻塞流；事件统计设备工作时间，主机性能计数器统计总耗时。

## 5. 历史实测结果（2026-10-20）

| 项目 | 原始结果 |
|---|---|
| CPU | 4 线程，4,194,304 个整数，实际和 = 参考和 = 2145386496，0 错误；线程工作与等待区间 0.878800 ms |
| 设备 | RTX 4060 Laptop GPU，8188 MiB，计算能力 8.9，24 SM |
| 向量 | 1,048,576 个元素，0 错误，核函数 0.066560 ms |
| 传输 | 可分页 H→D / D→H：9.699051 / 9.678477 GB/s；页锁定：12.026570 / 12.703436 GB/s；D→D：114.385235 GB/s |
| 计算预热 60 秒 | 核函数 11282.422815 GFLOP/s；墙钟 8705.640591 GFLOP/s；最高 59℃ |
| 持续计算 300 秒 | 核函数 11024.140677 GFLOP/s；墙钟 8410.144861 GFLOP/s；300 次周期检查，0 错误；最高 62℃ |
| 显存读写 120 秒 | 核函数 228.047448 GB/s；墙钟 182.393572 GB/s；120 次周期检查，0 错误；最高 55℃ |

七项均记录 `RESULT=PASS`，会话退出码均为 0。核函数吞吐只使用事件记录的设备工作时间；墙钟吞吐包含发射、同步、校验等开销。两种口径应同时保留，不能把核函数数据写成整机端到端速度。

FMA 按 2 次浮点运算计数，计算核每次发射 2,147,483,648 FLOP。显存核按两读一写统计流量，每次 402,653,184 字节；三个数组共 384 MiB，大于本机 32 MiB L2。D→D 传输表按有效载荷计一次，不能直接与两读一写的带宽比较。H→D / D→H 涉及主机与设备的传输链路，不等于显存带宽。

周期检查不是每次发射都下载结果；`utilization.memory` 是显存活动时间占比，`memory.used` 才是占用容量。120 秒测试只覆盖本次分配的数据区，不能称为全显存验证。本结果是单卡、特定内核和有限时长的记录，不代表官方峰值、长期稳定性认证或多卡虚拟化已经完成。

## 6. 日期、报告与证据版本

正式测试是 2026-10-20；2026-10-21 只补充了环境截图、CPU/info 复跑及重新编译日志。最新 Word 报告更新于 10 月 21 日。历史 session 中的绝对路径是当时机器目录，不需要照抄。

正式七项 session 对应的 EXE SHA256 为 `0c050a9bfbb2e1674ab07955ddf28f848d38d36259db08f2a312b801375715d5`。后来重新编译会产生新的 EXE；本仓库不上传二进制。历史 `summary.json` 中的“匹配当前可执行文件”字段是当时生成汇总时的快照，不是对读者今天构建文件的实时判断。新的运行由 session 记录新的哈希。

源码与原测试版本保持一致；构建脚本为目录调整了相对路径，补充的运行/汇总脚本支持独立批次和拒绝覆盖。发布准备阶段另做了编译、设备查询、向量及 1 秒 compute/memory 检查，全部通过；它们不替换历史长测。

## 参考

- [CUDA 12.6 Driver API](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/index.html)
- [CUDA Best Practices：计时](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-c-best-practices-guide/index.html#timing)
- [NVIDIA System Management Interface](https://docs.nvidia.com/deploy/nvidia-smi/index.html)
