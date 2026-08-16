# Changelog — Titan-mini RA8P1 智能会议交互终端

本文件记录从 Phase 1 基础搭建到打卡功能实现的每一轮改动。

---

## Round 1 — 基础搭建 (Phase 1)

### 新增功能
- **双图层 GLCDC 显示**：Layer 1 = 摄像头画面 (1024×600, 左半边)，Layer 2 = LVGL UI 面板 (384×600 @ x=640, 右半边)
- **开机静态 Logo**：`boot_logo.c/h`，从 W25Q64 资产目录读取 "logo" 并全屏显示，替换原动画
- **LVGL UI 骨架**：4 个英文按钮（Enroll Face / Check-in / Start Rec / Stop Rec）+ 状态/结果标签，Montserrat 14 字体
- **RPMsg 录制控制通道**：`rpmsg_record.h` (CPU0+CPU1 共享)、CPU0 发送端 + 状态接收任务、CPU1 接收端 + 调度任务
- **CPU1 stdout 支持**：`syscalls_stubs.c` — tinystdio stdout 重定向到 RPMsg log，行缓冲避免 `[CPU1]` 前缀垃圾

### Bug 修复
- **删除了声源定位 (GCC-PHAT) 代码**：移除 `src/algorithm/sound_localization/` 及 `.cproject` 中的 include 路径
- **CPU1 编译错误**：移除 `arm_math.h`(M33 无 CMSIS-DSP)、修正 `av_recorder_get_info` 返回值类型、补充 `av_sync.h` 缺失头文件、修正 `pdm_callback` static 冲突、修正 `PDM_EVENT_RECEIVE_COMPLETE` → `PDM_EVENT_DATA`
- **FreeRTOS 堆 64KB→128KB**：双图层模式下新增 LVGL 任务 + REC 状态任务，原 64KB 不足
- **`R_BSP_SecondaryCoreStart()` 重新启用**

### 涉及文件
`cpu0main_thread_entry.c`, `boot_logo.c/h`, `lvgl_ui_main.c/h`, `lvgl_ui_face_stubs.c`, `rpmsg_record.h`, `rpmsg_record_cpu0.c/h`, `rpmsg_record_cpu1.c/h`, `cpu1main_thread_entry.c`, `syscalls_stubs.c`, `mjpeg_encoder.c`, `av_sync.h`, `pdm_audio.c`, `FreeRTOSConfig.h`

---

## Round 2 — 闪屏与触摸修复

### Bug 修复
- **LVGL 面板在开机 Logo 期间出现**：`lvgl_ui_init()` 移到 `boot_logo_show_layer1()` **之后**调用；`boot_logo_show_layer1()` 同时填充 Layer 1 (`fb_background`) 和 Layer 2 (`fb_foreground`) 实现**全屏 Logo**
- **GT911 触摸无响应**：触摸坐标 x 偏移 `-640`（GT911 返回绝对屏幕坐标 0-1023，LVGL Layer 2 区域在 x=640..1023）
- **LVGL 任务优先级 2→3**：避免被 Face Detection 任务(优先级 2)饿死
- **SDRAM 带宽不足导致严重闪屏**：FSP `g_display0` → `clock_div_ratio` 8→**12**，降低 GLCDC 像素时钟，释放 SDRAM 带宽余量
- **旋转循环低效**：逐像素 `uint16_t` 循环 → `uint32_t` 字拷贝 (320 次/行 vs 640 次/行)，减少 SDRAM 事务量
- **诊断计数器**：`skip_npu` / `late_vsync` 每 100 帧输出到 RTT

### 涉及文件
`boot_logo.c`, `lvgl_ui_main.c`, `mipi_camera_lcd.c`, `cpu0main_thread_entry.c`

---

## Round 3 — 打卡功能与 UI 重构

