# 人脸检测 AI 应用 (YOLO-Fastest + Ethos-U NPU)

## 概述

基于 Arm Ethos-U55-256 NPU 的实时人脸检测应用。使用 YOLO-Fastest 模型，输入 192×192 灰度图像，输出人脸检测框坐标。模型权重存储在 W25Q64 外部 Flash，启动时自动加载到 SDRAM 供 NPU 读取。

**硬件架构：**
- OV5640 → MIPI CSI-2 → VIN DMA → SDRAM (RGB565)
- CPU0：图像预处理 + NPU 推理 + YOLO 后处理
- CPU1：帧旋转 + 检测框绘制 + GLCDC 显示

**数据流：**
```
CPU0                                    CPU1
─────                                   ─────
OV5640 → VIN DMA → SDRAM (RGB565)
    │
    ├─→ face_detection_signal_new_frame()
    │       │
    │       ▼
    │   预处理: RGB565 → INT8 灰度 (192×192)
    │       │
    │       ▼
    │   NPU 推理: ethosu_invoke_v3()
    │       │
    │       ▼
    │   后处理: YOLO 解码 + NMS
    │       │
    │       ▼
    │   写入 cam_shmem_t ──────────→ 轮询 cam_shmem_t
    │   (帧地址 + 检测结果)              │
    │                                   ▼
    │                               旋转 90° + 画检测框
    │                                   │
    │                                   ▼
    │                               R_GLCDC_BufferChange → LCD
```

## 文件结构

```text
src/ai_application/
├── README.md                       # 本文档
├── face_detection_config.h         # 配置参数（AI 尺寸、SDRAM 地址、检测阈值）
├── face_detection_task.h           # FreeRTOS 任务头文件
├── face_detection_task.c           # 主任务：模型加载 + 推理循环
├── face_detection_main.cc          # C++ 推理入口：YOLO 后处理 + NMS
├── face_detection_preprocess.h     # 图像预处理头文件
├── face_detection_preprocess.c     # RGB565 → INT8 灰度预处理
├── DetectionResult.hpp             # 检测结果结构体
├── DetectorPostProcessing.cc       # YOLO 后处理实现
├── DetectorPostProcessing.hpp      # YOLO 后处理头文件
├── ethosu_dcache.c                 # NPU D-Cache 管理（覆盖 Ethos-U 驱动默认实现）
├── common/
│   ├── log_macros.h                # 日志宏（trace/debug/info/warn/error）
│   ├── ImageUtils.cc               # NMS/IOU 工具函数
│   ├── ImageUtils.hpp              # 图像工具头文件
│   ├── PlatformMath.cc             # 数学工具（Sigmoid、Softmax 等）
│   └── PlatformMath.hpp            # 数学工具头文件
└── model/                          # MERA 编译输出（Ethos-U NPU 模型）
    ├── model.c                     # RunModel()、输入/输出指针访问
    ├── model.h                     # 模型 API 声明
    ├── sub_0000_invoke.c           # NPU 调用（已适配 SDRAM 模型加载）
    ├── sub_0000_invoke.h           # NPU 调用声明
    ├── sub_0000_tensors.c          # 张量元数据（名称、大小、Arena 偏移）
    ├── sub_0000_tensors.h          # 张量声明
    ├── sub_0000_model_data.c       # 模型权重 C 数组（422KB，首次编程用）
    ├── sub_0000_model_data.h       # 模型数据声明
    ├── sub_0000_command_stream.c   # NPU 命令流（11KB）
    ├── sub_0000_command_stream.h   # 命令流声明
    ├── sub_0000_io_data.c          # 默认 I/O 数据（测试用）
    ├── sub_0000_io_data.h          # I/O 数据声明
    ├── model_io_data.c             # 默认 I/O 数据（测试用）
    ├── model_io_data.h             # I/O 数据声明
    ├── ethosu_common.h             # TensorInfo 结构定义
    └── wrapper.h                   # 便捷封装（mera_input_ptr 等）
```

