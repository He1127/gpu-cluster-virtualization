# CPU/GPU 主要调用函数与参数参考

核对日期：2026-10-20。以本机 Windows x64 与 CUDA 12.6 为范围。

1. 范围：本机 Windows x64 + C 主机程序 + NVIDIA CUDA Driver API 12.6；60 项主要接口覆盖线程、同步、主机内存、计时、设备、上下文、模块、显存、传输、核函数、流、事件、错误。不是穷尽所有操作系统、厂商或 CUDA 接口。图形渲染、视频编解码、完整图执行、IPC、多 GPU/分布式、NVML 监控接口属于后续专题。
2. 这是一份接口参考，不等于60项均在本次执行。报告中的本次实测范围应另按源码和日志标注。
3. in/out 描述调用接口参数如何传递；例如 dst 指针本身以值传入，指向的数据区被写入。返回状态与通过 out 指针取得的数据/句柄是不同通道。
4. 本文件原型采用便于阅读的参数名；导出符号可能因 cuda.h 的版本宏映射为 *_v2 等，编译以安装的头文件和库为准。
5. 标准 C 的加减乘除通常被编译为 CPU 指令在用户态执行，不需要每次先调用操作系统。线程创建、阻塞、内存管理等由 Win32 接口请求系统服务；Windows 内核调度线程。
6. GPU kernel（核函数）不是 Windows kernel（操作系统内核）。主机 C → CUDA Driver API → NVIDIA 用户态/内核态驱动与 Windows 图形调度设施 → GPU 执行；每个 cu* 调用不一定都立即进入内核或提交独立硬件任务。
7. CPU C 源码由 MSVC 编译为 x64 机器码；CUDA .cu 核函数由 nvcc 编译成 PTX/设备二进制 cubin。PTX 是虚拟指令集，SASS 是特定 GPU 的机器指令反汇编表示；PTX 也可由驱动 JIT 编译。不能把 CPU 指令直接接到 GPU 指令上执行。
8. 主机用 /TC 作为 C 编译；.cu 文件仍使用 CUDA C++ 工具链，可用 extern "C" 导出稳定核函数名。这不代表整个程序只有标准 C 编译器就能完成构建。
9. cuLaunchKernel 返回 CUDA_SUCCESS 仅说明本次提交调用成功；之后的同步/复制仍可能报告异步执行错误。所有返回状态都需检查，最后再校验输出数组。
10. Async 不是永不阻塞的承诺，没有 Async 也不保证等待 GPU 完成。尤其 cuMemcpyDtoD 不做主机侧同步；需要使用事件/流同步界定完成。
11. 同一流中按提交顺序执行。不同流是否重叠受依赖、设备资源、锁页传输和硬件支持影响；创建两个流不等于获得两块物理 GPU。
12. 释放顺序：完成/校验 → 事件 → 流 → 设备/锁页主机内存 → 模块 → 上下文；即使释放接口有隐式行为，也不能省掉对运行结果的等待和检查。

## 调用链与报告解释

- C 主线程把数组分片 → CreateThread 把入口地址与分片参数交给 Windows → Windows 让工作线程在允许的逻辑处理器上执行 → 每个线程用普通 CPU 指令累加自己的分片 → 主线程 WaitForMultipleObjects → 汇总各分片并检查参考值 → CloseHandle。
- 主机 C 调用 cuInit、选设备、建立上下文 → 加载 nvcc 生成的 cubin → 分配设备数组并上传输入 → 取 CUfunction → cuLaunchKernel 提交线程网格和参数 → GPU 每个线程处理自己的元素/矩阵块 → 事件或流同步并检查错误 → 读回数组与 CPU 参考比较 → 成对释放。
- CUdeviceptr a,b,c; unsigned int n; void *args[]={&a,&b,&c,&n}; args[0] 是主机变量 a 的地址；a 的数值才是设备地址。驱动按核函数声明复制参数值，GPU 再用设备地址访问显存。
- 操作步骤告诉读者如何重现；源代码片段说明写了什么；调用顺序说明如何发出工作；PASS 需要退出码、API 状态和数值校验支持；截图只是这些原始结果的可见记录。
- GFLOP/s=实际定义的浮点操作数/秒/10^9；FMA 计2次操作。GB/s=约定读写字节数/秒/10^9。核函数事件时间、主机端到端时间和传输时间分别报告。简单示例内核测得的吞吐不能称为显卡峰值算力。
- 压力测试需要固定问题规模、持续时间、数据类型、种子、预热/重复方式和停止条件；同时记录温度、功耗、利用率等可支持项。采样项不支持应写 N/A，不能当作0。无错误完成一段测试只说明该条件和时长内未发现错误。

## 60 项接口目录


### Win32 / CPU：CPU 信息

**1. GetSystemInfo** — 查询页大小、体系结构等系统基本信息。

