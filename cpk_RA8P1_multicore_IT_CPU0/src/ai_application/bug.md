# 人脸检测 (Face Detection) Bug 修复记录

## 概述

本文档记录了 Titan-mini RA8P1 双核开发板上实现 AI 人脸检测功能过程中遇到的所有 Bug、根因分析和修复方案。调试过程历时多轮，涉及 GLCDC、FreeRTOS、NPU (Ethos-U55)、W25Q64 Flash、D-Cache、TensorFlow Lite 量化参数等多个层面。

---

## Bug #1: GLCDC VSync 回调导致 FreeRTOS 断言崩溃

### 症状
Camera 模式下启动后屏幕变蓝，系统立即卡死，LED 以 assert 模式闪烁。串口日志显示：
```
configASSERT(pxQueue) at queue.c:1344 (xQueueGiveFromISR)
```

### 根因
FSP 在 `common_data.c` 中将 GLCDC 的 vsync 回调硬编码为 `_rm_lvgl_port_display_callback`。该回调调用：
```c
xSemaphoreGiveFromISR(g_semaphore_vpos, ...)
```
但 `g_semaphore_vpos` 信号量只在 `RM_LVGL_PORT_Open()` 中创建。Camera 模式不调用 `RM_LVGL_PORT_Open()`（它用 `rgblcd_init()` 直接打开 GLCDC），因此 `g_semaphore_vpos` 为 NULL → `xQueueGiveFromISR(NULL)` → assert。

### 修复
在 [rgblcd.c](../driver/rgblcd/rgblcd.c) 的 `rgblcd_init()` 中，创建本地 `display_cfg_t` 副本，将回调覆盖为 `DisplayVsyncCallback`（只递增帧计数器，不使用信号量）：
```c
display_cfg_t local_cfg = *g_display0.p_cfg;
local_cfg.p_callback = DisplayVsyncCallback;
err = g_display0.p_api->open(g_display0.p_ctrl, &local_cfg);
```

### 影响文件
- `Titan-mini_RA8P1_multicore_CPU0/src/driver/rgblcd/rgblcd.c`

---

## Bug #2: Face Detection 任务创建失败 — FreeRTOS 堆耗尽

### 症状
日志显示：
```
[FACE_DET] Free heap before alloc: 13120 bytes
[FACE_DET] ERROR: Failed to create task
```

### 根因
`FACE_DETECT_TASK_STACK_SIZE` 原值为 4096 字节，加上任务控制块和信号量，总需求超过可用的 13KB 堆空间。

### 修复
将 [face_detection_config.h](face_detection_config.h) 中栈大小从 4096 降到 2048：
```c
#define FACE_DETECT_TASK_STACK_SIZE (2048)
```

---

## Bug #3: ethou_invoke_v3 返回 -1 — 模型数据未烧录

### 症状
NPU 推理始终失败，日志显示：
```
[NPU] ethosu_invoke_v3 returned -1
[NPU] STATUS=0x00000000 CMD=0x0000000C
[NPU] WD_STATUS=0x00008424 DMA_STATUS0=0x0000033F
```

### 诊断误区
**`DMA_STATUS0=0x0000033F` 被误判为 AXI 读错误长达多轮调试。** 实际上该寄存器在 Ethos-U55 中的位定义全部是**空闲/状态标志**——bits [5:0]=1 表示所有 DMA 引擎（cmd/ifm/wgt/bas/m2m/ofm）均处于**空闲**状态，而非 AXI 错误。DMA_STATUS1=0 也确认了无 AXI 停顿。

这就解释了为什么修改 AXI_LIMIT.memtype、REGIONCFG 等所有 AXI 配置都无法修复问题。

### 真正的根因
**模型权重文件从未烧录到 W25Q64 Flash。** 文件 [sub_0000_model_data.c](model/sub_0000_model_data.c) 第 1KB 全为零（虽然这是该特定模型正常的前导零），但 checksum=0x00000000 明确表示 Flash 中无模型数据。

NPU 的 DMA 成功从 SDRAM 读取了全零的"模型权重" → 无 AXI 错误 → 所有 DMA 引擎空闲。但 NPU 用全零权重进行计算，计算单元卡在异常状态，最终看门狗超时。

### 修复
通过 PCDC Flash 工具部署模型到 W25Q64：
```bash
python scripts/pcdc_flash_tool.py COM<N> write sub_0000_model_data.c 0x000000
python scripts/pcdc_flash_tool.py COM<N> write sub_0000_command_stream.c 0x000670A0
```

### 影响文件
- 无需代码修改（模型数据通过外部工具部署）

---

## Bug #4: W25Q64 Flash 写入丢失数据 — PCDC Flash Tool 写入失败

