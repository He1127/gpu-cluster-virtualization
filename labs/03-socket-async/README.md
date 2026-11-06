# 实验三 Socket 一对多异步通信

一台 Windows 电脑启动 1 个协调者和 3 个 worker 进程，通过三条 TCP 连接拆分整数求和任务、返回局部结果并合并；另外测量每条链路和三链路并发的数据吞吐。主程序与结果查看器均用 C 编写，Python 只负责编排进程和保存结果。

- [按步骤操作并看原始截图](steps.md)
- [Word 实验报告与代码详解](reports/实验三Socket一对多异步通信实验报告_代码与拆分详解版.docx)
- [正式结果 JSON](runs/2026-11-05-01/summary.json) · [逐轮 CSV](runs/2026-11-05-01/measurements.csv) · [汇总日志](runs/2026-11-05-01/summary.log) · [完整运行记录](runs/2026-11-05-01/README.md)

## 1 文件与运行入口

```text
03-socket-async/
├─ README.md / steps.md
├─ 启动演示.cmd                    快速复跑入口
├─ src/
│  ├─ socket_lab.c                 C / Winsock OVERLAPPED + IOCP
│  └─ result_viewer.c              只读 Win32 日志查看器
├─ scripts/
│  ├─ build.cmd / build_viewer.cmd 编译脚本
│  ├─ run_suite.py                启动四进程、校验与测量
│  └─ start_demo.ps1              演示启动脚本
├─ reports/                       Word 报告
└─ runs/
   ├─ 2026-11-05-01/               正式汇总、截图、代表日志和全量 ZIP
   └─ demo_20261105_231521_875/   Word 图2的一键演示日志
```

构建生成的 `bin`、`build` 可以重新生成，不必上传仓库。要保留的原始结果放在 `runs`；每次复跑新建批次，不覆盖已保存结果。

## 2 先编译，再运行

本次环境为 Windows 11 x64、32 个逻辑处理器、Visual Studio 2022 x64 C 工具链，辅助编排使用 Python 3.12.14。需要 VS 的“使用 C++ 的桌面开发”与 Windows SDK；`/TC` 将两个 `.c` 文件按 C 编译。

打开本文件所在的 `03-socket-async` 文件夹，在资源管理器地址栏输入 `powershell` 后回车。以下命令都从这一层执行。

```powershell
.\scripts\build.cmd
$LASTEXITCODE
.\scripts\build_viewer.cmd
$LASTEXITCODE
```

两次构建都应显示 PASS、退出码为 `0`。脚本自动加载已安装的 VS 2022 x64 编译环境，核心程序链接 `ws2_32.lib`；查看器链接 Win32 窗口库。错误详情在 `build/compile.log` 或 `build/viewer_compile.log`。

初次查看可双击 `启动演示.cmd`，默认执行一次 quick 检查并打开真实日志。若希望直接使用命令，先确认 `python --version` 能显示已配置的 Python，再执行：

```powershell
python .\scripts\run_suite.py --out runs/my-quick-01
```

出现 `ALL PASS` 后，查看新批次的 `summary.log` 和 `summary.json`。默认 quick 做功能、分帧、尾块、断线检查，并以 32 MiB/链路做短测；它不是下面 512 MiB、三轮正式结果。

演示启动器会自动寻找已有 Python，并为本次运行建立新批次。如果自动查找失败，可用 `-PythonExe '已有解释器的完整路径'` 指定；要通过启动器执行完整模式，使用 `启动演示.cmd -Full`。

完整复现正式测量使用：

```powershell
python .\scripts\run_suite.py --out runs/my-full-01 --full
```

必须换成尚不存在的目录；脚本拒绝覆盖旧批次。脚本从可用本机端口启动四个 C 进程，将 stdout/stderr 原样保存为日志，逐个检查退出码，再生成汇总。Python 自身不承担实验中的数据传输和求和。

## 3 演示中具体调用了什么