`void GetSystemInfo(LPSYSTEM_INFO info);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| info | out | 调用者提供 SYSTEM_INFO，函数写入系统信息。 | 无 |

返回：无返回值。

同步：查询操作；不等待工作线程。

资源：结构体由调用者管理。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getsysteminfo)。本项是接口参考，实测状态另查运行日志。

**2. GetActiveProcessorCount** — 统计某处理器组或全系统活动逻辑处理器。

`DWORD GetActiveProcessorCount(WORD group);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| group | in | 组号；ALL_PROCESSOR_GROUPS 表示所有组。 | 无 |

返回：返回处理器数量；0 表示失败，可读 GetLastError。

同步：查询操作。

资源：无资源释放。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getactiveprocessorcount)。本项是接口参考，实测状态另查运行日志。


### Win32 / CPU：线程

**3. CreateThread** — 创建执行指定 C 函数的 Windows 线程。

`HANDLE CreateThread(LPSECURITY_ATTRIBUTES attrs, SIZE_T stackBytes, LPTHREAD_START_ROUTINE entry, LPVOID arg, DWORD flags, LPDWORD threadId);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| attrs | in，可空 | 安全/继承属性，NULL 使用默认值。 | 无 |
| stackBytes | in | 0 使用可执行文件默认栈设置。 | 字节 |
| entry | in | 线程入口：DWORD WINAPI worker(LPVOID)。 | 无 |
| arg | in，可空 | 传给入口的指针；所指数据在线程使用期间必须有效。 | 无 |
| flags | in | 0 立即就绪；CREATE_SUSPENDED 需再恢复。 | 无 |
| threadId | out，可空 | 线程编号；它不是线程句柄。 | 无 |

返回：返回新线程 HANDLE；NULL 失败并读 GetLastError。入口函数的 DWORD 返回值另用 GetExitCodeThread 获取。

同步：创建成功不等于线程计算完成；新线程甚至可能在 CreateThread 返回前执行。

资源：实句柄最终 CloseHandle；参数/结果缓冲区在线程结束前保留。线程内使用 CRT 时优先选 _beginthreadex。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createthread)。本项是接口参考，实测状态另查运行日志。


### Win32 / CPU：等待

**4. WaitForSingleObject** — 等待线程、事件等一个对象。

`DWORD WaitForSingleObject(HANDLE object, DWORD timeoutMs);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| object | in | 需有 SYNCHRONIZE 权限的对象句柄。 | 无 |
| timeoutMs | in | 0 只检查；INFINITE 无限等待。 | 毫秒 |

返回：WAIT_OBJECT_0：满足；WAIT_TIMEOUT：超时；WAIT_FAILED：错误并读 GetLastError；互斥对象还可能 WAIT_ABANDONED。

同步：不满足时阻塞调用线程；等待线程句柄成功才表示该线程已退出。

资源：等待未结束前不可关闭正在等待的句柄。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject)。本项是接口参考，实测状态另查运行日志。

**5. WaitForMultipleObjects** — 等待多个工作线程或事件。

`DWORD WaitForMultipleObjects(DWORD count, const HANDLE *objects, BOOL all, DWORD timeoutMs);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| count | in | 1 至 MAXIMUM_WAIT_OBJECTS；不能为 0。 | 个 |
| objects | in | 不含重复句柄的数组。 | 无 |
| all | in | TRUE 全部满足；FALSE 任一满足。 | 无 |
| timeoutMs | in | 超时长度或 INFINITE。 | 毫秒 |

返回：成功返回 WAIT_OBJECT_0 至 WAIT_OBJECT_0+count-1；任一等待时差值为数组下标；也可能超时、WAIT_FAILED 或废弃互斥状态。

同步：不满足时阻塞；all=TRUE 适合汇总多个计算线程。

资源：数组与句柄在等待结束前有效，结束后逐个 CloseHandle。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects)。本项是接口参考，实测状态另查运行日志。


### Win32 / CPU：线程

**6. GetExitCodeThread** — 读取线程退出码。

`BOOL GetExitCodeThread(HANDLE thread, LPDWORD exitCode);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| thread | in | 有查询权限的线程句柄。 | 无 |
| exitCode | out | 线程的 DWORD 退出码；未退出为 STILL_ACTIVE。 | 无 |

返回：BOOL 非零为查询成功；0 失败。查询成功与工作计算正确是两件事。

同步：不等待线程；通常先成功等待线程再查询。

资源：先查询再 CloseHandle；不要把工作退出码设为 STILL_ACTIVE(259)。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getexitcodethread)。本项是接口参考，实测状态另查运行日志。


### Win32 / CPU：资源回收

**7. CloseHandle** — 关闭拥有的 Win32 对象句柄。

