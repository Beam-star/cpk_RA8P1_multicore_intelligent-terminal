# AI 模型部署与推理完整流程文档

> **Titan-mini RA8P1 智能会议交互终端 — 人脸检测 AI 流水线**
>
> 从 TFLite 模型量化编译 → W25Q64 Flash 部署 → SDRAM 加载 → NPU 推理 → 摄像头画面人脸检测的端到端流程。

---

## 目录

1. [概述](#1-概述)
2. [第一阶段：模型编译（RUHMI 量化 + NPU 命令流生成）](#2-第一阶段模型编译ruhmi-量化--npu-命令流生成)
3. [第二阶段：模型部署到 W25Q64 Flash（PCDC 工具）](#3-第二阶段模型部署到-w25q64-flashpcdc-工具)
4. [第三阶段：系统启动与模型加载（Flash → SDRAM）](#4-第三阶段系统启动与模型加载flash--sdram)
5. [第四阶段：摄像头采集与帧同步](#5-第四阶段摄像头采集与帧同步)
6. [第五阶段：人脸检测推理循环](#6-第五阶段人脸检测推理循环)
   - [5.1 帧信号触发](#51-帧信号触发)
   - [5.2 预处理：RGB565 → INT8 灰度图](#52-预处理rgb565--int8-灰度图)
   - [5.3 NPU 推理](#53-npu-推理)
   - [5.4 后处理：反量化 → YOLO 解码 → NMS](#54-后处理反量化--yolo-解码--nms)
7. [第六阶段：检测结果渲染到 LCD](#7-第六阶段检测结果渲染到-lcd)
8. [附录：关键数据流与内存布局](#8-附录关键数据流与内存布局)

---

## 1. 概述

### 1.1 硬件平台

| 组件 | 型号/规格 |
|------|----------|
| MCU | Renesas RA8P1 (R7KA8P1KFLCAC) |
| CPU0 | Cortex-M85 @ 250MHz，带 Helium/MVE |
| NPU | Ethos-U55-256 (Cortex-M85 内置) |
| 摄像头 | OV5640，MIPI CSI-2 接口，640×480 RGB565 |
| Flash | W25Q64JV，8MB，OSPI_B 接口 |
| SDRAM | W9825G6KH-6，32MB @ 166MHz |
| 显示屏 | GLCDC 1024×600 RGB565，双图层 |

### 1.2 AI 模型

| 参数 | 值 |
|------|-----|
| 模型架构 | YOLO-Fastest |
| 输入 | 192×192×1 INT8 灰度图 |
| 输出 | 2 个 INT8 张量（6×6 + 12×12 多尺度检测） |
| 模型权重 | 422,048 字节 (INT8) |
| NPU 命令流 | 11,252 字节 |
| Tensor Arena | 442,368 字节 (SDRAM) |
| 推理耗时 | ~8ms (AXI 限流后) |
| 检测阈值 | 0.3 (置信度) / 0.45 (NMS IoU) |
| 最大检测数 | 20 个人脸 |

### 1.3 总体数据流

```
┌─────────────────────────────────────────────────────────────────────┐
│  ① 模型编译 (PC)                                                     │
│  TFLite (Float) ──[RUHMI + Vela]──→ 权重.c + 命令流.c                │
├─────────────────────────────────────────────────────────────────────┤
│  ② 部署 (PC→设备)                                                    │
│  .c 数组文件 ──[PCDC USB 虚拟串口]──→ W25Q64 Flash 指定分区           │
├─────────────────────────────────────────────────────────────────────┤
│  ③ 启动加载 (设备初始化)                                              │
│  W25Q64 ──[w25q64_read()]──→ SDRAM 0x68570000                       │
├─────────────────────────────────────────────────────────────────────┤
│  ④ 运行时推理 (每帧循环)                                              │
│  OV5640 → VIN DMA → SDRAM 帧缓冲                                     │
│      → 预处理 (RGB565→INT8 192×192)                                  │
│      → NPU 推理 (ethosu_invoke_v3)                                   │
│      → 后处理 (反量化 → YOLO解码 → NMS)                               │
│      → 边界框绘制 → GLCDC 显示                                        │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 2. 第一阶段：模型编译（RUHMI 量化 + NPU 命令流生成）

### 2.1 编译入口

编译由根目录下的批处理脚本触发：

**文件**: `run_compile2.bat`（根目录）

```bat
cd /d E:\ruhmi-framework-mcu-main\scripts
E:\ruhmi-framework-mcu-main\.venv\Scripts\python.exe mcu_compile.py ^
  E:\...\src\yolov5\yolov5n.tflite ^          ← 输入：Float32 TFLite 模型
  E:\...\src\yolov5\deploy_output ^            ← 输出目录
  --npu --quantize --external                   ← 编译选项
```

### 2.2 关键编译选项

**文件**: `src/yolov5/deploy_output/build/MERAInterpreter/deploy_cfg.json`

| 选项 | 值 | 含义 |
|------|-----|------|
| `enable_ethos` | `true` | 生成 Ethos-U NPU 命令流 |
| `enable_ospi` | `false` | 不从 OSPI 直接执行（模型在 SDRAM） |
| `enable_ref_data` | `false` | 不包含参考数据（减小体积） |
| `convert_canonical` | `true` | 转换为标准 NPU 算子 |
| `fe_mode` | `"CONVERTED_ONLY"` | 仅编译模式 |
| `devices` | `["CPU", "EC_C_CODEGEN"]` | 目标：CPU 子图 + Ethos-U C 代码 |

### 2.3 编译输入

**文件**: `src/yolov5/yolov5n.tflite`

- **必须是 Float32 TFLite**，不能是预量化的 INT8 模型
- 如果用 INT8 tflite + `--quantize` → RUHMI 编译器崩溃 (0xC0000005)
- 量化由 RUHMI 在编译过程中完成

### 2.4 编译产物

输出到 `src/yolov5/deploy_output/`，需要手动复制到 `src/ai_application/model/`：

| 产物文件 | 大小 | 用途 |
|----------|------|------|
| `sub_0000_model_data.c/h` | 422,048 B | INT8 模型权重 |
| `sub_0000_command_stream.c/h` | 11,252 B | Ethos-U NPU 命令流 |
| `sub_0000_tensors.c/h` | — | 张量地址/大小元数据 |
| `sub_0000_invoke.c/h` | — | NPU 调用封装（Runtime 改写） |
| `sub_0000_io_data.c/h` | — | 测试用 I/O 数据（不编译进固件） |
| `model.c/h` | — | RunModel() 入口 + 张量指针访问器 |
| `wrapper.h` | — | mera_input_ptr / mera_invoke 便捷宏 |

### 2.5 sub_0000_tensors 的关键定义

**文件**: `src/ai_application/model/sub_0000_tensors.h` — 行 11-16

```c
#define kArenaSize_sub_0000 442368    // NPU arena 总大小 442KB

extern const uint32_t sub_0000_address_image_input;    // 输入张量在 arena 中的偏移
extern const uint32_t sub_0000_address_Identity_70275; // 输出张量0 (6×6 分支)
extern const uint32_t sub_0000_address_Identity_1_70284; // 输出张量1 (12×12 分支)
```

这些地址由 RUHMI 编译器在编译时确定，定义了每个张量在 442KB arena 中的偏移位置。

### 2.6 wrapper.h — 便捷访问宏

**文件**: `src/ai_application/model/wrapper.h` — 行 12-27

```c
// 获取 NPU 输入张量指针 = arena + sub_0000_address_image_input
static inline uint8_t* mera_input_ptr() {
    return (uint8_t*) GetModelInputPtr_image_input();
}
// 获取输出张量0指针 (6×6 stride-32 分支)
static inline uint8_t* mera_output1_ptr() {
    return (uint8_t*) GetModelOutputPtr_Identity_70275();
}
// 获取输出张量1指针 (12×12 stride-16 分支)
static inline uint8_t* mera_output2_ptr() {
    return (uint8_t*) GetModelOutputPtr_Identity_1_70284();
}
// 触发一次完整推理
static inline void mera_invoke() {
    RunModel(false);  // false = 不清理输出区域 (由调用方管理)
}
```

这些宏是 C/C++ 推理代码访问 NPU 输入/输出张量的统一入口。`GetModelInputPtr_image_input()` 的实际实现位于 `model.c:65-67`，返回 `sub_0000_arena + sub_0000_address_image_input`。

---

## 3. 第二阶段：模型部署到 W25Q64 Flash（PCDC 工具）

### 3.1 部署工具

**文件**: `scripts/pcdc_flash_tool.py`（PC 端 Python 脚本）

通过 USB 虚拟串口 (PCDC) 将模型数据写入 W25Q64 指定地址。

### 3.2 部署命令序列

```bash
# Step 1: 擦除 AI 模型分区 (448KB, 从 0x000000 开始)
python scripts/pcdc_flash_tool.py COM10 erase 0x000000 0x070000

# Step 2: 写入模型权重 (422,048 字节 → Flash 偏移 0x000000)
python scripts/pcdc_flash_tool.py COM10 write sub_0000_model_data.c 0x000000

# Step 3: 写入 NPU 命令流 (11,252 字节 → Flash 偏移 0x000670A0)
python scripts/pcdc_flash_tool.py COM10 write sub_0000_command_stream.c 0x000670A0
```

### 3.3 为什么模型数据不编译进固件

- `sub_0000_model_data.c`（422KB）和 `sub_0000_command_stream.c`（11KB）在 e2studio 工程中被**排除编译**
- 它们仅作为 PCDC 工具的**输入文件**使用
- 模型在运行时从 W25Q64 Flash **动态加载**到 SDRAM
- 节省固件 Flash 空间约 433KB

### 3.4 Flash 分区布局（模型相关部分）

**文件**: `src/driver/w25q64/w25q64_partition.h`

```
W25Q64 8MB:
0x000000 ┌────────────────────────┐
         │ YOLO 模型权重            │ 422,048 B
0x000670A0 ├────────────────────────┤
         │ YOLO NPU 命令流          │  11,252 B
0x00070000 ├────────────────────────┤
         │ 资产目录 (Asset Dir)     │   4 KB
0x071000 ├────────────────────────┤
         │ 启动 Logo               │ 1.2 MB
         │ ...                     │
```

关键常量（`face_detection_config.h` 行 40-42）：

```c
#define FACE_MODEL_FLASH_OFFSET          (0x00000000)
#define FACE_MODEL_DATA_SIZE             (422048)
#define FACE_MODEL_COMMAND_STREAM_SIZE   (11252)
```

---

## 4. 第三阶段：系统启动与模型加载（Flash → SDRAM）

### 4.1 启动入口

**文件**: `src/cpu0main_thread_entry.c` — 行 268-348（MODE_CAMERA 模式）

操作模式设为 `MODE_CAMERA` (行 62) 且 `FACE_DETECTION_ENABLE=1` (行 80) 时，启动流程为：

```
R_BSP_SecondaryCoreStart()  → 启动 CPU1
CONSOLE_Init()              → SEGGER RTT 控制台
perfc_init()                → 性能计数器
rpmsg_core_init()           → RPMsg-Lite 初始化
rpmsg_log_cpu0_init()       → CPU1 日志中继
rpmsg_record_cpu0_init()    → 录制控制通道
SCB_EnableDCache()          → 使能 D-Cache

rgblcd_backlight_init()     → LCD 背光
lv_init()                   → LVGL 内存初始化
RM_LVGL_PORT_Open()         → GLCDC 双图层
gt911_init()                → 触摸控制器
w25q64_open()               → OSPI Flash 驱动
face_db_init()              → 人脸数据库（行 326）
face_embedding_init()       → 人脸特征模型加载（行 330）
boot_logo_show_layer1(2000) → 开机 Logo
lvgl_ui_init()              → LVGL UI 面板
mipi_camera_lcd_start(...)  → 摄像头+人脸检测启动（行 347）
```

### 4.2 模型加载：load_model_from_flash()

**文件**: `src/ai_application/face_detection_task.c` — 行 127-196

这是整个加载过程的核心函数：

```
load_model_from_flash()
  │
  ├─ [行 133] w25q64_open()                         打开 OSPI_B 驱动
  ├─ [行 140] w25q64_read_jedec_id()                验证 Flash 存在
  │
  ├─ [行 147] g_model_data_sdram = (uint8_t*)0x68570000
  │           分配 SDRAM 目标地址
  │
  ├─ [行 148] w25q64_read(                          从 Flash 读取模型权重
  │              FACE_MODEL_FLASH_OFFSET,             偏移 = 0x000000
  │              g_model_data_sdram,                  目标 = 0x68570000
  │              FACE_MODEL_DATA_SIZE)                大小 = 422,048 B
  │
  ├─ [行 154] g_command_stream_sdram = g_model_data_sdram + 422048
  │           命令流紧接在权重数据之后 (0x685670A0)
  │
  ├─ [行 155] w25q64_read(                          从 Flash 读取命令流
  │              FACE_MODEL_FLASH_OFFSET + 422048,    偏移 = 0x000670A0
  │              g_command_stream_sdram,              目标 = 0x685670A0
  │              FACE_MODEL_COMMAND_STREAM_SIZE)      大小 = 11,252 B
  │
  ├─ [行 177] SCB_CleanDCache_by_Addr(               ★关键★ D-Cache 清理
  │              g_model_data_sdram,                  确保 NPU 能读到
  │              FACE_MODEL_DATA_SIZE)                正确的模型数据
  │
  ├─ [行 179] SCB_CleanDCache_by_Addr(               ★关键★ D-Cache 清理
  │              g_command_stream_sdram,              命令流区域
  │              FACE_MODEL_COMMAND_STREAM_SIZE)
  │
  └─ [行 194] w25q64_close()                        释放 OSPI_B（模型已在 SDRAM）
```

**为什么需要 D-Cache Clean**：NPU 通过 AXI 总线直接读取 SDRAM，绕过 CPU 的 D-Cache。如果 D-Cache 中有脏行（CPU 写入但未写回 SDRAM），NPU 将读到过期/损坏的数据。`SCB_CleanDCache_by_Addr` 强制将 Cache 中的脏数据写回 SDRAM。这是一次性操作（启动时执行一次），不产生运行时开销。

### 4.3 创建人脸检测任务：face_detection_task_start()

**文件**: `src/ai_application/face_detection_task.c` — 行 356-388

```c
void face_detection_task_start(void) {
    // 1. 创建帧同步信号量 (行 363)
    g_frame_ready_sem = xSemaphoreCreateBinary();

    // 2. 创建 FreeRTOS 任务 (行 374-380)
    xTaskCreate(face_detection_task_entry,   // 任务函数
                "face_det",                  // 任务名
                FACE_DETECT_TASK_STACK_SIZE, // 栈大小 = 2048
                NULL,
                FACE_DETECT_TASK_PRIORITY,   // 优先级 = 2
                NULL);
}
```

此函数由摄像头任务在初始化阶段调用（`mipi_camera_lcd.c` 行 300）。

### 4.4 任务入口：face_detection_task_entry()

**文件**: `src/ai_application/face_detection_task.c` — 行 202-350

```
face_detection_task_entry()
  │
  ├─ [行 209] load_model_from_flash()         加载模型 W25Q64→SDRAM
  │
  ├─ [行 217] RM_ETHOSU_Open()                打开 Ethos-U NPU 驱动
  │
  ├─ [行 230] ethosu_request_power()          保持 NPU 供电（避免每次推理重新上电）
  │
  ├─ [行 260-274] NPU AXI 限流器配置            ★关键★ 防止屏幕闪烁
  │   将所有 6 个地址区域映射到单一限流器 0
  │   max_outstanding_reads  = 1
  │   max_outstanding_writes = 1
  │   memtype = Normal Bufferable (SDRAM 要求)
  │
  └─ [行 287-349] 主推理循环
       while(1) {
         xSemaphoreTake(g_frame_ready_sem)    等待摄像头帧就绪
         → 跳帧 (每 10 帧推理 1 次)
         → 预处理 → NPU 推理 → 后处理
       }
```

#### NPU AXI 限流器配置（面部检测任务入口，行 260-274）

这是解决**屏幕闪烁问题**的主要修复。原始问题：

| 带宽消费者 | 峰值带宽 |
|-----------|---------|
| NPU 推理 | ~216 MB/s |
| GLCDC 扫描 | ~60 MB/s |
| VIN 摄像头 DMA | ~18 MB/s |
| **总计** | **~294 MB/s** |

而 SDRAM 物理带宽仅约 **200 MB/s**。NPU 默认可以通过 4 个独立 AXI 限流器同时发送多个读/写请求，垄断 SDRAM 总线 → GLCDC 像素 FIFO 下溢 → 屏幕闪烁。

**修复**：将所有 6 个地址区域通过**单一限流器**串行化，每次仅允许 1 个读或 1 个写。NPU 带宽降至 ~108 MB/s（推理时间从 ~4ms 增至 ~8ms），总需求降为 ~186 MB/s < 200 MB/s。

```c
volatile uint32_t *npu = (volatile uint32_t *)R_NPU_BASE;
// 4 个限流器统一配置
static const uint32_t lim[] = { 0x0040, 0x0044, 0x0048, 0x004C };
for (int i = 0; i < 4; i++) {
    uint32_t v = npu[lim[i] / 4];
    v &= ~((0xFu << 3) | (0x3FFu << 7) | (0x3FFu << 17) | 0x7u);
    v |=  (3u    << 3);   // memtype = Normal Bufferable
    v |=  (0u    << 7);   // max_outstanding_reads  = 1
    v |=  (0u    << 17);  // max_outstanding_writes = 1
    npu[lim[i] / 4] = v;
}
// REGIONCFG: 全部 6 个地址区域 → 限流器 0
npu[0x003C / 4] = (0u << 0) | (0u << 2) | (0u << 4)
                | (0u << 6) | (0u << 8) | (0u << 10);
```

---

## 5. 第四阶段：摄像头采集与帧同步

### 5.1 摄像头任务启动

**文件**: `src/driver/mipi_camera/mipi_camera_lcd.c` — 行 191-206

```c
void mipi_camera_lcd_start(bool use_test_pattern, bool enable_face_detection) {
    // 将两个布尔参数打包为一个 32 位值（行 193-194）
    uint32_t param = ((uint32_t)use_test_pattern & 1)
                   | (((uint32_t)enable_face_detection & 1) << 1);
    // 创建摄像头+LCD 统一任务，优先级 4（最高）（行 197-201）
    xTaskCreate(mipi_camera_lcd_task, "cam_lcd", 4096, (void *)param, 4, NULL);
}
```

从 `cpu0main_thread_entry.c` 行 347 调用：
```c
mipi_camera_lcd_start(false, (bool)FACE_DETECTION_ENABLE);
```
第一个参数 `false` = 正常摄像头模式（非测试彩条），第二个参数 = 使能 AI 人脸检测。

### 5.2 摄像头任务初始化阶段

**文件**: `src/driver/mipi_camera/mipi_camera_lcd.c` — 行 222-308

```
mipi_camera_lcd_task()
  │
  ├─ [行 242-256] 清空两个帧缓冲 (fb_background[0] 和 fb_background[1])
  │              全黑 (0x0000)，D-Cache Clean 确保 GLCDC 可读
  │
  ├─ [行 259-274] 等待第一个 VSYNC 信号（确认 GLCDC 正在运行）
  │              超时 2 秒则告警
  │
  ├─ [行 280] mipi_camera_init()             初始化 OV5640 + MIPI CSI + VIN
  │
  ├─ [行 298-302] face_detection_task_start() ★创建人脸检测任务★
  │               （仅在 enable_face_detection=true 时）
  │
  └─ [行 307] mipi_camera_capture_start()    启动 VIN 连续采集
```

### 5.3 摄像头任务主循环（帧采集+显示）

**文件**: `src/driver/mipi_camera/mipi_camera_lcd.c` — 行 331-463

这是整个系统的**心跳循环**，每帧执行一次：

```
while(1) {
  ┌─────────────────────────────────────────────────────────┐
  │ 6a [行 333] mipi_camera_wait_frame(500)                │
  │     等待 VIN DMA 完成一帧采集 (超时 500ms)                │
  │     VIN ISR 在 DMA 完成后 Give 信号量                    │
  ├─────────────────────────────────────────────────────────┤
  │ 6b [行 341] vin_frame = mipi_camera_get_frame()        │
  │     获取当前帧的 SDRAM 地址指针                          │
  │     [行 348] SCB_InvalidateDCache_by_Addr(vin_frame)   │
  │     ★关键★ D-Cache 无效化: VIN DMA 写 SDRAM 绕过 Cache   │
  │     若 CPU 读到 Cache 中的旧数据 → 画面撕裂               │
  ├─────────────────────────────────────────────────────────┤
  │ 6c [行 364-367] face_detection_signal_new_frame(addr)  │
  │     ★通知 AI 任务★ 有新帧可用                           │
  │     Give 信号量 g_frame_ready_sem → 唤醒人脸检测任务      │
  ├─────────────────────────────────────────────────────────┤
  │ 6d [行 378-388] 等待 VSYNC (GLCDC 行同步信号)            │
  │     等待 g_frame_count 变化 → GLCDC 已切换到另一缓冲      │
  │     确保目标帧缓冲不再被 GLCDC 扫描输出                   │
  ├─────────────────────────────────────────────────────────┤
  │ 6d2 [行 397-403] 防御性检查: g_npu_inferencing?         │
  │     若 NPU 正在访问 SDRAM → 跳过帧缓冲写入               │
  │     防止 SDRAM 带宽竞争导致屏幕闪烁                       │
  ├─────────────────────────────────────────────────────────┤
  │ 6e [行 412-422] 帧旋转+写入 OFF-SCREEN 缓冲              │
  │     垂直翻转 (摄像头传感器行序与显示相反)                  │
  │     uint32_t 字拷贝 (比 uint16_t 减少一半 SDRAM 事务)     │
  ├─────────────────────────────────────────────────────────┤
  │ 6e2 [行 431] draw_face_boxes(fb)                        │
  │     读取 g_face_detection_results[] → 画绿色矩形框        │
  ├─────────────────────────────────────────────────────────┤
  │ 6f [行 439-442] SCB_CleanDCache_by_Addr(fb)             │
  │     ★关键★ D-Cache 清理: 确保 GLCDC 能读到最新像素        │
  ├─────────────────────────────────────────────────────────┤
  │ 6g [行 450] R_GLCDC_BufferChange(fb, LAYER_1)           │
  │     调度原子缓冲切换 → 下一 VSYNC 时 GLCDC 开始扫描新帧     │
  │     write_idx = 1 - write_idx  (行 454) 乒乓切换         │
  └─────────────────────────────────────────────────────────┘
}
```

### 5.4 帧同步信号机制

摄像头任务 (优先级 4) 和人脸检测任务 (优先级 2) 之间通过 FreeRTOS 二进制信号量通信：

```
摄像头任务 (cam_lcd, prio=4)              人脸检测任务 (face_det, prio=2)
  │                                           │
  ├─ 新帧就绪                                  │ (阻塞等待)
  ├─ face_detection_signal_new_frame(addr) ──→ xSemaphoreGive(g_frame_ready_sem)
  │  (face_detection_task.c:115-121)           │
  │                                           ├─ xSemaphoreTake() 返回
  │                                           ├─ 跳帧判断 (每10帧推理1次)
  │                                           ├─ g_npu_inferencing = true
  │                                           ├─ 预处理 (读 VIN 帧缓冲)
  │                                           ├─ NPU 推理 (~8ms)
  │                                           ├─ 后处理
  │                                           ├─ g_npu_inferencing = false
  │                                           └─ 更新 g_face_detection_results[]
  │                                               g_face_detection_count
  │ (继续下一帧循环，读 g_face_detection_results)
  ├─ draw_face_boxes(fb)
  └─ R_GLCDC_BufferChange
```

**关键设计点**：
- `g_npu_inferencing` 标志在预处理开始前设为 true（`face_detection_task.c` 行 311），在后处理完成后设为 false（行 325）。这覆盖了整个 SDRAM 访问窗口。
- 摄像头任务在写帧缓冲前检查此标志（`mipi_camera_lcd.c` 行 398），若为 true 则跳过本帧显示，防止 NPU 和 GLCDC 同时竞争 SDRAM。

---

## 6. 第五阶段：人脸检测推理循环

### 6.1 帧信号触发

**文件**: `src/ai_application/face_detection_task.c` — 行 115-121

```c
void face_detection_signal_new_frame(uint32_t frame_addr) {
    g_latest_frame_addr = frame_addr;   // 记录帧在 SDRAM 中的地址
    if (g_frame_ready_sem) {
        xSemaphoreGive(g_frame_ready_sem);  // 唤醒 AI 任务
    }
}
```

### 6.2 推理主循环

**文件**: `src/ai_application/face_detection_task.c` — 行 287-349

```c
while (1) {
    // [行 289] 等待新帧信号，超时 500ms
    if (xSemaphoreTake(g_frame_ready_sem, pdMS_TO_TICKS(500)) == pdTRUE) {

        // [行 290-292] ★跳帧★ 每 10 帧推理 1 次 (30fps → 3fps 推理)
        infer_count++;
        if (infer_count % INFER_EVERY_N_FRAMES != 0) continue;

        uint32_t frame_addr = g_latest_frame_addr;

        // [行 311-312] ★设置忙标志★ 防止摄像头任务写帧缓冲
        g_npu_inferencing = true;
        __DSB();  // 数据同步屏障 — 确保标志对其他总线主设备可见

        // [行 315-319] ① 预处理
        face_detect_preprocess_rgb565_to_int8(
            (const void *)frame_addr,          // 输入: VIN 帧缓冲 (640×480 RGB565)
            g_face_detect_input_buffer,        // 输出: INT8 灰度图缓冲 (192×192)
            CAMERA_INPUT_WIDTH,                // 640
            CAMERA_INPUT_HEIGHT,               // 480
            AI_INPUT_IMAGE_WIDTH,              // 192
            AI_INPUT_IMAGE_HEIGHT);            // 192

        // [行 322] ②③ 推理 + 后处理 (C++ 函数)
        int ret = face_detect_run_inference();

        // [行 325-326] ★清除忙标志★
        g_npu_inferencing = false;
        __DSB();

        // [行 329-335] 统计检测结果数量
        uint32_t count = 0;
        for (uint32_t i = 0; i < AI_MAX_DETECTION_NUM; i++) {
            if (g_face_detection_results[i].w > 0 && g_face_detection_results[i].h > 0)
                count++;
        }
        g_face_detection_count = count;  // 摄像头任务读取此值判断是否有检测结果
    }
}
```

### 6.3 预处理：RGB565 → INT8 灰度图

**文件**: `src/ai_application/face_detection_preprocess.c` — 行 16-58

```
face_detect_preprocess_rgb565_to_int8()
  │
  │  输入: 640×480 RGB565
  │  输出: 192×192 INT8 灰度 [-128, 127]
  │
  ├─ [行 33] crop_offset = (640 - 480) / 2 = 80
  │          中心裁剪: 跳过左右各 80 像素 → 480×480 正方形区域
  │
  ├─ [行 35-55] 双循环 192×192，最近邻采样:
  │
  │  for y in 0..191:
  │    y_offset = 640 * (480 * y / 192)    ← 垂直方向的源像素偏移
  │    for x in 0..191:
  │      x_offset = (480 * x / 192)        ← 水平方向的源像素偏移
  │      pixel = src[crop_offset + y_offset + x_offset]
  │
  ├─ [行 48-50] RGB565 → 灰度近似:
  │    R = pixel >> 11          (5 bits)
  │    G = (pixel >> 4) & 0x3F  (6 bits)
  │    B = pixel & 0x1F          (5 bits)
  │    gray = R*0.25 + G*0.5 + B*0.125
  │         = (R<<1) + (G & 0x3E ?) + B
  │         实现: ((input >> 11) << 1) + ((input >> 4) & 0x7E) + (input & 0x1F)
  │
  └─ [行 53] 映射到 INT8: output = gray - 128
```

坐标映射链：

```
原始摄像头帧: 640×480 RGB565
  ↓ 中心裁剪 (跳过左右各80像素)
裁剪区域: 480×480 (正方形)
  ↓ 最近邻降采样 (因子 480/192 = 2.5)
AI 输入: 192×192 INT8 灰度
```

### 6.4 NPU 推理

#### 6.4.1 推理入口：face_detect_run_inference()

**文件**: `src/ai_application/face_detection_main.cc` — 行 74-140

```c
int face_detect_run_inference(void)
{
    // [行 82] ★Step 1★ 复制预处理后的图像到 NPU Arena
    memcpy(mera_input_ptr(),                       // 目标: arena + input_offset
           g_face_detect_input_buffer,             // 源:   INT8 灰度缓冲
           AI_INPUT_IMAGE_SIZE);                   // 大小: 36,864 B (192×192)

    // [行 85] ★Step 2★ 运行 NPU 推理
    mera_invoke();  // → RunModel(false) → sub_0000_invoke(false)
                    //    内部调用 ethosu_invoke_v3()

    // [行 88-89] ★Step 3★ 获取两个输出张量指针
    int8_t *output0 = (int8_t *)mera_output1_ptr();  // 6×6 分支 (stride-32)
    int8_t *output1 = (int8_t *)mera_output2_ptr();  // 12×12 分支 (stride-16)

    // [行 92-107] ★Step 4★ 构造带量化参数的 TfLiteTensor
    // 量化参数 (行 54-63，编译期初始化，避免 DSE 优化消除):
    //   output0: scale=0.13408,  zero_point=47
    //   output1: scale=0.18536,  zero_point=10
    outputTensor0.quantization.params = &q_out0;
    outputTensor1.quantization.params = &q_out1;

    // [行 110-123] ★Step 5★ 后处理: YOLO 解码 + NMS
    DetectorPostProcess postProcess(&outputTensor0, &outputTensor1,
                                     results, postProcessParams);
    postProcess.DoPostProcess();

    // [行 126-137] ★Step 6★ 结果写回全局数组
    for (i = 0; i < AI_MAX_DETECTION_NUM; i++)
        face_detect_update_result(i, 0, 0, 0, 0);  // 清空旧结果
    for (i = 0; i < results.size(); i++)
        face_detect_update_result(i, results[i].m_x0, results[i].m_y0,
                                     results[i].m_w, results[i].m_h);
}
```

#### 6.4.2 RunModel → sub_0000_invoke（实际 NPU 调用）

**文件**: `src/ai_application/model/model.c` — 行 80-89

```c
void RunModel(bool clean_outputs) {
    // 99% 以上工作在 NPU 上执行，CPU 仅做调度
    sub_0000_invoke(clean_outputs);
}
```

**文件**: `src/ai_application/model/sub_0000_invoke.c` — 行 60-159

这是整个推理流程中最底层的函数，直接与 Ethos-U NPU 驱动交互：

```
sub_0000_invoke(clean_outputs=false)
  │
  ├─ [行 65-66] 获取模型权重和命令流指针（已在 SDRAM 中）
  │   model_data = face_detect_get_model_data()    → 0x68570000
  │   model_data_size = 422,048
  │
  ├─ [行 68] sub_0000_fast_scratch = sub_0000_arena  → 0x68500000
  │
  ├─ [行 76-77] 获取命令流指针 (也在 SDRAM)
  │   cms_data = face_detect_get_command_stream()  → 0x685670A0
  │
  ├─ [行 81-104] 配置 6 个基地址区域 (base_addrs[]):
  │   [0]: 模型权重      0x68570000  422,048 B
  │   [1]: Arena 区域0   0x68500000  442,368 B
  │   [2]: Arena 区域1   0x68500000  442,368 B
  │   [3]: 输入张量区域   0x68524000   36,864 B  (arena + 147456)
  │   [4]: 输出张量0区域  0x68505580    2,592 B  (arena + 21888)
  │   [5]: 输出张量1区域  0x68500D80      648 B  (arena + 3456)
  │
  ├─ [行 111-121] ★关键★ 修正优化器 ID
  │   hw_id = NPU 硬件硅片版本 (arch 1.66)
  │   opt_id = 命令流中记录的编译目标版本 (arch 1.0)
  │   若 hw_id ≠ opt_id → 将命令流中的 opt_id 替换为 hw_id
  │   然后 D-Cache Clean 该 4 字节 (确保 NPU 读到修正后的值)
  │
  ├─ [行 135-138] ★关键★ D-Cache Clean 输入张量区域
  │   SCB_CleanDCache_by_Addr(arena + 147456, 36864)
  │   仅清理 CPU 写入的 36KB 输入区域（而非整个 442KB arena）
  │   优化：比全 arena clean 减少 12× Cache 写回流量
  │
  ├─ [行 144-145] ★核心★ NPU 推理
  │   ethosu_invoke_v3(&g_ethosu0,    // NPU 驱动句柄
  │                     cms_data,      // 命令流指针
  │                     cms_size,      // 命令流大小 (11,252 B)
  │                     base_addrs,    // 6 个基地址
  │                     base_addrs_size, // 6 个基地址区域大小
  │                     6,             // 基地址数量
  │                     NULL);
  │   // 此调用阻塞 ~8ms，NPU 通过 AXI 直接读 SDRAM 中的权重和输入
  │
  └─ [行 148-152] ★关键★ D-Cache Invalidate 输出张量区域
      SCB_InvalidateDCache_by_Addr(arena + 21888, 2592)   // 输出0
      SCB_InvalidateDCache_by_Addr(arena + 3456, 648)     // 输出1
      // NPU 通过 AXI 写 SDRAM → D-Cache 中的这些地址已过期
      // 必须 Invalidate 使 CPU 从 SDRAM 重新读取最新数据
```

#### 6.4.3 D-Cache 一致性完整流程

NPU 与 CPU 共享 SDRAM 但**不共享 D-Cache**，必须手动维护一致性：

| 时机 | 操作 | 位置 | 原因 |
|------|------|------|------|
| 启动时 | Clean 模型权重区域 | `face_detection_task.c:177` | CPU 刚从 Flash 读入 SDRAM，Cache 中为新数据 |
| 启动时 | Clean 命令流区域 | `face_detection_task.c:179` | 同上 |
| 每次推理前 | Clean 输入张量区域 | `sub_0000_invoke.c:136` | CPU 通过 memcpy 写入了预处理图像 |
| 每次推理后 | Invalidate 输出张量0 | `sub_0000_invoke.c:149` | NPU 写入了检测结果 |
| 每次推理后 | Invalidate 输出张量1 | `sub_0000_invoke.c:150` | NPU 写入了检测结果 |
| 每帧采集后 | Invalidate VIN 帧缓冲 | `mipi_camera_lcd.c:348` | VIN DMA 写入了新帧 |
| 每帧渲染后 | Clean 帧缓冲区域 | `mipi_camera_lcd.c:439` | CPU 写入了像素数据 |
| 命令流修正后 | Clean 修正的 4 字节 | `sub_0000_invoke.c:117` | CPU 覆写了 opt_id |

### 6.5 后处理：反量化 → YOLO 解码 → NMS

**文件**: `src/ai_application/DetectorPostProcessing.cc` — 行 73-123

```
DoPostProcess()
  │
  ├─ [行 76-80] GetNetworkBoxes():
  │   对两个输出分支 (6×6 stride-32 和 12×12 stride-16) 分别:
  │   ① INT8 → Float 反量化: value = (int8_val - zero_point) × scale
  │      分支0: scale=0.13408, zp=47
  │      分支1: scale=0.18536, zp=10
  │   ② Sigmoid 激活 (置信度/类别概率)
  │   ③ YOLO 解码: 将 (tx, ty, tw, th) 映射到图像坐标
  │      bx = sigmoid(tx) × 2 - 0.5 + cx
  │      by = sigmoid(ty) × 2 - 0.5 + cy
  │      bw = anchor_w × (sigmoid(tw) × 2)²
  │      bh = anchor_h × (sigmoid(th) × 2)²
  │   ④ 过滤: 置信度 < 0.3 的框丢弃
  │
  ├─ [行 83] CalculateNMS():
  │   非极大值抑制:
  │   对所有候选框按置信度降序排列
  │   贪心合并: IoU > 0.45 的低分框被抑制
  │   每个类别独立执行
  │
  └─ [行 85-119] 结果组装:
      将保留的框转换为中心+宽高 → 左上+右下
      限制到图像边界内 (0..192)
      写入 results vector
```

YOLO 锚框（`face_detection_main.cc` 行 44-45）：

```c
// 6×6 分支 (stride=32): 大物体锚框
static const float anchor1[] = {38, 77, 47, 97, 61, 126};
// 12×12 分支 (stride=16): 小物体锚框
static const float anchor2[] = {14, 26, 19, 37, 28, 55};
```

量化参数的关键注意事项（`face_detection_main.cc` 行 48-63）：

量化结构体**必须编译期初始化**（放在 `.data` 段），不能运行时赋值。原因：编译器 DSE（Dead Store Elimination）优化可能消除运行时写入（compiler 无法追踪 `DetectorPostProcess` 通过 opaque 指针的后续读取）。

---

## 7. 第六阶段：检测结果渲染到 LCD

### 7.1 边界框坐标映射

**文件**: `src/driver/mipi_camera/mipi_camera_lcd.c` — 行 133-160

```
draw_face_boxes(fb)
  │
  │  坐标映射链 (4 步转换):
  │
  ├─ [行 140] ai_to_crop = 480.0 / 192.0 = 2.5
  │            AI 坐标空间 (192×192) → 裁剪区域 (480×480)
  │
  ├─ [行 141] crop_x_off = (640 - 480) / 2 = 80
  │
  ├─ [行 148-151] AI → 摄像头坐标:
  │   cam_x = ai_x × 2.5 + 80
  │   cam_y = ai_y × 2.5 + 0
  │   cam_w = ai_w × 2.5
  │   cam_h = ai_h × 2.5
  │
  ├─ [行 154-155] 摄像头 → LCD 坐标:
  │   lcd_x = LCD_X_OFF(0) + cam_x
  │   lcd_y = LCD_Y_OFF(60) + (479 - (cam_y + cam_h))   ← 垂直翻转
  │
  └─ [行 157] draw_rect_rgb565(fb, lcd_x, lcd_y, cam_w, cam_h)
      画绿色 (0x07E0) 单像素宽矩形边框
```

坐标映射全链路：

```
AI 输出 (192×192)
  ↓ ×2.5 (480/192)
裁剪区域 (480×480)
  ↓ +80 X偏移
摄像头帧 (640×480)
  ↓ LCD_X_OFF=0, LCD_Y_OFF=60, 垂直翻转
LCD 显示 (1024×600, Layer 1 左半边)
```

### 7.2 绘制函数

**文件**: `src/driver/mipi_camera/mipi_camera_lcd.c` — 行 98-117

```c
static void draw_rect_rgb565(uint16_t *fb, int x, int y, int w, int h) {
    const uint16_t color = 0x07E0;  // RGB565 纯绿色
    // 先裁剪到帧缓冲边界 (防止越界写)
    if (x < 0) { w += x; x = 0; }
    // ...
    // 画四条边: 上/下/左/右，每条边一个像素宽
    // 上边: for i in 0..w: fb[y * W + x + i] = green
    // 下边: for i in 0..w: fb[(y+h-1) * W + x + i] = green
    // 左边: for i in 0..h: fb[(y+i) * W + x] = green
    // 右边: for i in 0..h: fb[(y+i) * W + x + w - 1] = green
}
```

---

## 8. 附录：关键数据流与内存布局

### 8.1 SDRAM 内存布局（CPU0 16MB: 0x68000000–0x68FFFFFF）

```
0x68000000 ┌────────────────────────────┐
           │ VIN DMA 缓冲 (3×614,400B)  │  OV5640 摄像头原始帧
0x681C2080 ├────────────────────────────┤
           │ fb_background[0]           │  GLCDC Layer 1 帧缓冲0 (1024×600×2)
0x682E4100 ├────────────────────────────┤
           │ fb_background[1]           │  GLCDC Layer 1 帧缓冲1 (1024×600×2)
0x68406180 ├────────────────────────────┤
           │ (空闲)                      │
0x68500000 ├────────────────────────────┤
           │ NPU Tensor Arena           │  442,368 B
           │  ├─ 输入张量 @ +0x24000    │  (36,864 B)
           │  ├─ 输出0 @ +0x5580        │  (2,592 B)
           │  └─ 输出1 @ +0xD80         │  (648 B)
0x6856C000 ├────────────────────────────┤
           │ (对齐间隙)                  │
0x68570000 ├────────────────────────────┤
           │ YOLO 模型权重 (INT8)        │  422,048 B
0x685D70A0 ├────────────────────────────┤
           │ NPU 命令流                  │  11,252 B
0x685DA000 ├────────────────────────────┤
           │ (空闲)                      │
0x68600000 ├────────────────────────────┤
           │ Embed NPU Arena            │  144 KB (人脸特征模型)
0x68630000 ├────────────────────────────┤
           │ Embed 模型权重              │  ~721 KB
0x686E6000 ├────────────────────────────┤
           │ (空闲)                      │
0x68FFFFFF └────────────────────────────┘
```

### 8.2 FreeRTOS 任务优先级布局

```
优先级 4 (最高): cam_lcd          — 摄像头采集+LCD显示 (不能掉帧)
优先级 3:        lvgl             — LVGL UI 渲染 (需要响应触摸)
优先级 2:        face_det         — 人脸检测推理 (在空闲时运行)
优先级 2:        enrol_worker     — 人脸录入 Flash 写入
优先级 2:        clear_db_worker  — 人脸库清除
优先级 1 (最低): rec_stat         — CPU1 录制状态中继 (低优先级后台)
```

### 8.3 关键文件速查表

| 阶段 | 文件 | 关键函数/行号 |
|------|------|-------------|
| 编译 | `run_compile2.bat` | 全文件 |
| 编译 | `deploy_cfg.json` | 配置项 `enable_ethos=true` |
| 部署 | `scripts/pcdc_flash_tool.py` | 全文件 |
| 配置 | `face_detection_config.h` | 所有 #define 常量 |
| 配置 | `w25q64_partition.h` | Flash 分区偏移量 |
| 启动 | `cpu0main_thread_entry.c` | `face_embedding_init()` 行 330, `mipi_camera_lcd_start()` 行 347 |
| 启动 | `face_detection_task.c` | `load_model_from_flash()` 行 127-196 |
| 启动 | `face_detection_task.c` | NPU AXI 限流器 行 260-274 |
| 帧同步 | `face_detection_task.c` | `face_detection_signal_new_frame()` 行 115-121 |
| 帧同步 | `mipi_camera_lcd.c` | 主循环 行 331-463 |
| 帧同步 | `mipi_camera_lcd.c` | D-Cache Invalidate 行 348 |
| 预处理 | `face_detection_preprocess.c` | `face_detect_preprocess_rgb565_to_int8()` 行 16-58 |
| 推理 | `face_detection_main.cc` | `face_detect_run_inference()` 行 74-140 |
| 推理 | `model.c` | `RunModel()` 行 80-89 |
| 推理 | `wrapper.h` | `mera_invoke()` 行 24-27 |
| 推理 | `sub_0000_invoke.c` | `sub_0000_invoke()` 行 60-159 |
| 推理 | `sub_0000_invoke.c` | `ethosu_invoke_v3()` 行 144-145 |
| 推理 | `sub_0000_invoke.c` | D-Cache Clean 输入 行 136 |
| 推理 | `sub_0000_invoke.c` | D-Cache Invalidate 输出 行 149-150 |
| 后处理 | `DetectorPostProcessing.cc` | `DoPostProcess()` 行 73-123 |
| 后处理 | `DetectorPostProcessing.cc` | `GetNetworkBoxes()` 行 80 |
| 后处理 | `DetectorPostProcessing.cc` | `CalculateNMS()` 行 83 |
| 后处理 | `face_detection_main.cc` | 量化参数初始化 行 54-63 |
| 后处理 | `face_detection_main.cc` | YOLO 锚框 行 44-45 |
| 渲染 | `mipi_camera_lcd.c` | `draw_face_boxes()` 行 133-160 |
| 渲染 | `mipi_camera_lcd.c` | `draw_rect_rgb565()` 行 98-117 |
| 渲染 | `mipi_camera_lcd.c` | 坐标映射 行 140-157 |

### 8.4 常见问题排查

| 问题 | 症状 | 根因位置 |
|------|------|---------|
| 推理输出全零 | 检测不到任何人脸 | D-Cache 未 Clean 模型数据 (`face_detection_task.c:177`) |
| 量化参数错误 | 检测框位置/大小异常 | 运行时赋值被 DSE 消除 (`face_detection_main.cc:54-63`，必须编译期初始化) |
| 屏幕闪烁 | 画面撕裂/闪烁 | SDRAM 带宽超限 → AXI 限流器配置 (`face_detection_task.c:260-274`) |
| NPU open 失败 | ethosu_invoke_v3 返回 -1 | Arena 放在 SRAM（NPU 只能访问 SDRAM）(`sub_0000_invoke.c:48`) |
| 命令流被拒绝 | 推理不执行 | opt_id ≠ hw_id 未修正 (`sub_0000_invoke.c:111-121`) |
| 预处理输出错误 | 灰度值异常 | RGB565 位域提取错误 (`face_detection_preprocess.c:48-50`) |

---

*文档基于代码版本: 2026-08-06, commit a7ebd59 及后续未提交修改*
