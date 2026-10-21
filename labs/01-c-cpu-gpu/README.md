# 实验 01 C 调用 CPU 与 GPU

本实验观察 C 运算在 CPU 上执行，以及 C 程序通过 Windows API 申请内存、读写文件和创建线程；再通过 CUDA Driver API 完成 GPU 初始化、显存分配、数据传输、核函数启动、结果校验与资源释放。

主机端代码是 C，编译选项为 `/TC`；GPU 核函数写在 `kernels.cu` 中，由 nvcc 编译。普通 C 运算直接执行 CPU 指令，申请系统资源时调用 Windows API；GPU 部分由主机程序调用 CUDA Driver API，经 NVIDIA 驱动提交到 GPU。

## 从这里开始

- [12 步图文操作](steps.md)：按步骤查看命令、调试过程和原始截图。
- [下载原始 Word 报告](reports/C调用CPU与GPU.docx)：使用本实验提供的原报告，正文和内嵌图片保持原样。
- [查看历史运行记录](runs/2026-10-19-01/README.md)：2026-10-19 的日志、19 张报告原图及日志附录摘录。

Word 和截图中的旧电脑路径属于历史记录。现在复现时以本 README、`steps.md` 和当前项目位置为准。

## 环境

历史实验电脑为 Intel Core i9-14900HX、NVIDIA GeForce RTX 4060 Laptop GPU，Windows x64，Visual Studio 2022，CUDA Toolkit 12.6，NVIDIA 驱动 560.94。

需要安装 Visual Studio 2022 的“使用 C++ 的桌面开发”、MSVC v143 与 Windows SDK，以及 NVIDIA 驱动和 CUDA Toolkit 12.6。GPU 构建目标是 `sm_89`，对应本机 RTX 4060 Laptop GPU；更换 GPU 时需按实际架构调整构建脚本。

## 编译

在资源管理器中打开本实验文件夹 `labs/01-c-cpu-gpu`，在地址栏输入 `powershell` 并回车。确认这里直接有 `src`、`scripts`、`debug`、`reports`、`runs`，后续命令均从这里执行。

依次执行，每条构建命令结束后查看退出码：

```powershell
.\scripts\build_cpu.cmd
$LASTEXITCODE
.\scripts\build_gpu.cmd
$LASTEXITCODE
```

两个退出码均应为 `0`。脚本会自动加载已安装的 VS 2022 x64 编译环境，在本实验目录生成 `bin`、`build` 和 `results`。

如 CUDA 安装在其他位置，先把下面的路径改成实际目录再执行 GPU 构建：

```powershell
$env:LAB_CUDA = 'D:\NVIDIA\CUDA\v12.6'
.\scripts\build_gpu.cmd
```

失败时检查 `results/01_build_Od.log`、`results/05_build_c.log` 和 `results/06_nvcc_cubin.log`。CUDA 官方头文件可能出现 C4201、C4819 警告，仍需检查构建退出码和是否存在错误。

## 运行

逐条执行下表命令，每条结束后立即执行 `$LASTEXITCODE`，正常应为 `0`。

| 入口 | 命令 | 核对结果 |
|---|---|---|
| 环境信息 | `.\bin\cpu_labs_Od.exe 01` | Windows、CPU、编译器、CUDA 和 GPU 信息 |
| C 运算 | `.\bin\cpu_labs_Od.exe 02 --a 37 --b 19 --n 100000 --seed 17` | 加法 56、乘法 703、求和 50396012，检查 PASS |
| 内存和文件 | `.\bin\cpu_labs_Od.exe 03` | 申请 4096 字节、回读 PASS、释放成功 |
| 工作线程 | `.\bin\cpu_labs_Od.exe 04 --n 1000000 --seed 17` | 1、2、4 个工作线程求和均为 503993310，检查 PASS |
| GPU 向量加法 | `.\bin\gpu_labs.exe 08` | 257/257，max_error=0，PASS |
| GPU 线程映射 | `.\bin\gpu_labs.exe 09` | 有效下标恰好访问一次、越界保护 PASS |

新运行结果写入本实验根目录的 `results`。它是临时工作输出，同名文件可能被下一次运行覆盖；`runs` 中的历史材料不会被这些命令覆盖。

## Visual Studio 单步调试

**先编译，再打开 `debug` 中的方案。** 这些方案用于启动原生调试，不会替你重新编译 C/CUDA；修改源码后需要重新执行对应构建脚本。

| 方案 | 配置 | 平台 |
|---|---|---|
| `debug/Step02.sln` | `02_CPU_ASM_BREAK` | x64 |
| `debug/Step03.sln` | `03_WIN_API_BREAK` | x64 |
| `debug/Step04.sln` | `04_THREADS` | x64 |
| `debug/Step08.sln` | `08_GPU_LAUNCH_BREAK` | x64 |

按 F5 启动、F9 设置或取消断点、F10 逐过程执行、F11 进入函数。在“调试 → 窗口”查看局部变量、调用堆栈、线程、反汇编、寄存器和模块。具体操作见 [12 步图文操作](steps.md)。

Step08 的第 6～12 步在同一次调试里继续。程序安排的 `DebugBreak()` 可能显示异常 `0x80000003`，按 F5 可继续；实验 09 等 Step08 正常退出后单独运行。CPU 调试器的寄存器窗口显示主机 CPU 状态。

## 保存下一次结果

先结束程序。在 `runs` 下新建一个使用实际运行时间命名的批次，把本次 `results` 复制到它的 `logs`，本次真实截图放 `screenshots`。例如在实验根目录执行：

```powershell
$labBatch = Get-Date -Format 'yyyy-MM-dd-HHmmss-fff'
$labRunPath = Join-Path '.\runs' $labBatch
New-Item -ItemType Directory -Path (Join-Path $labRunPath 'logs') -ErrorAction Stop
Copy-Item -Path '.\results\*' -Destination (Join-Path $labRunPath 'logs') -Recurse
New-Item -ItemType Directory -Path (Join-Path $labRunPath 'screenshots')
```

只复现了部分命令，就在该批次说明中写清实际执行的项目。线程性能测试应另行采用一致计时条件；包含断点暂停的调试耗时不用于评价加速效果。

提交源码、脚本、调试工程、说明、报告和归档后的 `runs`。`bin`、`build`、`results` 和 `.vs` 由项目的 `.gitignore` 排除。