`BOOL CloseHandle(HANDLE handle);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| handle | in | CreateThread/CreateEventW 等返回的实句柄。 | 无 |

返回：非零成功；0 失败并读 GetLastError。

同步：关闭线程句柄不会停止线程或等待线程结束。

资源：每个拥有的实句柄关闭一次；不要关闭 GetCurrentThread 的伪句柄。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-closehandle)。本项是接口参考，实测状态另查运行日志。


### Win32 / CPU：线程

**8. GetCurrentThread** — 取得表示调用线程的伪句柄。

`HANDLE GetCurrentThread(void);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| 无 | — | 无入参/出参 | — |

返回：返回当前调用线程的伪句柄。

同步：立即返回；交给另一个线程使用时将代表另一个线程自身。

资源：伪句柄不需要 CloseHandle。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getcurrentthread)。本项是接口参考，实测状态另查运行日志。


### Win32 / CPU：调度约束

**9. SetThreadAffinityMask** — 限制线程可运行的逻辑处理器集合。

`DWORD_PTR SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| thread | in | 待设置的线程句柄。 | 无 |
| mask | in | 每个置 1 位代表一个允许的逻辑处理器；必须在进程允许集合内。 | 位掩码 |

返回：成功返回旧掩码；0 失败并读 GetLastError。

同步：影响操作系统后续线程调度；不能直接命令某条指令立刻运行。

资源：记录旧掩码便于恢复；超过 64 逻辑处理器需考虑处理器组。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setthreadaffinitymask)。本项是接口参考，实测状态另查运行日志。

**10. SetThreadPriority** — 设置线程相对进程优先级。

`BOOL SetThreadPriority(HANDLE thread, int priority);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| thread | in | 有设置权限的线程句柄。 | 无 |
| priority | in | 如 THREAD_PRIORITY_NORMAL 或 BELOW_NORMAL。 | 无 |

返回：非零成功；0 失败并读 GetLastError。

同步：优先级影响 CPU 时间分配，不能保证独占 CPU 或固定性能。

资源：无独立资源；实验使用普通优先级即可。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadpriority)。本项是接口参考，实测状态另查运行日志。


### Win32 / CPU：让出 CPU

**11. Sleep** — 暂停当前线程一段时间。

`void Sleep(DWORD timeoutMs);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| timeoutMs | in | 0 表示让出剩余时间片的机会；非零请求等待。 | 毫秒 |

返回：无返回值。

同步：睡眠结束后仅重新具备被调度资格，并非精确计时器。

资源：无资源释放；不能代替等待计算完成。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleep)。本项是接口参考，实测状态另查运行日志。

**12. SwitchToThread** — 尝试将执行机会让给其他就绪线程。

`BOOL SwitchToThread(void);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| 无 | — | 无入参/出参 | — |

返回：非零表示发生切换；0 表示未让给其他线程。

同步：只尝试让出；不是等待特定任务的同步原语。

资源：无资源释放。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-switchtothread)。本项是接口参考，实测状态另查运行日志。


### Win32 / CPU：事件同步

**13. CreateEventA** — 创建或打开事件对象，控制线程何时继续。

`HANDLE CreateEventA(LPSECURITY_ATTRIBUTES attrs, BOOL manualReset, BOOL initiallySet, LPCSTR name);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| attrs | in，可空 | 安全/继承属性。 | 无 |
| manualReset | in | TRUE 手动复位；FALSE 自动复位。 | 无 |
| initiallySet | in | TRUE 初始有信号；FALSE 初始无信号。 | 无 |
| name | in，可空 | ANSI 名称；本实验传 NULL 创建匿名事件。CreateEventW 对应 UTF-16 名称。 | 无 |

返回：返回事件 HANDLE；NULL 失败。打开已存在同名事件时可读到 ERROR_ALREADY_EXISTS。

同步：创建本身不等待；等待方调用 WaitForSingleObject 等。

资源：最终 CloseHandle。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createeventa)。本项是接口参考，实测状态另查运行日志。

**14. SetEvent** — 将事件设为有信号，允许等待线程继续。

`BOOL SetEvent(HANDLE event);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| event | in | 具有 EVENT_MODIFY_STATE 权限的事件句柄。 | 无 |

返回：非零成功；0 失败并读 GetLastError。

同步：唤醒不保证立即获得 CPU。自动复位通常放行一个等待线程；手动复位可放行多个。

资源：用已创建事件，最后 CloseHandle。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setevent)。本项是接口参考，实测状态另查运行日志。

**15. ResetEvent** — 把手动复位事件恢复为无信号。

`BOOL ResetEvent(HANDLE event);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| event | in | 事件句柄。 | 无 |

返回：非零成功；0 失败并读 GetLastError。

同步：后续等待需要下一次 SetEvent。

资源：不创建新资源。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-resetevent)。本项是接口参考，实测状态另查运行日志。


### Win32 / CPU：临界区

**16. InitializeCriticalSection** — 初始化进程内互斥保护结构。

`void InitializeCriticalSection(LPCRITICAL_SECTION cs);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| cs | out | 调用者持有的 CRITICAL_SECTION 存储。 | 无 |

返回：无返回值；不可按 BOOL 检查。

同步：初始化必须先于所有线程使用。