### 症状
PCDC 工具写入 422KB 模型数据后 verify 显示 MISMATCH：
```
MISMATCH @ 0x000000F8: expected 0x00, got 0xFF
MISMATCH @ 0x000002F8: expected 0x00, got 0xFF
```
每个 256 字节页的最后 8 字节未被写入。

### 根因
W25Q64 Flash 写入驱动的 `R_OSPI_B_Write` 路径要求写入大小为 **8 的倍数**。PCDC 协议的 WRITE 帧 payload 为 252 字节（加上 4 字节地址头 = 256 字节帧），但 `w25q64_write()` 内的 burst 分块最后一段可能为 60 或 4 字节——非 8 倍数 → FSP 驱动丢弃/截断。

页边界处的数据丢失规律：
- Chunk N 末段: 60 字节 → 被截断为 56 字节 → 丢失 4 字节
- Chunk N+1 首段 (跨页边界余数): 4 字节 → 被截断为 0 字节 → 丢失 4 字节
- 合计每 256 字节页丢失 8 字节 ⚡ 与 verify 结果完全吻合

### 修复
切换为 `R_OSPI_B_DirectTransfer` 写入路径（每 4 字节一个独立的 WREN + Page Program，不受 8 字节对齐限制）：
```c
#define WRITE_CUSTOM 1   /* 绕过 FSP R_OSPI_B_Write 的 8 字节对齐 bug */
```

### 影响文件
- `Titan-mini_RA8P1_multicore_CPU0/src/driver/w25q64/w25q64.c` (第 197 行)

---

## Bug #5: 模型数据 "all 0x00" 误报

### 症状
启动日志始终显示：
```
[FACE_DET] Model checksum (first 1KB)=0x00000000
[FACE_DET] *** FATAL: Model data is all zeros ***
```
但 NPU 能产生有意义的输出（objectness 值各异）。

### 根因
模型文件的前 1024 字节**恰好全部为零**（该 YOLO-Fastest 量化模型的正常前导零权重）。检查前 32 字节全零不能作为"未烧录"的判定依据。

### 修复
移除仅检查头部的误报警告。如果 Flash 真的未烧录，CMS 的 COP1 魔数会异常、NPU 会看门狗超时，这些都是更可靠的指标。

### 影响文件
- [face_detection_task.c](face_detection_task.c)

---

## Bug #6: 量化参数别名导致零检测 ⭐ 核心 Bug

### 症状
- NPU 推理成功（`ethosu_invoke_v3 returned 0`）
- 输出张量有变化，Max objectness raw 值高达 79~81（sigmoid 后应 >95%）
- 但后处理始终返回 0 个检测框：`Detections: 0`
- 量化参数诊断显示异常：
  ```
  QParams Out0: scale=0.000000 zp=10  Out1: scale=0.185359 zp=10
  ```

### 根因
**三条连锁的 bug：**

**① 静态变量共享**
原 `create_int8_tensor()` 函数使用 `static` 局部变量存储 `TfLiteAffineQuantization`、`TfLiteFloatArray`、`TfLiteIntArray`。两次调用（Out0 和 Out1）共享同一套 static 变量。第二次调用覆盖了第一次的值 → 两个输出张量的 `quantization.params` 最终都指向 Out1 的参数（zp=10）。

**② 编译器死存储消除 (DSE)**
尝试用文件作用域 static 变量 + 运行时赋值修复时，编译器优化掉了对量化结构体的写入（因为编译器分析不到 `DetectorPostProcess` 会透过不透明指针读取这些值）。导致运行时写入的值丢失，scale 读回为 0.0。

**③ C++ 零初始化崩溃**
`TfLiteTensor outputTensor0 = {};` 在 C++ 模式下的零初始化与 C struct 布局不兼容，导致构造函数/析构函数异常，在第 4 次推理后任务崩溃。

### 修复
使用**编译期初始化**（`.data` 段）的独立量化结构体，每个张量完全隔离：

```c
// 文件作用域，编译期初始化 — 不会被优化掉
static TfLiteFloatArray s_out0 = { .size = 1, .data = { 0.1340839f } };
static TfLiteIntArray   z_out0 = { .size = 1, .data = { 47 } };
static TfLiteAffineQuantization q_out0 = {
    .scale = &s_out0, .zero_point = &z_out0, .quantized_dimension = 0
};
// Out1 同理，独立存储
static TfLiteFloatArray s_out1 = { .size = 1, .data = { 0.1853593f } };
static TfLiteIntArray   z_out1 = { .size = 1, .data = { 10 } };
static TfLiteAffineQuantization q_out1 = {
    .scale = &s_out1, .zero_point = &z_out1, .quantized_dimension = 0
};
```

TfLiteTensor 初始化使用 `memset()` 替代 C++ 的 `= {}`：
```c
TfLiteTensor outputTensor0;
memset(&outputTensor0, 0, sizeof(outputTensor0));
outputTensor0.quantization.params = &q_out0;  // 指向编译期初始化的结构
```