### 新增功能
- **人脸特征数据库 (face_db)**：`face_db.c/h`，W25Q64 64KB 专用分区 (0x7F0000)，最多 32 人，128 维浮点 embedding + 24 字节 ASCII 名字，余弦相似度匹配 (阈值 0.45)
- **LVGL UI 全面重写**：
  - 2×2 紧凑按钮布局 + 日志窗口 (textarea，自动滚动) + 录制计时器 (MM:SS) + 状态栏
  - 录入流程：键盘弹窗 (textarea + LVGL 键盘 + Save 按钮)，过滤控制字符
  - **Clear DB** 按钮：清除所有人脸特征
- **本地录制计时器**：LVGL 任务循环内独立计时，不依赖 CPU1 RPMsg 状态回报即可即时显示
- **伪 embedding 生成**：用当前人脸检测框坐标生成确定性向量，用于测试完整录入→匹配流程 (待 MobileFaceNet 真实模型替换 `get_face_embedding()`)

### Bug 修复
- **face_db Flash 写入卡死**：`face_db_flush()` 在 erase/write 前 close+reopen OSPI_B，避免 XIP 模式冲突
- **LVGL 事件回调内 Flash 写入导致卡死**：改为 **Save 按钮 + 独立 FreeRTOS worker 任务** (优先级 2) 执行 Flash 写入，LVGL 任务完全不阻塞
- **键盘 Enter 事件删除父容器崩溃**：移除 LV_EVENT_READY 事件回调，改用显式 Save 按钮
- **名字保存附带控制字符**：保存前自动过滤前导/尾随空格、`\n`、`\r` 及 <32 的控制字符
- **W25Q64 分区**：`w25q64_partition.h` 新增 `PART_FACE_DB_OFFSET` (0x7F0000) / `PART_FACE_DB_SIZE` (60KB)

### 涉及文件
`face_db.c/h`, `lvgl_ui_main.c/h`, `lvgl_ui_face_stubs.c`, `rpmsg_record_cpu0.c`, `cpu0main_thread_entry.c`, `w25q64_partition.h`

---

## 当前状态

| 功能 | 状态 | 备注 |
|------|------|------|
| 开机全屏 Logo | ✅ | Layer 1 + Layer 2 拼接 |
| 摄像头预览 (左) | ✅ | clock_div_ratio=12 后无闪屏 |
| LVGL UI 面板 (右) | ✅ | 触摸正常，英文界面 |
| 人脸检测 (NPU) | ✅ | FACE_DETECTION_ENABLE=1 |
| 人脸录入 (Enroll) | ✅ | 伪 embedding，写入 W25Q64 |
| 人脸打卡 (Check-in) | ✅ | 余弦匹配，日志显示名字 |
| 清除人脸库 | ✅ | Clear DB 按钮 |
| 录制控制 (RPMsg) | ✅ | Start/Stop 发送到 CPU1 |
| 录制计时器 | ✅ | 本地计时，即时响应 |
| **真实人脸特征模型** | ❌ | 待 RUHMI 编译 MobileFaceNet |
| **音视频录制 (CPU1)** | ❌ | SD+PDM+WAV+MJPEG+AVI |

---

---

## Round 4 — 真实人脸特征模型 (RUHMI 编译)

### 新增
- **人脸特征模型**：MobileNetV2 0.35× (ImageNet 预训练骨干)，96×96×3 输入，128-d 输出
  - 模型参数：574,048 (~574K)
  - 100% NPU 执行 (66/66 算子)
  - INT8 权重 ~728 KB + 命令流 ~43 KB → W25Q64 768KB 分区
  - SRAM Arena: 144 KB
  - 预估推理时间: <1ms
- **W25Q64 分区**：`PART_EMBED_MODEL` (0x720000, 768KB) + `PART_EMBED_CMDSTREAM` (0x7E0000, 64KB)
- **编译产物**：`src/ai_application/embed_model/` (sub_0001_* 文件)
- **构建脚本**：`src/mobilefacenet/build_tiny_model.py` — 从零构建模型 + TFLite 转换
- **编译脚本**：`run_compile_mfn.bat` — RUHMI 一键编译
- **vela.bat**：修复 RUHMI venv 中 Vela 编译器 PATH 问题