## 模型信息

| 参数 | 值 |
| ---- | ---- |
| 模型 | YOLO-Fastest (人脸检测) |
| 编译器 | EdgeCortix Mera 2.6.0 |
| NPU | Arm Ethos-U55-256 |
| 输入 | 192×192×1 INT8 灰度 |
| 输出 0 | Identity_70275, 648 字节, scale=0.1341, zp=47 |
| 输出 1 | Identity_1_70284, 2592 字节, scale=0.1854, zp=10 |
| 模型权重 | 422,048 字节 (sub_0000_model_data) |
| NPU 命令流 | 11,252 字节 (sub_0000_command_stream) |
| Tensor Arena | 442,368 字节 |
| Anchor boxes | anchor1={38,77, 47,97, 61,126}, anchor2={14,26, 19,37, 28,55} |
| 类别数 | 1 (人脸) |

## 内存布局

| 区域 | 地址 | 大小 | 用途 |
| ---- | ---- | ---- | ---- |
| W25Q64 Flash | 0x000000 | ~433KB | 模型权重 + 命令流（持久存储） |
| OSPI 映射 | 0x90000000 | 8MB | W25Q64 内存映射读取 |
| SDRAM (模型) | 0x68100000 | 1MB | 启动时从 Flash 加载的模型数据 |
| SDRAM (Arena) | 由 .sdram 段分配 | 442KB | NPU tensor arena |
| SRAM | 由 .bss 段分配 | 36KB | 预处理输入缓冲区 |
| 共享内存 | 0x69DFF000 | ~256B | cam_shmem_t（帧地址 + 检测结果） |

## API 参考

### 1. 任务管理

#### `face_detection_task_start`

```c
void face_detection_task_start(void);
```

创建人脸检测 FreeRTOS 任务。任务启动后会自动：
1. 从 W25Q64 加载模型数据到 SDRAM
2. 打开 Ethos-U NPU 驱动
3. 进入推理主循环

**调用位置**：在 `mipi_camera_test_start()` 内部调用，无需手动调用。

> **注意**：此函数创建的任务优先级为 3，与摄像头采集任务相同。推理在摄像头帧间空闲时执行。

#### `face_detection_signal_new_frame`

```c
void face_detection_signal_new_frame(uint32_t frame_addr);
```

通知人脸检测任务有新帧可用。每帧摄像头采集完成后自动调用。

| 参数 | 说明 |
| ---- | ---- |
| `frame_addr` | VIN DMA 帧缓冲区的 SDRAM 地址 |

**调用位置**：在 `mipi_camera_test.c` 的采集循环中自动调用。

### 2. 检测结果

#### 全局变量

```c
/* 检测结果数组（AI 坐标空间 192×192） */
face_detect_result_t g_face_detection_results[AI_MAX_DETECTION_NUM];

/* 检测到的人脸数量 */
volatile uint32_t g_face_detection_count;
```

#### `face_detect_result_t` 结构

```c
typedef struct {
    int16_t x;   /* 检测框左上角 X（AI 空间 192×192） */
    int16_t y;   /* 检测框左上角 Y */
    int16_t w;   /* 检测框宽度 */
    int16_t h;   /* 检测框高度 */
} face_detect_result_t;
```

**读取示例：**

```c
#include "face_detection_task.h"

void process_results(void)
{
    for (uint32_t i = 0; i < g_face_detection_count; i++) {
        int16_t x = g_face_detection_results[i].x;
        int16_t y = g_face_detection_results[i].y;
        int16_t w = g_face_detection_results[i].w;
        int16_t h = g_face_detection_results[i].h;
        printf("Face %lu: x=%d y=%d w=%d h=%d\r\n", i, x, y, w, h);
    }
}
```

### 3. 图像预处理

#### `face_detect_preprocess_rgb565_to_int8`