### 影响文件
- [face_detection_main.cc](face_detection_main.cc)

---

## Bug #7: 屏幕闪烁（NPU 推理期间 SDRAM 带宽超限）⭐ 最终修复

### 症状
每 N 帧进行一次 NPU 推理时，屏幕出现闪烁（画面撕裂/像素异常）。增大 `INFER_EVERY_N_FRAMES` 可降低闪烁频率但无法根除。

### 诊断过程

| 测试 | 帧缓冲写入 | NPU 推理 | 结果 | 分析 |
|------|-----------|---------|------|------|
| ① FB_WRITE_DISABLE | ✗ | ✓ | **不闪** | GLCDC FIFO 下溢在黑屏上不可见 |
| ③ NPU_MEMSET_TEST | ✓ | ✗(CPU memset) | **不闪** | 无 NPU SDRAM 访问，带宽未超限 |
| 正常模式 | ✓(skip时停) | ✓ | **闪** | NPU + GLCDC + VIN 三者带宽超限 |

**测试① 不闪的原因**：NPU 推理时 GLCDC 同样存在 FIFO 下溢，但静态黑屏画面中下溢产生的异常像素（通常也是暗色）肉眼不可见。相机画面包含丰富纹理，下溢造成的像素错位/颜色异常非常明显。

### 尝试过但失败的方案

1. **降低 NPU 时钟** (500→250 MHz via e2studio SCKDIVCR2) — 无效。SCKDIVCR2 运行时不可写（受 PRCR CGC 保护），寄存器值实际未改变
2. **NPU Arena 移到内部 SRAM** — NPU 完全不工作。Ethos-U55 AXI master 无法访问内部 SRAM（Bug #9）
3. **NPU 推理期间跳过帧缓冲写入** (`g_npu_inferencing` 标志) — 无效。跳帧只能减少 CPU 写 FB 的带宽，不能减少 NPU 本身的 SDRAM 访问
4. **NPU keep-powered** (`ethosu_request_power`) — 无效。NPU 电源状态切换不是根因
5. **覆盖 `vApplicationIdleHook` 阻止 CPU 睡眠** — 无效。CPU 睡眠不是根因
6. **减小 D-Cache clean 范围** (442KB→36KB) — 有优化作用但未能根除
7. **移除每次推理的 CMS 重新加载** — 有优化作用但未能根除

### 真正的根因

**NPU 的 4 个 AXI 限制器（AXI_LIMIT0-3）独立并行工作，加上高 outstanding 配置，NPU 可以同时发出大量并发 SDRAM 事务，独占总线带宽。**

SDRAM 带宽计算（W9825G6KH-6，166 MHz × 16-bit，实际可用 ~200 MB/s）：

| 组件 | 带宽 | 说明 |
|------|------|------|
| **NPU 推理** | **~216 MB/s** | 422KB 权重 + 442KB arena 读写，默认 ~4ms 内完成 |
| GLCDC 扫描输出 | ~60 MB/s | 1024×600 RGB565 @ ~60 Hz |
| VIN 摄像头 DMA | ~18 MB/s | 640×480 RGB565 @ 30 fps 连续采集 |
| **合计** | **~294 MB/s** | **1.5 倍于 SDRAM 物理上限** |

原 AXI 配置将 6 个 NPU 地址区域分散到 4 个独立限制器，每个限制器允许 2 个 outstanding 读 + 2 个 outstanding 写。NPU 最高可发出 4 × 2 = **8 个并发读事务**——几乎独占 SDRAM 总线。GLCDC 的像素 FIFO 在 NPU 推理的 ~4ms 内持续欠载 → 可见屏幕闪烁。

### 修复 ⭐

在 [face_detection_task.c](face_detection_task.c) 中，`RM_ETHOSU_Open()` 后配置 NPU AXI 寄存器：

**① memtype = Normal Bufferable**（SDRAM 必需，复位值为 Device Non-Bufferable）

**② max_outstanding reads = 1, writes = 1**（寄存器编码 0 = 1 个实际 outstanding，这是硬件最小值）

**③ REGIONCFG：所有 6 个地址区域 → 限制器 0**（而非分散到 4 个限制器）：

```c
/* 之前：6 个区域分散到 4 个限制器 → 8 个并发读 */
npu[0x003C / 4] = (0u << 0) | (1u << 2) | (2u << 4)
                | (3u << 6) | (0u << 8) | (1u << 10);

/* 修复后：全部经限制器 0 串行化 → 1 个并发读 */
npu[0x003C / 4] = (0u << 0) | (0u << 2) | (0u << 4)
                | (0u << 6) | (0u << 8) | (0u << 10);
```