资源：不用后 DeleteCriticalSection；初始化前不可进入。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-initializecriticalsection)。本项是接口参考，实测状态另查运行日志。

**17. EnterCriticalSection** — 进入受互斥保护的代码段。

`void EnterCriticalSection(LPCRITICAL_SECTION cs);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| cs | in/out | 已经初始化的临界区对象。 | 无 |

返回：无返回值。

同步：若被其他线程持有，调用线程等待；同一线程可重入。

资源：每次成功进入对应一次 LeaveCriticalSection。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-entercriticalsection)。本项是接口参考，实测状态另查运行日志。

**18. LeaveCriticalSection** — 离开受保护代码段。

`void LeaveCriticalSection(LPCRITICAL_SECTION cs);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| cs | in/out | 必须由当前线程持有的临界区对象。 | 无 |

返回：无返回值。

同步：释放对应的一次持有，其他等待线程才有机会进入。

资源：不能替其他线程释放。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-leavecriticalsection)。本项是接口参考，实测状态另查运行日志。

**19. DeleteCriticalSection** — 回收临界区内部资源。

`void DeleteCriticalSection(LPCRITICAL_SECTION cs);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| cs | in/out | 已经初始化且无人使用的临界区。 | 无 |

返回：无返回值。

同步：不等待仍在使用它的线程；先结束全部使用者。

资源：删除后不得继续使用，除非重新初始化。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-deletecriticalsection)。本项是接口参考，实测状态另查运行日志。


### Win32 / CPU：原子操作

**20. InterlockedIncrement** — 原子地把共享 LONG 加 1。

`LONG InterlockedIncrement(LONG volatile *value);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| value | in/out | 按 4 字节对齐的 32 位共享 LONG。 | 整数 |

返回：返回增加后的值；该返回值不是错误码。

同步：读改写不可被并发线程拆开；Windows 此接口提供完整内存屏障。

资源：由调用者持有变量；volatile 本身不能代替原子操作。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-interlockedincrement)。本项是接口参考，实测状态另查运行日志。


### Win32 / CPU：主机内存

**21. VirtualAlloc** — 在进程虚拟地址空间保留/提交可读写内存。

`LPVOID VirtualAlloc(LPVOID address, SIZE_T bytes, DWORD allocType, DWORD protection);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| address | in，可空 | NULL 让系统选择基址。 | 无 |
| bytes | in | 请求大小，系统按页处理。 | 字节 |
| allocType | in | 常用 MEM_RESERVE 或 MEM_COMMIT。 | 无 |
| protection | in | 本实验用 PAGE_READWRITE。 | 无 |

返回：成功返回内存基址；NULL 失败并读 GetLastError。

同步：创建主机虚拟内存，不是 CUDA 显存；不证明页已全部实际驻留。

资源：最终 VirtualFree(base, 0, MEM_RELEASE)。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc)。本项是接口参考，实测状态另查运行日志。

**22. VirtualFree** — 释放 VirtualAlloc 分配。

`BOOL VirtualFree(LPVOID address, SIZE_T bytes, DWORD freeType);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| address | in | MEM_RELEASE 必须传原始分配基址。 | 无 |
| bytes | in | MEM_RELEASE 时必须为 0。 | 字节 |
| freeType | in | MEM_RELEASE 释放整块；MEM_DECOMMIT 仅取消提交。 | 无 |

返回：非零成功；0 失败并读 GetLastError。

同步：释放前确保所有工作线程停止访问。

资源：释放后指针无效；不能用来释放 cuMemAlloc 内存。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree)。本项是接口参考，实测状态另查运行日志。


### Win32 / CPU：主机计时

**23. QueryPerformanceCounter** — 读高分辨率计数器。

`BOOL QueryPerformanceCounter(LARGE_INTEGER *counter);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| counter | out | 64 位计数值。 | tick |

返回：非零成功；计数值写入 counter，不是 BOOL 返回值。

同步：不等待 GPU；仅测主机提交时间会遗漏异步 GPU 执行。

资源：秒数=(结束tick−开始tick)/频率。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancecounter)。本项是接口参考，实测状态另查运行日志。

**24. QueryPerformanceFrequency** — 读性能计数器频率。

`BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| frequency | out | 计数器每秒 tick 数；不等于 CPU 时钟。 | tick/s |

返回：非零成功；频率通过 out 参数写入。

同步：启动时查询一次即可用于时间换算。

资源：与 QueryPerformanceCounter 配套。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancefrequency)。本项是接口参考，实测状态另查运行日志。


### Win32 / CPU：错误处理

**25. GetLastError** — 读取当前线程记录的 Win32 错误码。

`DWORD GetLastError(void);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| 无 | — | 无入参/出参 | — |

返回：返回错误编号；不是所有接口都会更新它。

同步：按对应接口文档，在检测失败后立即读取，避免被后续调用覆盖。

资源：只适用于约定使用它的 Win32 接口；CUDA 错误使用 CUresult。

[官方接口说明](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-getlasterror)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：驱动初始化

**26. cuInit** — 初始化 CUDA Driver API。

`CUresult cuInit(unsigned int flags);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| flags | in | 必须为 0。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：不作为 GPU 完成屏障。