```c
int face_detect_preprocess_rgb565_to_int8(
    const void *p_input,     /* 输入 RGB565 图像 */
    void *p_output,          /* 输出 INT8 图像 */
    uint16_t in_width,       /* 输入宽度 */
    uint16_t in_height,      /* 输入高度 */
    uint16_t out_width,      /* 输出宽度 (192) */
    uint16_t out_height      /* 输出高度 (192) */
);
```

将 RGB565 图像转换为 INT8 灰度图像，用于 NPU 推理。

**处理步骤：**
1. 裁剪中心正方形（如 640×480 → 480×480）
2. 最近邻缩放到输出尺寸（如 480×480 → 192×192）
3. RGB565 → 灰度（近似加权：R×0.25 + G×0.5 + B×0.125）
4. 转换为 INT8 范围（减 128，映射 0~255 → -128~127）

**返回值**：0 成功，-1 参数错误。

### 4. C++ 推理入口

#### `face_detect_run_inference`

```c
int face_detect_run_inference(void);  /* C 链接 */
```

执行完整的推理流程：拷贝输入到 Arena → NPU 推理 → YOLO 后处理 → 更新检测结果。

**调用位置**：在 `face_detection_task.c` 的主循环中自动调用，无需手动调用。

**返回值**：0 成功，-1 后处理失败。

### 5. 模型编程（首次使用）

#### `face_detection_program_model_to_flash`

```c
int face_detection_program_model_to_flash(void);
```

将编译在固件中的模型数据写入 W25Q64 Flash。**仅首次使用时需要调用**。

首次运行时，如果检测到 Flash 为空（全 0xFF），会自动调用此函数。后续启动直接从 Flash 加载。

> **重要**：调用此函数前，`sub_0000_model_data.c` 必须编译到固件中（约 2.5MB）。
> 编程完成后，应从构建中排除该文件以减小固件体积。

### 6. MERA 模型 API（底层）

#### `RunModel`

```c
void RunModel(bool clean_outputs);
```

执行 NPU 推理。`clean_outputs` 为 true 时在推理前清零输出缓冲区。

#### 输入/输出指针

```c
int8_t* GetModelInputPtr_image_input(void);         /* 输入张量指针 */
int8_t* GetModelOutputPtr_Identity_70275(void);     /* 输出 0 指针 (648B) */
int8_t* GetModelOutputPtr_Identity_1_70284(void);   /* 输出 1 指针 (2592B) */
```

#### 便捷封装（wrapper.h）

```c
uint8_t* mera_input_ptr(void);      /* = GetModelInputPtr_image_input() */
uint8_t* mera_output1_ptr(void);    /* = GetModelOutputPtr_Identity_70275() */
uint8_t* mera_output2_ptr(void);    /* = GetModelOutputPtr_Identity_1_70284() */
void     mera_invoke(void);         /* = RunModel(false) */
```

## 配置参数

### face_detection_config.h

| 宏 | 默认值 | 说明 |
| ---- | ---- | ---- |
| `AI_INPUT_IMAGE_WIDTH` | 192 | 模型输入宽度 |
| `AI_INPUT_IMAGE_HEIGHT` | 192 | 模型输入高度 |
| `AI_INPUT_IMAGE_BYTE_PER_PIXEL` | 1 | 灰度 = 1 |
| `AI_MAX_DETECTION_NUM` | 20 | 最大检测框数量 |
| `AI_DETECTION_THRESHOLD` | 0.5f | 置信度阈值 |
| `AI_NMS_THRESHOLD` | 0.45f | NMS IoU 阈值 |
| `AI_NUM_CLASSES` | 1 | 类别数（人脸） |
| `FACE_MODEL_FLASH_OFFSET` | 0x00000000 | 模型在 W25Q64 中的偏移 |
| `FACE_MODEL_SDRAM_ADDR` | 0x68100000 | 模型加载到 SDRAM 的地址 |
| `FACE_DETECT_TASK_STACK_SIZE` | 4096 | 任务栈大小 |
| `FACE_DETECT_TASK_PRIORITY` | 3 | 任务优先级 |