**效果**：NPU 一次只能发 1 个读 OR 1 个写 AXI 事务。峰值带宽从 ~216 MB/s 降至 **~108 MB/s**。推理时间从 ~4ms 增至 ~8ms，但 SDRAM 总需求降至 ~186 MB/s——**在 200 MB/s 物理上限以内**。GLCDC FIFO 不再欠载，屏幕不再闪烁。

### 辅助优化

以下修改在 [sub_0000_invoke.c](model/sub_0000_invoke.c) 中，进一步减少推理期间的 SDRAM 流量：

1. **D-Cache clean 从 442KB 缩小到 36KB**：CPU 只写入输入 tensor（arena offset 0x24000，36KB），无需清理整个 arena
2. **移除每次推理的 CMS 重新加载**：CMS 启动时从 W25Q64 加载到 SDRAM 一次，不会被 camera DMA 破坏

### 影响文件
- [face_detection_task.c](face_detection_task.c) — **主要修复**：NPU AXI 限制器配置
- [model/sub_0000_invoke.c](model/sub_0000_invoke.c) — 辅助优化：精准 D-Cache clean + 移除 CMS reload
- [../driver/mipi_camera/mipi_camera_lcd.c](../driver/mipi_camera/mipi_camera_lcd.c) — 防御性帧跳过（保留，但不再是主要修复手段）

---

## Bug #8: NPU AXI 配置（已合并到 Bug #7）

AXI_LIMIT0-3 寄存器的 `memtype` 字段复位值为 0（Device Non-Bufferable），SDRAM 需要配置为 3（Normal Non-Cacheable Bufferable）。原配置还使用了高 outstanding 值（32 读 / 16 写）并分散到 4 个独立限制器——这是 Bug #7 闪屏的根本原因。详见 Bug #7 的修复方案。

---

## Bug #9: NPU Arena 移到 SRAM 导致推理失败

### 症状
Arena 改为 `static uint8_t npu_arena_sram[442368]`（位于内部 SRAM ~0x22000000+）后，人脸检测框完全不出现。

### 根因
**Ethos-U55 NPU 的 AXI master 无法访问内部 SRAM**。RA8P1 的总线矩阵中，NPU 的 AXI 接口只连接到外部存储器总线（SDRAM、OSPI Flash 等）。内部 SRAM（0x22000000）位于 Cortex-M85 的 AHB 系统总线上，不在 NPU AXI master 的可达范围内。NPU 读/写 arena 失败 → 推理输出全部为零 → 后处理检测不到任何人脸。

### 教训
**不是所有总线 master 都能访问所有地址空间。** 芯片的 bus matrix 拓扑决定了每个 master 的可达范围。Ethos-U55 作为 AXI master 只能访问外部存储器。内部 SRAM/TCM 等紧耦合存储器仅供 CPU 使用。

### 修复
回退到 SDRAM arena（`sub_0000_arena = (uint8_t *)0x68500000`），闪屏问题改用 Bug #7 的帧跳过方案。

### 影响文件
- [model/sub_0000_invoke.c](model/sub_0000_invoke.c)

---

## 经验教训

1. **不要相信寄存器名称** — `DMA_STATUS0` 不是错误寄存器，是空闲状态寄存器。每个 bitfield 必须对照 datasheet/接口头文件确认含义。

2. **static 局部变量 + 指针 = 定时炸弹** — C/C++ 中多次调用使用 static 局部变量保存状态并通过指针传递的函数，容易产生别名 bug。

3. **编译器优化会消除你的修复** — 运行时写入 static 变量后通过不透明指针读取，Clang 的 DSE pass 可能认为写入是 dead store。编译期初始化（`.data` 段）是最可靠的替代方案。

4. **C struct 在 C++ 中的 `= {}` 初始化不安全** — Clang ARM 在 C++ 模式下对 C POD struct 的处理可能与预期不同。`memset` 是更可靠的清零方式。

5. **先验证数据再怀疑硬件** — Flash 未烧录模型这个最基础的问题，被误判为复杂的 AXI/NPU 总线问题，浪费了大量调试时间。

6. **FSP 驱动有坑** — `R_OSPI_B_Write` 的 8 字节对齐限制没有文档化，写入参数不对齐时静默丢弃数据。

7. **多 master SDRAM 带宽预算必须精确计算** — NPU + GLCDC + VIN DMA 三者同时访问 SDRAM 时，峰值带宽需求 (~294 MB/s) 远超物理上限 (~200 MB/s)。不能只降低单个组件的带宽——必须通过 AXI QoS/limiter 机制从源头限制最占带宽的 master（本案例中是 NPU 的 4 个并发限制器 → 1 个串行限制器）。

8. **黑屏测试会掩盖显示问题** — 静态纯黑画面即使发生 GLCDC FIFO 下溢也看不出来，因为异常像素在黑色背景下不可见。诊断显示问题时始终使用有丰富纹理的测试画面。