资源：无独立资源需要释放。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__INITIALIZE.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：版本

**27. cuDriverGetVersion** — 读取驱动支持的 CUDA API 版本。

`CUresult cuDriverGetVersion(int *version);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| version | out | 整数版本编码，如 12060 表示 12.6；不等于 nvidia-smi 驱动版本号。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：不作为 GPU 完成屏障。

资源：无独立资源需要释放。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__VERSION.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：设备

**28. cuDeviceGetCount** — 枚举可见 CUDA GPU 数量。

`CUresult cuDeviceGetCount(int *count);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| count | out | 可见设备数。 | 个 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：不作为 GPU 完成屏障。

资源：无独立资源需要释放。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__DEVICE.html)。本项是接口参考，实测状态另查运行日志。

**29. cuDeviceGet** — 按序号取得 CUDA 设备标识。

`CUresult cuDeviceGet(CUdevice *device, int ordinal);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| device | out | 写回设备标识。 | 无 |
| ordinal | in | 0 至 count-1 的设备序号。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：不作为 GPU 完成屏障。

资源：无独立资源需要释放。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__DEVICE.html)。本项是接口参考，实测状态另查运行日志。

**30. cuDeviceGetName** — 取得 GPU 型号名称。

`CUresult cuDeviceGetName(char *name, int capacity, CUdevice device);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| name | out | 调用者提供的字符缓冲区。 | 无 |
| capacity | in | 缓冲区容量。 | 字节 |
| device | in | 设备标识。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：不作为 GPU 完成屏障。

资源：无独立资源需要释放。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__DEVICE.html)。本项是接口参考，实测状态另查运行日志。

**31. cuDeviceTotalMem** — 查询设备全局内存总量。

`CUresult cuDeviceTotalMem(size_t *bytes, CUdevice device);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| bytes | out | 总显存大小；不是当前空闲显存。 | 字节 |
| device | in | 设备标识。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：不作为 GPU 完成屏障。

资源：无独立资源需要释放。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__DEVICE.html)。本项是接口参考，实测状态另查运行日志。

**32. cuDeviceGetAttribute** — 查询某项硬件能力。

`CUresult cuDeviceGetAttribute(int *value, CUdevice_attribute attribute, CUdevice device);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| value | out | 属性值，单位随属性变化。 | 无 |
| attribute | in | 如 SM 数、warp 大小、最大线程数、计算能力等枚举。 | 无 |
| device | in | 设备标识。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：不作为 GPU 完成屏障。

资源：无独立资源需要释放。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__DEVICE.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：上下文

**33. cuCtxCreate** — 为所选设备创建并设为本线程当前 CUDA 上下文。

`CUresult cuCtxCreate(CUcontext *context, unsigned int flags, CUdevice device);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| context | out | CUDA 上下文句柄。 | 无 |
| flags | in | 本实验用 0；可配置 CPU 等待 GPU 的调度策略。 | 无 |
| device | in | 选定设备标识。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：不作为 GPU 完成屏障。

资源：自行创建的上下文最终 cuCtxDestroy；不要与 Runtime 的主上下文任意混用。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__CTX.html)。本项是接口参考，实测状态另查运行日志。

**34. cuCtxSetCurrent** — 设置调用 CPU 线程当前使用的上下文。

`CUresult cuCtxSetCurrent(CUcontext context);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| context | in，可空 | NULL 解除当前绑定；非空绑定指定上下文。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：不保证已有 GPU 工作完成；不同 CPU 线程需分别维护当前上下文。

资源：无独立资源需要释放。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__CTX.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：GPU 等待

**35. cuCtxSynchronize** — 等待当前上下文中先前提交的 GPU 工作。

`CUresult cuCtxSynchronize(void);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| 无 | — | 无入参/出参 | — |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：阻塞调用 CPU 线程，等待上下文此前工作结束；还可能报告异步执行错误。

资源：无独立资源需要释放。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__CTX.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：上下文回收

**36. cuCtxDestroy** — 销毁自己创建的上下文及其关联资源。

`CUresult cuCtxDestroy(CUcontext context);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| context | in | 待销毁上下文。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：正常清理先显式等待工作结束，不能把销毁函数当作正确性验证。

资源：销毁后上下文及其关联设备资源不可再用；本代码按逆序显式释放。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__CTX.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：GPU 程序

**37. cuModuleLoad** — 加载 PTX、cubin 或 fatbin 文件。