## 坐标变换

检测结果在 AI 坐标空间（192×192）中，显示时需要变换到 LCD 坐标空间（1024×600）。

### 变换公式

```
AI 空间 (192×192)  →  裁剪空间 (480×480)  →  相机空间 (640×480)  →  旋转后  →  LCD (1024×600)

步骤 1: AI → 裁剪空间 (×2.5)
    crop_x = ai_x × (480/192)
    crop_y = ai_y × (480/192)

步骤 2: 裁剪 → 相机空间 (+80 偏移)
    cam_x = crop_x + (640-480)/2
    cam_y = crop_y

步骤 3: 相机 → 旋转 90° CW
    rot_x = cam_y
    rot_y = 639 - cam_x

步骤 4: 旋转 → LCD 空间 (+居中偏移)
    lcd_x = rot_x + (1024-640)/2
    lcd_y = rot_y + (600-480)/2
```

CPU1 的 `mipi_camera_lcd.c` 中 `draw_detection_boxes()` 函数已实现此变换。

## e2studio 构建配置

### 1. 添加源码路径

在 e2studio 中，右键 CPU0 项目 → Properties → C/C++ Build → Settings → Compiler → Source，添加以下目录：
- `../src/ai_application`
- `../src/ai_application/common`
- `../src/ai_application/model`

### 2. 添加包含路径

Compiler → Include paths 添加：
- `../src/ai_application`
- `../src/ai_application/common`
- `../src/ai_application/model`
- `../ra/npu/tflite-micro`（TFLite 头文件，用于 `TfLiteTensor` 类型）

### 3. 排除大文件

首次模型编程完成后，右键以下文件 → Resource Configurations → Exclude from Build：
- `model/sub_0000_model_data.c`（2.5MB，模型权重数组）
- `model/sub_0000_io_data.c`（测试用默认 I/O 数据）
- `model/model_io_data.c`（测试用默认 I/O 数据）

这可以将固件大小减少约 2.9MB。

### 4. C++ 编译器

`.cc` 文件需要 C++ 编译器。e2studio 默认会根据文件扩展名选择编译器，通常无需额外配置。

## 依赖项

| 依赖 | 说明 |
| ---- | ---- |
| Renesas FSP | Ethos-U 驱动 (`rm_ethosu`)、OSPI_B、VIN、IIC |
| Arm Ethos-U 驱动 | `ethosu_invoke_v3()`、D-Cache 管理 |
| W25Q64 驱动 | Flash 读写（`src/driver/w25q64/`） |
| MIPI Camera 驱动 | 摄像头采集（`src/driver/mipi_camera/`） |
| FreeRTOS | 任务、信号量 |
| CMSIS | D-Cache 操作（`SCB_CleanDCache_by_Addr`） |
| TFLite Micro 头文件 | `TfLiteTensor` 类型定义（仅头文件，不链接 TFLite 运行时） |

## 首次使用流程

### 步骤 1：编译固件

确保 `model/sub_0000_model_data.c` **已包含**在构建中（不要排除）。

### 步骤 2：烧录并运行

首次启动时，控制台输出：
```
[FACE_DET] Task started
[FACE_DET] Loading model from W25Q64 to SDRAM...
[FACE_DET]   Flash appears empty (all 0xFF)
[FACE_DET] Programming model to W25Q64...
[FACE_DET] Model programmed successfully
[FACE_DET] Model loaded successfully
[FACE_DET] Ethos-U NPU opened
[FACE_DET] Ready for inference
```

### 步骤 3：排除大文件

模型编程完成后，在 e2studio 中排除 `sub_0000_model_data.c`、`sub_0000_io_data.c`、`model_io_data.c`，重新编译以减小固件体积。

### 步骤 4：后续启动