### 遇到并解决的问题
- ONNX 模型动态 batch size → `-b 1` 固定批量
- Vela 不在 PATH → 创建 `vela.bat` 包装器
- TFLite FULLY_CONNECTED v12 不兼容 MERA → 替换为 Conv2D 1×1
- Keras Lambda 层不兼容 MERA → L2 归一化改在 CPU 后处理
- BatchNorm 常量 INT8/FP32 类型不匹配 → 显式 `tf.cast(w, tf.float32)`
- `w600k_mbf` 模型 INT8 19MB 太大 → 换用 MobileNetV2 0.35× (~728KB)

### 涉及文件
`build_tiny_model.py`, `run_compile_mfn.bat`, `quant_config.json`, `w25q64_partition.h`, `embed_model/`, `face_db.h`, `lvgl_ui_face_stubs.c`, `vela.bat`

---

## Round 5 — 真实模型集成 + W25Q64 分区终局

### W25Q64 8MB Flash 分区布局

```
0x000000 ┌──────────────────────────┐
         │ AI Model (YOLO detection) │  448 KB  — 人脸检测权重 + 命令流
0x070000 ├──────────────────────────┤
         │ Asset Directory           │    4 KB  — "ASST" magic, 92 entries
0x071000 ├──────────────────────────┤
         │ Boot Logo                 │  1.2 MB  — 1024×600 RGB565 raw
0x19D000 ├──────────────────────────┤
         │                          │
         │   UI Assets (free)        │  6.3 MB  — LVGL images, extra assets
         │                          │
0x720000 ├──────────────────────────┤
         │ Embed Model weights       │  768 KB  — MobileNetV2 0.35× INT8 (738,672 B)
0x7E0000 ├──────────────────────────┤
         │ Embed Command Stream      │   64 KB  — NPU command stream (7,000 B)
0x7F0000 ├──────────────────────────┤
         │ Face Database             │   60 KB  — 32 entries × (24B name + 512B emb)
0x7FF000 ├──────────────────────────┤
         │ Test Sector               │    4 KB  — w25q64_test_run() safety
0x800000 └──────────────────────────┘
```

**无重叠，无碎片。** 全 8MB 已分配。Logo 与嵌入模型之间有 6.3MB 留给 UI 资产。

### SDRAM 内存布局 (CPU0 16MB: 0x68000000-0x68FFFFFF)

```
0x68000000  VIN DMA buffers (3 × 614,400 B)
0x681C2080  fb_background[0..1] (GLCDC Layer 1, 2 × 1,228,800 B)
0x68500000  YOLO NPU Arena (442 KB)
0x68570000  YOLO Model weights (from flash, 433 KB)
0x68600000  Embed NPU Arena (144 KB)           ← NEW
0x68630000  Embed Model weights (from flash, 721 KB)  ← NEW
0x686E6000  (free)
0x68FFFFFF
```

### 新增文件
- `face_embedding_task.c/h` — 模型加载 (W25Q64→SDRAM)，NPU 推理 + 反量化 + L2 归一化
- `embed_model/embed_invoke.c` — NPU 调用封装 (替换生成的 sub_0001_invoke.c)
- `embed_model/model.c/h` — 简化 wrapper (去除 CPU compute_sub 依赖)
- `embed_model/compute_sub_0000.h` — 桩文件（模型 100% NPU，无 CPU 子图）

### 修改
- `lvgl_ui_face_stubs.c` — 真实预处理：人脸裁剪→96×96→INT8→NPU 推理
- `cpu0main_thread_entry.c` — boot 加入 `face_embedding_init()`
- 生成的 `sub_0001_invoke.c` → `#if 0` 禁用，`sub_0001_model_data.c` / `sub_0001_command_stream.c` → `#ifdef` 保护