`CUresult cuModuleLoad(CUmodule *module, const char *path);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| module | out | 模块句柄。 | 无 |
| path | in | 本机文件路径，指向已编译 GPU 程序。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：不作为 GPU 完成屏障。

资源：最终 cuModuleUnload；加载 PTX 时驱动可能 JIT 生成设备机器代码。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__MODULE.html)。本项是接口参考，实测状态另查运行日志。

**38. cuModuleGetFunction** — 根据核函数名称取可启动的函数句柄。

`CUresult cuModuleGetFunction(CUfunction *function, CUmodule module, const char *name);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| function | out | 核函数句柄。 | 无 |
| module | in | 已加载模块。 | 无 |
| name | in | 模块中精确的入口名称；extern "C" 可避免 C++ 名称修饰。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：不作为 GPU 完成屏障。

资源：不单独释放 CUfunction；模块卸载后句柄失效。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__MODULE.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：GPU 程序回收

**39. cuModuleUnload** — 卸载已经加载的 GPU 模块。

`CUresult cuModuleUnload(CUmodule module);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| module | in | 已加载模块句柄。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：先等待引用该模块的 GPU 工作结束。

资源：卸载一次；相关 CUfunction 不再使用。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__MODULE.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：GPU 内存

**40. cuMemGetInfo** — 查询当前上下文的可用与总设备内存。

`CUresult cuMemGetInfo(size_t *freeBytes, size_t *totalBytes);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| freeBytes | out | 当前可用内存，受其他进程/系统占用影响。 | 字节 |
| totalBytes | out | 可报告的总内存。 | 字节 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：不作为 GPU 完成屏障。

资源：无独立资源需要释放。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__MEM.html)。本项是接口参考，实测状态另查运行日志。

**41. cuMemAlloc** — 为 GPU 数组分配设备内存。

`CUresult cuMemAlloc(CUdeviceptr *pointer, size_t bytes);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| pointer | out | 写回设备地址；不是可在 CPU 上直接解引用的 float*。 | 无 |
| bytes | in | 元素个数×sizeof(元素类型)。 | 字节 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：不作为 GPU 完成屏障。

资源：最终 cuMemFree；未初始化数据需另传输或写入。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__MEM.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：GPU 内存回收

**42. cuMemFree** — 释放设备内存。

`CUresult cuMemFree(CUdeviceptr pointer);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| pointer | in | cuMemAlloc 等相配分配的设备地址。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：先显式等待使用该地址的工作完成。

资源：释放后不可继续传给复制或核函数。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__MEM.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：锁页主机内存

**43. cuMemHostAlloc** — 分配适合主机与 GPU 传输的锁页内存。

`CUresult cuMemHostAlloc(void **pointer, size_t bytes, unsigned int flags);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| pointer | out | CPU 可读写的主机地址。 | 无 |
| bytes | in | 分配大小。 | 字节 |
| flags | in | 0 使用默认锁页分配；可选可移植/映射/写合并等标志。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：不作为 GPU 完成屏障。

资源：最终 cuMemFreeHost；它不是独立显存分配，不宜无上限占用锁页内存。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__MEM.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：主机内存回收

**44. cuMemFreeHost** — 释放 CUDA 分配的锁页主机内存。

`CUresult cuMemFreeHost(void *pointer);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| pointer | in | cuMemHostAlloc 等返回的主机地址。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：先等待读写该缓冲区的异步传输结束。

资源：不能用 free 或 VirtualFree 代替。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__MEM.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：CPU→GPU 传输

**45. cuMemcpyHtoD** — 把主机数组上传到显存。

`CUresult cuMemcpyHtoD(CUdeviceptr dst, const void *src, size_t bytes);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| dst | in，指向输出区 | 已分配的设备目标地址。 | 无 |
| src | in | 主机源地址。 | 无 |
| bytes | in | 复制大小，不是元素数量。 | 字节 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：锁页主机源时相对主机同步；可分页源可能在暂存复制后返回。需要完整设备完成时间时显式同步。

资源：不分配内存；双方容量至少为 bytes。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__MEM.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：GPU→CPU 传输

**46. cuMemcpyDtoH** — 把显存结果读回主机。

`CUresult cuMemcpyDtoH(void *dst, CUdeviceptr src, size_t bytes);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| dst | in，指向输出区 | 主机目标地址，函数写入其指向数据。 | 无 |
| src | in | 设备源地址。 | 无 |
| bytes | in | 复制大小。 | 字节 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：返回时该次设备到主机复制已经完成；仍检查 CUresult。

资源：不分配内存，返回成功后可在 CPU 验证结果。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__MEM.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：GPU 内部传输

**47. cuMemcpyDtoD** — 在设备内存之间复制。