后续启动直接从 Flash 加载模型（~26ms）：
```
[FACE_DET] Task started
[FACE_DET] Loading model from W25Q64 to SDRAM...
[FACE_DET] W25Q64 ID: EF 40 17
[FACE_DET] Model loaded successfully
[FACE_DET] Ethos-U NPU opened
[FACE_DET] Ready for inference
```

## 模型存储链路

模型数据从编译到运行时推理，经历 4 个阶段、3 次存储介质转换。

### 阶段 1：编译时 — 模型嵌入固件

MERA 工具链将 TFLite 模型编译为 C 数组，编译后嵌入内部 Flash：

```
MERA 编译器 (PC)
  ↓
sub_0000_model_data.c:              sub_0000_command_stream.c:
  const uint8_t                       const uint8_t
    sub_0000_model_data[422048]         sub_0000_command_stream[11252]
    = { 0x01, 0x02, ... }              = { 0xAA, 0xBB, ... }
  ↓ 编译                                   ↓ 编译
内部 Flash (512KB):
  ┌──────────┬────────────────────┬─────────────┐
  │ 固件代码  │ 模型权重 422KB     │ 命令流 11KB  │
  │ ~100KB   │ (sub_0000_model_   │ (sub_0000_  │
  │          │  data[])           │  command_   │
  │          │                    │  stream[])  │
  └──────────┴────────────────────┴─────────────┘
```

### 阶段 2：首次启动 — 编程到 W25Q64

任务入口检测 Flash 为空（全 0xFF），自动触发编程流程：

```
face_detection_task_entry()
  │
  ├─ load_model_from_flash()
  │   ├─ w25q64_open()                         // 初始化 OSPI 控制器
  │   ├─ w25q64_read_jedec_id()                // 读 Flash ID: EF 40 17
  │   ├─ w25q64_read(0x000000, buf, 16)        // 读前 16 字节
  │   └─ 全部 == 0xFF → 返回 -2 (Flash 为空)
  │
  ├─ face_detection_program_model_to_flash()
  │   ├─ w25q64_erase_sector(0x000000)         // 擦除 4KB 扇区
  │   ├─ w25q64_erase_sector(0x001000)         // 循环 ~103 次
  │   ├─ ...
  │   ├─ w25q64_write(0x000000,                // 写入模型权重
  │   │                sub_0000_model_data,     //   数据来源: 内部 Flash const 数组
  │   │                422048)                  //   经 OSPI Quad Write → W25Q64
  │   └─ w25q64_write(0x67800,                 // 写入命令流 (紧跟权重之后)
  │                    sub_0000_command_stream,
  │                    11252)
  │
  └─ load_model_from_flash()  // 再次调用, 现在有数据了
```

编程完成后 W25Q64 布局：

```
地址        内容                  大小
──────────────────────────────────────────
0x000000    模型权重              422,048 B
0x067800    NPU 命令流            11,252 B
0x06A400    空闲                  ~7.6 MB
```

### 阶段 3：每次启动 — 从 W25Q64 加载到 SDRAM

后续启动跳过编程，直接从 Flash 拷贝到 SDRAM（~26ms）：

```
load_model_from_flash()
  │
  ├─ w25q64_read(0x000000, 0x68100000, 422048)
  │   │
  │   │  W25Q64 Flash (0x000000)
  │   │    ↓
  │   │  OSPI 控制器: 内存映射读 (0x90000000 + offset)
  │   │    ↓
  │   │  memcpy → SDRAM 0x68100000
  │   │
  │   └─ SDRAM 布局:
  │       0x68100000: [模型权重 422KB]
  │       0x6867800:  [NPU 命令流 11KB]  (g_command_stream_sdram)
  │
  └─ 返回 0 (成功)
```

### 阶段 4：推理时 — NPU 从 SDRAM 读取

`ethosu_invoke_v3()` 通过 AXI 总线直接访问 SDRAM：