### 遇到并解决的问题
- `compute_sub_0000.h` 不存在 → 重写 model.c 去除依赖
- `sub_0001_arena` 数组/指针类型冲突 → 统一为指针
- 两个 `RunModel` 重复 → 嵌入版重命名为 `EmbedRunModel`
- 生成的 invoke.c 重复编译 → `#if 0` 禁用

### 涉及文件
`face_embedding_task.c/h`, `embed_invoke.c`, `embed_model/model.c/h`, `sub_0001_invoke.h`, `lvgl_ui_face_stubs.c`, `cpu0main_thread_entry.c`

---

## 下一步

1. **Phase 2 收尾**：PCDC 烧录脚本 → 部署模型到 W25Q64 → 端到端测试录入+打卡
2. **Phase 3**：CPU1 SD 卡 + PDM 麦克风 + WAV 音频录制
3. **Phase 4**：CPU1 软件 MJPEG 编码 + AVI 封装 + 跨核帧传输

---

## Round 6 — 多人标注 + 手掌识别模式 (替换打卡)

### 新增功能
- **多人脸标注**：检测框按**首次出现顺序**标注 `person1`/`person2`/…（IoU 时序跟踪，连续编号——画面中有几个人就编到几，有人离开后自动补位），边框颜色按 红→绿→蓝 循环
- **手掌识别模型**：新增 96×96×1 INT8 单类 hand 检测器（与 v3 人脸检测器同架构），部署到 W25Q256
- **双模式检测**：`DETECTION_MODE_FACE` / `DETECTION_MODE_HAND`，NPU 按模式切换人脸/手掌模型（两模型均常驻 SDRAM，启动时一次性加载）
- **模式切换按钮**：LVGL UI 第一行全宽按钮 `Mode: Face` ↔ `Mode: Hand`（替换原 Enroll/Check-in 按钮），点击即切换
- **手掌检测框标注**：hand 模式下检测框统一标注 `hand`（无序号）

### 关键实现
- 手掌模型与 v3 人脸模型**张量布局完全相同**（arena 216KB，输入偏移 73728，输出偏移 0/192），复用同一套 `sub_0000_invoke.c` + 解码代码，仅通过 `face_detect_get_model_data()`/`face_detect_get_command_stream_size()` 按模式返回不同指针与大小
- 手掌模型**输出反量化参数**（从 gesture_int8.tflite 提取）：
  - box logits: scale=0.0265189, zp=-117
  - score logits: scale=0.0071936, zp=-89
- 手掌模型**输入预处理**为 **letterbox**（保宽高比 + 灰 114 填充），与人脸模型的中心裁剪不同；坐标映射 `cam = (ai - offset) / ratio`，ratio=0.15、offset_y=12
- **检测结果改存摄像头坐标**（640×480）：后处理把 AI(96×96) 框按模式映射回摄像头像素，摄像头任务只做 cam→LCD 偏移，无需感知模式

### 移除
- 人脸特征打卡流程（embedding + face_db + 键盘录入 + Clear DB）——`face_db.c/h`、`face_embedding_task.c/h`、`embed_model/` 保留在工程中但不再被调用
- `lvgl_ui_face_stubs.c` 清空（保留空翻译单元，可安全从构建中移除）

### Flash 分区变更
- 原人脸 embedding 分区 (0x00B00000) 复用为手掌模型分区：
  - `PART_HAND_MODEL_OFFSET = 0x00B00000`（256KB，weights 231,632B）
  - `PART_HAND_CMDSTREAM_OFFSET = 0x00B40000`（64KB，command stream 11,996B）

### 涉及文件
`face_detection_config.h`, `face_detection_task.c/h`, `face_detection_main.cc`, `face_detection_preprocess.c/h`, `model/sub_0000_invoke.c`, `w25q256_partition.h`, `mipi_camera_lcd.c`, `lvgl_ui_main.c/h`, `lvgl_ui_face_stubs.c`, `cpu0main_thread_entry.c`