```text
协调者创建监听 socket → bind / listen / accept
worker 创建 socket → connect → HELLO 注册身份
socket 关联 IOCP → WSASend / WSARecv 提交重叠操作
两个完成线程调用 GetQueuedCompletionStatus 处理完成包
协调者发任务分片 → worker 求局部和 → 返回 RESULT
协调者校验并合并 → QUIT / BYE → 排空待完成 I/O 后释放
```

`WSASocket(..., WSA_FLAG_OVERLAPPED)` 创建支持重叠操作的 socket；每次收发携带有效的 `OVERLAPPED`。返回 `WSA_IO_PENDING` 表示请求已成功挂起，不是提交失败。IOCP 将多个连接的完成事件交给线程池处理，日志记录真实进程 PID、完成线程 TID 和完成包数量。

每个进程有 **2 个 IOCP 完成线程**，另外还有主线程。主线程等待整项实验结束是协调动作；数据面没有用阻塞 `send/recv` 循环替代异步 I/O。建立连接的 `accept/connect` 属于控制阶段，仍可阻塞。多线程与异步 I/O 是两个概念，本程序同时使用二者。

每条连接保持独立的接收状态和串行发送队列。协议头是六个网络序 32 位字段，共 24 字节：魔数、消息类型、任务 ID、序号、载荷长度、辅助字段。接收端先累积消息头，再按长度累积消息体；不能假设一次 `WSARecv` 就得到一个完整消息。发送按实际完成字节推进，仍未发送完的部分继续提交。

## 4 拆分与合并的实测结果

输入共 **1,000,003 个 uint32 整数**，第 i 个值为 `(i*17+23)%1009`，范围为 `[0,1008]`。按下标分成三个连续区间，每个 worker 只求自己的局部和。

| worker | 起始下标 | 元素数 | 局部和 |
|---:|---:|---:|---:|
| 1 | 0 | 333,334 | 167,996,887 |
| 2 | 333,334 | 333,334 | 167,998,240 |
| 3 | 666,668 | 333,335 | 167,999,009 |

协调者逐片核对局部和，再合并得到 **503,994,136**，与单线程参考值相同。运行脚本根据日志检查区间是否连续、总数是否正确，并独立重算参考值。[协调者日志](runs/2026-11-05-01/01-task/server.log)记录分片、worker 返回和合并，同目录的三个 `worker*.log` 记录各进程实际计算。

这里的“合并”是应用层合并局部结果；吞吐部分另合计三个连接的有效载荷。它没有将三条 TCP 连接变成一个透明的虚拟连接，也没有实现多 GPU 资源合并。

## 5 正式吞吐测量

正式批次为 **2026-11-05-01**，每条活动链路每轮传输 **512 MiB**。块大小 16/64/256 KiB，分别做下行、上行；每个 worker 单独测试，再三链路同时测试，每项重复 3 轮，共 **72 组测量**。另外有 6 次三链路 32 MiB 预热，不进入正式统计。

下表是三轮的**中位数**，单位 **GB/s = 10⁹ 字节/秒**。各 worker 单测列只有指定的一条链路传载荷，其他连接保持在线；最后一列是三条链路共同时间窗口的合计有效吞吐。

| 方向 | 块大小 | worker 1 单测 | worker 2 单测 | worker 3 单测 | 三链路并发合计 |
|---|---:|---:|---:|---:|---:|
| 协调者 → worker | 16 KiB | 1.0179 | 0.9820 | 0.9382 | 1.7603 |
| 协调者 → worker | 64 KiB | 1.8672 | 1.7608 | 1.9094 | 2.6111 |
| 协调者 → worker | 256 KiB | 2.5676 | 2.5512 | 2.5271 | 2.7112 |
| worker → 协调者 | 16 KiB | 0.9492 | 0.9495 | 0.9492 | 2.1826 |
| worker → 协调者 | 64 KiB | 1.6218 | 1.6573 | 1.6305 | 3.5934 |
| worker → 协调者 | 256 KiB | 2.3811 | 2.5399 | 2.5575 | 4.1145 |

本次扫描的最大单轮三链路结果为：下行 **2.7249 GB/s**，上行 **4.2154 GB/s**，均出现在 256 KiB 块大小。它们只是本机、本程序和所选参数中的观测最大值，不能称为 socket 的通用极限。