```
ethosu_invoke_v3() 的 6 个 base_addrs:

[0] 0x68100000 (SDRAM)        ← 模型权重 + 权重张量, 422KB
[1] sub_0000_arena + 0 (SDRAM) ← Tensor Arena, 442KB
[2] sub_0000_arena + 0 (同上)  ← Fast scratch (复用 arena)
[3] sub_0000_arena + 147456    ← 输入张量 (192×192×1 INT8), 36KB
[4] sub_0000_arena + 21888     ← 输出 0 (Identity_70275), 2592B
[5] sub_0000_arena + 3456      ← 输出 1 (Identity_1_70284), 648B
```

### 完整数据流总览

```
编译时                    首次启动                    每次启动                 推理时
─────────                ──────────                  ──────────              ──────────
MERA 工具链              Flash 编程                  Flash → SDRAM           NPU 读取
    ↓                        ↓                           ↓                       ↓
sub_0000_model_data[]    w25q64_erase_sector()      w25q64_read()           AXI 总线
(内部 Flash const)       w25q64_write()             memcpy                  ↓
    ↓                    ↓                          ↓                       SDRAM
烧录到内部 Flash         W25Q64 Flash               SDRAM 0x68100000        ↓
    ↓                    ↓                          ↓                       推理结果
固件 ELF               持久存储                    NPU 可访问
                       (掉电不丢失)                (快速随机读)
```

## 调试技巧

### 无检测框显示

1. 检查控制台是否输出 `[FACE_DET] Ready for inference`
2. 检查摄像头是否正常采集（`[CAM TEST] frame#` 日志）
3. 降低 `AI_DETECTION_THRESHOLD`（如 0.3）测试
4. 确认人脸在摄像头视野内且光线充足

### 检测框位置偏移

1. 确认 `face_detection_config.h` 中的 `AI_INPUT_IMAGE_WIDTH/HEIGHT` 与模型匹配
2. 确认 CPU1 `mipi_camera_lcd.c` 中的坐标变换参数与摄像头分辨率一致

### 推理速度慢

1. 确认 D-Cache 已启用（`BSP_CFG_DCACHE_ENABLED = 1`）
2. 确认 Arena 在 SDRAM 中（不在 OSPI Flash）
3. 检查 `face_detection_config.h` 中的 SDRAM 地址是否与其他分配冲突

### 模型加载失败

1. 检查 W25Q64 连接（`[FACE_DET] W25Q64 ID: EF 40 17`）
2. 如果 Flash 数据损坏，擦除后重新编程：
   ```c
   w25q64_erase_chip();
   face_detection_program_model_to_flash();
   ```

## 已知限制

1. **推理延迟**：YOLO-Fastest 在 Ethos-U55-256 上的推理时间约 30-50ms，加上预处理和后处理，整体帧率约 15-20fps。

2. **检测精度**：YOLO-Fastest 是轻量级模型，对小人脸、侧脸、遮挡等情况的检测精度有限。如需更高精度，可替换为更大的模型（需要重新通过 RUHMI 转换）。

3. **模型与固件绑定**：当前模型编译在 `sub_0000_model_data.c` 中。如果更换模型，需要重新通过 RUHMI 工具链生成 MERA 代码并替换 `model/` 目录下的文件。

4. **单次编程**：模型数据写入 W25Q64 后，除非手动擦除，否则会一直保留。如果需要更新模型，需要先擦除对应扇区再重新编程。

## 移植到其他模型

如需使用其他模型（如 YOLOv5n）替换当前的 YOLO-Fastest：

1. 通过 RUHMI 工具将 `.tflite` 模型编译为 MERA C 代码
2. 替换 `model/` 目录下的所有文件
3. 更新 `face_detection_config.h` 中的参数（输入尺寸、anchor boxes 等）
4. 更新 `face_detection_main.cc` 中的量化参数（scale、zero_point）
5. 更新 `face_detection_main.cc` 中的 anchor box 数组
6. 重新编程模型到 W25Q64：`face_detection_program_model_to_flash()`