`CUresult cuMemcpyDtoD(CUdeviceptr dst, CUdeviceptr src, size_t bytes);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| dst | in，指向输出区 | 设备目标地址。 | 无 |
| src | in | 设备源地址。 | 无 |
| bytes | in | 有效复制字节数。 | 字节 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：即使名称没有 Async，也不执行主机侧同步；应显式等待再计完成时间。

资源：保持源/目标有效，不能把它当成 PCIe 主机传输。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__MEM.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：异步传输

**48. cuMemcpyHtoDAsync** — 把 CPU→GPU 复制排入指定流。

`CUresult cuMemcpyHtoDAsync(CUdeviceptr dst, const void *src, size_t bytes, CUstream stream);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| dst | in，指向输出区 | 设备目标地址。 | 无 |
| src | in | 主机源地址，可靠传输重叠通常使用锁页内存。 | 无 |
| bytes | in | 复制大小。 | 字节 |
| stream | in | 操作排入的流。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：通常异步；可分页内存/资源条件可能导致主机阻塞。对同一流的顺序有保证。

资源：完成前不可改写或释放 src，dst 亦需有效。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__MEM.html)。本项是接口参考，实测状态另查运行日志。

**49. cuMemcpyDtoHAsync** — 把 GPU→CPU 复制排入指定流。

`CUresult cuMemcpyDtoHAsync(void *dst, CUdeviceptr src, size_t bytes, CUstream stream);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| dst | in，指向输出区 | 主机目标缓冲区。 | 无 |
| src | in | 设备源地址。 | 无 |
| bytes | in | 复制大小。 | 字节 |
| stream | in | 操作排入的流。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：通常异步；读取结果前等该流或复制之后记录的事件完成。

资源：完成前不读取/释放 dst，不释放 src。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__MEM.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：GPU 计算

**50. cuLaunchKernel** — 配置线程网格并提交核函数执行。

`CUresult cuLaunchKernel(CUfunction function, unsigned int gx, unsigned int gy, unsigned int gz, unsigned int bx, unsigned int by, unsigned int bz, unsigned int sharedBytes, CUstream stream, void **args, void **extra);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| function | in | cuModuleGetFunction 取得的入口。 | 无 |
| gx, gy, gz | in | 网格的三个维度；每个值是线程块数。 | block |
| bx, by, bz | in | 每个线程块的三个维度。 | thread |
| sharedBytes | in | 每块动态共享内存大小。 | 字节/block |
| stream | in | 本次启动排入的流。 | 无 |
| args | in，可空 | 按核函数声明顺序排列的主机指针数组；每项指向一个参数值的存放位置。 | 无 |
| extra | in，可空 | 另一种参数打包方式；使用 args 时填 NULL，二者不能同时非空。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：相对主机异步；提交成功后仍需同步检查 GPU 执行错误并读回校验。

资源：核函数是 void，计算结果写入所传设备数组；设备内存在 GPU 完成前有效。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__EXEC.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：执行流

**51. cuStreamCreate** — 创建按顺序执行 GPU 操作的队列。

`CUresult cuStreamCreate(CUstream *stream, unsigned int flags);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| stream | out | 流句柄。 | 无 |
| flags | in | 0 为 CU_STREAM_DEFAULT 标志，仍创建显式新流；CU_STREAM_NON_BLOCKING 避免与 legacy 默认流隐式同步。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：不作为 GPU 完成屏障。

资源：最终 cuStreamDestroy；不同流不自动保证彼此先后或一定并发。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__STREAM.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：流等待

**52. cuStreamSynchronize** — 等待指定流此前操作完成。

`CUresult cuStreamSynchronize(CUstream stream);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| stream | in | 需要等待的流。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：阻塞主机直到该流完成；检查返回的异步执行错误。

资源：无独立资源需要释放。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__STREAM.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：执行流回收

**53. cuStreamDestroy** — 销毁流句柄。

`CUresult cuStreamDestroy(CUstream stream);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| stream | in | 自行创建的流。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：可能在排队工作尚未完成时返回，内部资源延后释放；不是完成屏障。

资源：先同步、再销毁；之后不复用流句柄。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__STREAM.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：设备事件

**54. cuEventCreate** — 创建用于完成标记或 GPU 计时的事件。

`CUresult cuEventCreate(CUevent *event, unsigned int flags);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| event | out | 事件句柄。 | 无 |
| flags | in | 0 启用计时；CU_EVENT_DISABLE_TIMING 的事件不能用于耗时计算。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：不作为 GPU 完成屏障。

资源：最终 cuEventDestroy；计时通常需要一对开始/结束事件。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__EVENT.html)。本项是接口参考，实测状态另查运行日志。

**55. cuEventRecord** — 在流中记录一个时间点/完成标记。

`CUresult cuEventRecord(CUevent event, CUstream stream);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| event | in | 已创建的事件。 | 无 |
| stream | in | 将事件排入此流。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：主机提交标记后返回；标记完成代表此流中它之前的工作已完成。

资源：复用事件会覆盖此前记录状态。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__EVENT.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：事件等待

**56. cuEventSynchronize** — 等待事件所记录工作完成。

`CUresult cuEventSynchronize(CUevent event);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| event | in | 已记录的事件。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：阻塞调用主机线程，等待记录点完成；适合在结束事件后读时间。

资源：无独立资源需要释放。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__EVENT.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：设备计时