每次均由协调者用 `QueryPerformanceCounter` 从共同开始点计时，直到接收端完成数据检查、确认流程让协调者收齐 DONE。下行由 worker 校验后返回 DONE；上行先由协调者验证收到的数据，再经 ACK→DONE 完成确认。发送函数完成只说明系统已经处理了本次发送缓冲区，不代表对端应用已收到并检查，所以不能在最后一次发送提交时停止计时。

```text
单链路有效吞吐 = 该链路载荷字节 /（共同开始至该链路 DONE）
三链路合计吞吐 = 三条链路载荷总字节 /（共同开始至最后一个 DONE）
```

三链路列不是把各条链路单独测试的 GB/s 相加。计时包含模式数据生成、复制、协议、CPU 校验、调度和应用确认；不含连接建立。它衡量应用有效载荷，不计 24 字节协议头等额外字节。所有数据按位置生成并逐个 32 位数据词检查，`pattern_errors=0` 不是仅核对发送字节数。

72 组正式测量总共处理 54 GiB 有效载荷，载荷在内存中生成和验证，不会把这些数据全部写成磁盘文件。完整运行比 quick 增加 CPU 与内存活动；计时会受后台负载、调度和频率变化影响，因此保留每轮值、最小值、中位数和最大值。

## 6 分帧、尾块和断线检查

| 检查 | 做法 | 实际验收 |
|---|---|---|
| 7 字节 I/O 分段 | N=1003，每次只提交最多 7 字节的收发缓冲 | 四进程退出 0，任务合并正确 |
| 非整块尾部 | 每链路 1,048,580 字节、64 KiB 块，下行与上行都测 | 最后 4 字节仍正确处理，模式检查通过 |
| worker 断线 | 让 worker 延迟处理，再终止 worker 1 | 协调者检测失败并非 0 退出，测试验收 PASS |

7 字节检查说明应用消息可以跨多次 I/O 完成，不代表强制操作系统产生了一次 `WSASend` 的短完成。断线用例中的服务端 `RESULT=FAIL` 是预期现象；外层 `session.json` 的 `check=PASS` 表示成功验证错误检测，不能把它改成程序正常传输成功。

不要只搜索日志里的 PASS。普通用例还要四个进程退出码都为 0；`run_suite.py` 已执行这项检查。脚本设有外层等待期限，并在结束时回收本轮创建的残留进程；不会终止其他不相关程序。

## 7 用普通窗口查看原始结果

查看器只读打开真实日志，约每 500 ms 刷新；窗口中的文字来自文件，不会重新生成成功结果。

```powershell
.\bin\result_viewer.exe --dir .\runs\2026-11-05-01\01-task
.\bin\result_viewer.exe --file .\runs\2026-11-05-01\summary.log
```

`--dir` 以四个窗口区域显示协调者和三个 worker 的日志；`--file` 显示一个实际文件。原始截图在本批次 `screenshots`，配套解释见 [steps.md](steps.md)。

## 8 结论范围与后续扩展

本次使用 **127.0.0.1 回环、同一台电脑上的四个进程**，数据未经过物理网卡。程序目前仅接受该地址，不能只把命令中的 IP 换成其他电脑就当成跨机测试。跨机扩展需要先修改地址约束并重新确认监听地址、网络连通性与部署环境，再测真实网络。

本次没有同步通信对照组，所以不能证明 IOCP 在任何场景都更快；也没有证明多进程求和比单进程快。72 组结果只覆盖两种方向、三档块大小和三次重复，不能由最大值推导长期稳定带宽。TLS、认证、自动重试、任务重分配和 GPU 计算不在本实验范围。

## Microsoft 原始资料

- [IOCP 的线程与完成队列](https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports)
- [WSASend 参数、完成语义与缓冲区生命周期](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsasend)
- [WSARecv、部分接收与正常关闭](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsarecv)
- [GetQueuedCompletionStatus](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getqueuedcompletionstatus)
- [QueryPerformanceCounter 计时](https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps)
