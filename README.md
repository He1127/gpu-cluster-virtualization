# 显卡集群虚拟化

围绕显卡集群虚拟化，逐步记录 C 程序调用 CPU、Windows API 与 CUDA Driver API 的实验，并在此基础上开展 GPU 性能测试与后续多卡研究。

## 实验目录

| 实验 | 当前内容 | 入口 |
|---|---|---|
| 01 C 调用 CPU 与 GPU | C 运算与反汇编、内存与文件、线程、CUDA 初始化与向量加法、GPU 线程映射 | [实验说明](labs/01-c-cpu-gpu/README.md) · [12 步图文操作](labs/01-c-cpu-gpu/steps.md) · [Word 报告](labs/01-c-cpu-gpu/reports/C调用CPU与GPU.docx) |
| 02 CPU/GPU 接口与单卡压力测试 | 60 项主要接口参考；CPU/向量正确性、数据传输、60 秒预热、300 秒计算与 120 秒显存读写 | [实验说明](labs/02-gpu-stress/README.md) · [图文操作](labs/02-gpu-stress/steps.md) · [接口参考](labs/02-gpu-stress/reference/api-reference.md) · [Word 报告](labs/02-gpu-stress/reports/CPU与GPU调用接口及单卡压力测试.docx) |

目前完成单机单 GPU 基础实验与固定型号测试；多卡调度、资源池和透明虚拟 GPU 功能属于后续研究内容。

## 整理方式

一个实验一个文件夹。代码、脚本、报告和该实验的运行结果放在一起；同一实验复跑时新增运行批次。

```text
labs/
├─ 01-c-cpu-gpu/
│  ├─ README.md / steps.md
│  ├─ src/ / scripts/ / debug/
│  ├─ reports/
│  └─ runs/2026-10-19-01/
└─ 02-gpu-stress/
   ├─ README.md / steps.md
   ├─ src/ / scripts/ / reference/
   ├─ reports/
   └─ runs/
      ├─ 2026-10-20-01/    正式七项测试
      └─ 2026-10-21-01/    环境补图、CPU/info 复跑与重新编译
```

构建结果 `bin/build/results` 不提交；需要保留的原始运行日志与截图整理到 `runs/实际日期-批次`。每个新实验放入 `labs/03-具体名称`，再更新本页目录，无需另建仓库或重传全部文件。

## 运行入口与日期

下载仓库后先读相应实验 README；请使用其中的相对路径命令。截图、Word 中的旧绝对路径只记录当时环境。

第一个实验材料来自 2026-10-19。第二个实验正式长测来自 2026-10-20，Word 于 2026-10-21 更新；补充环境查询、CPU/info 复跑与编译记录另存日期批次。整理发布时完成的短时检查不替代正式长测，详情见各实验说明。

## 后续方向

- 继续学习调用接口、驱动与设备执行之间的关系。
- 扩展可复现的单卡基准、数据校验与监控方法。
- 获得多 GPU 条件后，开展任务拆分、数据通信、调度与资源抽象实验。