**57. cuEventElapsedTime** — 求两个已记录 CUDA 事件之间的时间。

`CUresult cuEventElapsedTime(float *milliseconds, CUevent start, CUevent end);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| milliseconds | out | 写回浮点毫秒数；不是 CUresult 返回值。 | 毫秒 |
| start | in | 已完成且启用计时的开始事件。 | 无 |
| end | in | 已完成且启用计时的结束事件。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：本函数不代替等待；未完成可能返回 CUDA_ERROR_NOT_READY。

资源：同一流中围住目标操作；时间可能包括中间排队/其他操作影响。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__EVENT.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：设备事件回收

**58. cuEventDestroy** — 销毁事件句柄。

`CUresult cuEventDestroy(CUevent event);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| event | in | 自行创建的事件。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：事件未完成时也可能返回，资源延后释放；不是完成屏障。

资源：销毁后不得复用句柄。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__EVENT.html)。本项是接口参考，实测状态另查运行日志。


### CUDA Driver / GPU：CUDA 错误

**59. cuGetErrorName** — 把 CUresult 转成符号名称。

`CUresult cuGetErrorName(CUresult error, const char **name);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| error | in | 需解释的 CUDA 状态。 | 无 |
| name | out | 写回驱动持有的只读字符串指针。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：不作为 GPU 完成屏障。

资源：不释放/修改字符串；若 error 无效，本调用也可失败并写 NULL。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__ERROR.html)。本项是接口参考，实测状态另查运行日志。

**60. cuGetErrorString** — 把 CUresult 转成可读错误说明。

`CUresult cuGetErrorString(CUresult error, const char **description);`

| 参数 | 方向 | 含义/取值 | 单位 |
|---|---|---|---|
| error | in | 需解释的 CUDA 状态。 | 无 |
| description | out | 写回驱动持有的只读说明指针。 | 无 |

返回：返回状态 CUresult：CUDA_SUCCESS 为成功，其他值为错误；数据或句柄经标为 out 的参数写回。

同步：不作为 GPU 完成屏障。

资源：不释放字符串；它解释给定状态，不读取 Windows GetLastError。

[官方接口说明](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/group__CUDA__ERROR.html)。本项是接口参考，实测状态另查运行日志。

## Runtime 对照（便于识别其他资料中的写法）

| 操作 | 本文 Driver API | 常见 Runtime 写法 | 注意 |
|---|---|---|---|
| 枚举设备 | cuDeviceGetCount | cudaGetDeviceCount | Runtime 接口返回 cudaError_t，不能拿 GetLastError 解释。 |
| 选择设备/上下文 | cuDeviceGet + 显式上下文管理 | cudaSetDevice | 概念对应而非逐行替换；Runtime 管理主上下文。 |
| 设备内存 | cuMemAlloc / cuMemFree | cudaMalloc / cudaFree | 保持成对分配/释放，谨慎混合 API。 |
| 复制 | cuMemcpyHtoD / cuMemcpyDtoH 等 | cudaMemcpy / cudaMemcpyAsync + 方向参数 | 同样要区分提交时间和完成时间。 |
| 启动 | cuModuleLoad → cuModuleGetFunction → cuLaunchKernel | kernel<<<grid,block,shared,stream>>>(...) 或 cudaLaunchKernel | <<<>>> 是 CUDA C++ 扩展语法，纯 C 源码不直接支持。 |
| 完成等待 | cuCtxSynchronize / cuStreamSynchronize | cudaDeviceSynchronize / cudaStreamSynchronize | 同步后仍需验证计算值是否正确。 |
| 事件计时 | cuEventCreate/Record/ElapsedTime | cudaEventCreate/Record/ElapsedTime | GPU 事件给出设备时间，QPC 可测完整主机端到端时间。 |

## 主要官方资料

- [Microsoft：进程与线程](https://learn.microsoft.com/en-us/windows/win32/procthread/about-processes-and-threads)
- [Microsoft：线程接口索引](https://learn.microsoft.com/en-us/windows/win32/procthread/process-and-thread-functions)
- [NVIDIA：CUDA Driver API 12.6 总目录](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/index.html)
- [NVIDIA：API 同步行为](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/api-sync-behavior.html)
- [NVIDIA：Driver 与 Runtime 区别](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-driver-api/driver-vs-runtime-api.html)
- [NVIDIA：CUDA C++ 编程指南 12.6](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-c-programming-guide/index.html)
- [NVIDIA：PTX ISA 8.5](https://docs.nvidia.com/cuda/archive/12.6.0/parallel-thread-execution/index.html)
- [NVIDIA：CUDA Binary Utilities](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-binary-utilities/index.html)
- [NVIDIA：Runtime 设备接口](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-runtime-api/group__CUDART__DEVICE.html)
- [NVIDIA：Runtime 内存接口](https://docs.nvidia.com/cuda/archive/12.6.0/cuda-runtime-api/group__CUDART__MEMORY.html)
