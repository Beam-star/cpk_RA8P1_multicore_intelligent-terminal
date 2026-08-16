# RA8P1 双核智能会议交互终端 — 分工与功能实现手册

> 项目：cpk_RA8P1_multicore_IT（CPK 定制板，从 Titan-mini 移植）
> 芯片：Renesas RA8P1（R7KA8P1KFLCAC）双核异构 FreeRTOS
> SDK：Renesas FSP 6.4.0 + Clang ARM toolchain v21.1.1
> 构建：Renesas e2studio IDE（无独立 Makefile / CMake）

---

## 1. 双核分工总览

| | CPU0（Cortex-M85，主核） | CPU1（Cortex-M33，从核） |
|---|---|---|
| **角色** | 交互 / 感知 / 决策 | 音视频存储与回放 |
| **优化** | 调试优化 `-O0` | 代码体积优化 `-Os` |
| **核心职责** | 显示、摄像头、AI 检测、UI、声源定位、ASR/AI/PPT、语音控制、舵机追踪、指纹、RPMsg 主机 | 音频录制、软件视频录制、音频/视频回放、会议记录落盘、RPMsg 从机 |
| **主要外设** | GLCDC、MIPI CSI、Ethos-U55 NPU、GT911(I2C)、UART0/1/2/4/6、OSPI_B | SDHI、PDM(I2S 输入)、SSI0/I2S 输出、ES8156 DAC(I2C) |

```
                        ┌─────────────────────────────┐
                        │           RA8P1             │
                        │                             │
   ┌────────────────────┼─── CPU0 (M85) ──────────────┤
   │  显示/触摸/摄像头/AI/UI ── 会议交互主界面          │
   │  ESP32 ASR/AI/PPT、CI1302 语音、声源定位、舵机、指纹│
   │                                    │ RPMsg       │
   │                                    ▼             │
   │  ┌──────────────────┼─── CPU1 (M33) ─────────────┤
   │  │  录音 WAV / 录像 AVI / 回放 / 会议记录 txt      │
   └──┼──────────────────┼─────────────────────────────┤
      SD 卡 (FatFS)      SDRAM (共享帧/模型/RPMsg)
```

---

## 2. CPU0（Cortex-M85，主核）详细分工

CPU0 是整机的「脸面与大脑」，负责所有与人交互的功能。代码工程：`cpk_RA8P1_multicore_IT_CPU0/`。

### 2.1 显示系统（GLCDC 双图层）

- **分辨率**：1024×600，RGB565。
- **图层 1**：摄像头画面（640×480 底部对齐，相机任务直接写帧缓冲，叠加检测框 + 标签）；上方 640×120 条渲染静态 logo（`toplogo` 资产，开机一次性 blit）。
- **图层 2**：LVGL 9.3 UI 面板（右侧 384×600，D/AVE 2D 硬件加速渲染）。
- 触屏坐标带 −640 的 X 偏移以命中图层 2 区域。
- 关键坑：FSP 生成的 GLCDC vsync 回调默认绑定 LVGL 端口的信号量，直连打开需在 `rgblcd_init()` 里本地覆盖回调为 `DisplayVsyncCallback`。

### 2.2 摄像头（OV5645 MIPI CSI）

- OV5645，2-lane MIPI CSI-2，640×480 RGB565，VIN DMA 写入 SDRAM。
- 相机任务（优先级 4）：VIN 帧就绪 → D-Cache 失效 → 通知检测任务 → 等 vsync → 旋转 90° → 写图层 1 → 画框 → D-Cache clean → `R_GLCDC_BufferChange`。

### 2.3 AI 检测（Ethos-U55 NPU，双模型）

- **两个模型**：人脸检测 v3（anchor-free 96×96 MobileNetV2 w0.75）+ 手势检测（同架构灰度版）。
- 输入 96×96×1 INT8 灰度；推理 ~3ms；Tensor Arena 216KB（双模型复用）。
- 两种检测模式互斥：`DETECTION_MODE_FACE`（框标 `person1/2/…`，IoU 时序跟踪保持标签稳定）/ `DETECTION_MODE_HAND`（框标 `hand`）。
- 前处理人脸走中心裁剪、手势走 letterbox；后处理解码 + NMS 后把 AI(96×96) 框映射回相机(640×480) 空间。
- D-Cache 一致性三处必须处理：模型数据（加载后 clean 一次）、NPU 输入/输出 arena（推理前后 clean/invalidate）、相机帧缓冲（DMA 后 invalidate）。

### 2.4 LVGL UI（四页滑动界面）

- **页面 1**：控制台（两个子面板翻转：会议控制 / 智能外设）+ 底部共享区（字幕、日志 + 动态小人 mascot、录制计时、状态栏）。
- **页面 2**：音频/视频/笔记文件浏览器（播放/删除/进度/暂停/取消）。
- **页面 3**：声源定位粒子雷达。
- **页面 4**：CI1302 语音命令参考（中文滚动列表）。
- **动态图**（`lvgl_ui_anim.c`）：页面 1 小人用线条小狗 GIF 慢速循环（~4 fps）；指纹录入/打卡时弹出居中 loading 转圈（跨页可见）。帧存 W25Q256，SDRAM 双缓冲 + LVGL 定时器驱动，不占 LVGL 任务栈。
- 艺术字体：标题 Bangers、功能按钮 Righteous、短标签 Cinzel Decorative；正文 montserrat，中文 `myChineseFont`。

### 2.5 声源定位（Sipeed MA-USB8 麦克风阵列）

- UART1 / SCI1（P706/P707，2Mbps 8N1），持续流式 16×16 声场热力图。
- 解析任务提取峰值方向 → 驱动页面 3 粒子雷达 + 舵机声源追踪。

### 2.6 ASR / AI 对话 / PPT（ESP32，UART2 / SCI2）

- ESP32（云端大模型）↔ RA8P1，P801/P802，115200 8N1。
- 单字节命令：`0x01/0x02` ASR 开关、`0x03/0x04` AI 开关、`0x06` PPT 进入、`0x07/0x08` 上下翻页、`0x09` PPT 退出。
- ESP32 回传 UTF-8 中文逐行字幕 → 页面 1 中文字幕区。
- ASR/AI/PPT 三模式互斥（弹窗提示先停另一个）。
- **PPT 手势翻页**：仅「Hand 检测 + PPT 开」时，跟踪最大手势框中心 EMA 平滑后的水平滑动，左→右=上一页、右→左=下一页，带最小位移/时间窗/再触发回滞/冷却多重防抖。

### 2.7 离线语音控制（CI1302，UART0 / SCI0）

- CI1302 离线语音识别模块（P602/P603，115200 8N1），无需网络。
- 5 字节帧 `AA 55 <CMD> <DATA> FB`；RX 把识别命令映射到与 UI 按钮相同的动作函数（与触摸共用一条代码路径 + 同一套互斥弹窗）。
- UI 按钮点击时回发 0xFF 组「被动播报」帧让模块发声（替代已移除的 SD 卡 WAV 音效）。

### 2.8 舵机追踪（FEETECH STS，UART4 / SCI4）

- 单轴水平云台（pan），P414/P415，1Mbps 8N1，FT-SCS 协议。
- 两种互斥模式：**人脸追踪**（对检测框中心 X 做 PID，目标 person1/2/3 可选）/ **声源追踪**（麦克风阵列方位角 EMA + 比例控制）。
- `取消追踪` 通用关闭两者；±35° 行程限制（摄像头 FPC 排线太短），PID 死区 + 积分限幅 + 单拍步进限速保证平滑稳定。

### 2.9 指纹（ZW111，UART6 / SCI6）

- ZW111 半导体指纹模块（100 枚容量），P909/P908，57600 8N1。
- 三种操作：**录入**（`PS_AutoEnroll`，3 次按压，成功弹名字键盘）/ **打卡**（`PS_AutoIdentify` 1:N）/ **清空**（`PS_Empty`，弹确认框）。
- 阻塞操作在 `zw111_fp` worker 任务执行；结果经回调 defer 回 LVGL 任务。
- **加载动画**：录入/打卡期间弹出居中 loading 转圈（`lvgl_ui_loading_show/hide`），完成时收起；右上角 ✕ 可取消（发 `PS_Cancel 0x30` 终止模组操作）。
- **指纹 ↔ 名字映射**：录入成功即输入名字，按 `page_id` 存 W25Q256 @0xBD0000（4KB 分区，`fp_name_db` 表，RAM 副本 + 整块回写）。
- **打卡去重**：同一名字（同人多指纹）每场会议只打卡一次；名单在关闭会议记录时清空。

### 2.10 会议记录（ASR 转文字 → txt）

- 指纹打卡后开启 ASR，字幕逐行流式写入 CPU1 的 `/meeting/notes/record_XX.txt`。
- 首行「参会人员有: 甲、乙、丙…」在收到第一条字幕时写入，列出**本场会议所有已打卡名字**（去重后）。
- 无字幕则 txt 保持空，关闭时由 CPU1 丢弃不保存。
- 每次打卡 = 一场会议记录；开启下一场 ASR 需重新打卡。

---

## 3. CPU1（Cortex-M33，从核）详细分工

CPU1 是「记录与回放」引擎，全部由 CPU0 经 RPMsg 控制。代码工程：`cpk_RA8P1_multicore_IT_CPU1/`。

### 3.1 音频录制（PDM → WAV）

- 3 通道 PDM 麦克风阵列，16kHz，Mic0 → WAV 编码 → SD `/meeting/audio/meeting_XX.wav`（顺序命名，与视频同名配对）。
- 批量缓冲写入，与视频录制是**两个独立文件**（无音视频同步）。

### 3.2 软件视频录制（共享帧 → MJPEG → AVI）

- CPU0 相机任务把 640×480 打包帧写入共享 SDRAM 双缓冲（~10fps）。
- CPU1 轮询 `frame_id` → 内部 2×2 下采样到 320×240 → MJPEG 基线编码 → AVI 容器（`00dc` 块）→ SD `/meeting/video/meeting_XX.avi`（顺序命名，与音频同名配对）。
- 性能优化：下采样减块、量化用倒数乘法替代浮点除、每 MCU 行 `taskYIELD()` 避免饿死录音任务。

### 3.3 音频播放（WAV → ES8156 DAC）

- SD WAV → I2S（SSI0 + DTC DMA，TX_EMPTY 触发乒乓）→ ES8156 DAC。
- 顺序「读块→写块」模型 + 自动欠载恢复；停止后延时 2ms 再 close 解决异步停止竞态。

### 3.4 软件视频播放（AVI → MJPEG 解码）

- AVI 解析（提取 `00dc` MJPEG 块 + 帧间隔）→ 基线 JPEG 软件解码 → 320×240 RGB565 → 共享 SDRAM 双缓冲。
- CPU0 `video_play_display` 任务 2× 放大 → GLCDC 图层 1；期间暂停相机任务的图层 1 写入。

### 3.5 会议记录笔记落盘

- `REC_CMD_NOTES_OPEN/APPEND/CLOSE/READ`：CPU0 开启 ASR 后逐行字幕追加，关闭时落盘为 `/meeting/notes/record_XX.txt`（空文件丢弃），读取时按块回传中文文本查看器。

### 3.6 未启用模块（树内保留）

- 以太网（RMAC + RTL8211F PHY，FreeRTOS+TCP）、CFTP 文件服务器、W800 WiFi —— 代码在树内但**当前未初始化**。

---

## 4. 双核通信（RPMsg-Lite）

- NXP RPMsg-Lite over 共享内存（当前用 SDRAM 末尾 2MB @0x69E00000）。
- 逻辑通道：日志（CPU0=50 / CPU1=51）、录音/音频控制（CPU1=60 / CPU0=61）。
- 协议定义于 `src/rpmsg/rpmsg_record.h`（两工程各持一份相同拷贝）。
- **缓存一致性**：CPU0 的共享区（PDM shmem + cam_shmem + RPMsg）经 MPU 配为**非缓存**，禁止在 IPC 中断里做整区 clean/invalidate（会丢命令）。

---

## 5. 内存映射（SDRAM 16MB，0x68000000 起）

| 地址 | 用途 |
|---|---|
| 0x68000000 | VIN DMA 缓冲（3×614400B） |
| 0x681C2080 | 双帧缓冲（2×1024×600×2） |
| 0x68500000 | NPU Tensor Arena（216KB，双模型复用） |
| 0x68540000 | 人脸模型权重 + 命令流 |
| 0x68600000 | 手势模型权重 + 命令流 |
| 0x6863C000 | 视频帧同步结构 `video_shmem_t` |
| 0x68640000 / 0x686E0000 | 视频帧双缓冲（640×480 RGB565） |
| 0x68800000 | 中文字体 `myChineseFont` |
| 0x68F00000–0x68FFFFFF | CPU1 区（1MB） |

> 逻辑分区仅作开发约定，无硬件 MMU 强制；NPU 经 AXI 总线直接访问 SDRAM。

### W25Q256 Flash 分区（32MB，XIP @0x90000000）

| 偏移 | 内容 |
|---|---|
| 0x000000 | 人脸检测模型（448KB） |
| 0x070000 | 资产目录（4KB） |
| 0x071000 | 开机 LOGO（1024×600 RGB565） |
| 0x1E0000 | 顶部条 logo（640×120，摄像头上方） |
| 0x210000 | 动态小人帧（线条小狗，14 × 128×128，蓝青底） |
| 0x290000 | 指纹 loading 转圈（20 × 96×96） |
| 0xB00000 | 手势模型权重 |
| 0xB40000 | 手势命令流 |
| 0xBD0000 | 指纹名字库（4KB） |
| 0xBE0000 / 0xD11000 / 0xD15000 | 中文字体（bitmap/unicode/glyph-dsc） |

---

## 6. 启动流程（CPU0 主线程，`cpu0main_thread_entry.c`）

`OPERATING_MODE` 宏选择模式：`MODE_PCDC_ECHO`(1) / `MODE_LVGL_DEMO`(2) / `MODE_CAMERA`(3) / `MODE_RGBLCD_TEST`(4)。

**相机模式（会议终端主模式，当前默认）**：
1. `R_BSP_SecondaryCoreStart()` 启动 CPU1。
2. 背光 → `lv_init()` → GLCDC 双图层打开（图层 2 回调覆盖）→ GT911 触摸 → `w25q256_open()`。
3. `boot_logo_show_layer1(2000)` 全屏 LOGO。
4. `lvgl_ui_init()`（LOGO 之后初始化，避免面板闪烁）。
5. `mipi_camera_lcd_start()` → 相机任务拉起检测任务 → 加载人脸 + 手势双模型。
6. LED 闪烁主循环。

---

## 7. 任务优先级布局

### CPU0（0=空闲，4=最高）

| 优先级 | 任务 |
|---|---|
| 4 | 相机/LCD `mipi_camera_lcd_task`、视频回放显示 `vplay_disp` |
| 3 | LVGL UI `lvgl_task` |
| 2 | 人脸检测 `face_detection_task`、指纹 worker `zw111_fp`、ESP32 解析、CI1302 解析、录入/清库 worker |
| 1 | 录音状态 `rec_status_task`、PPT 手势 `ppt_gest`、舵机追踪 `servo_track` |

### CPU1

| 优先级 | 任务 |
|---|---|
| 4 | PDM SHMEM `pdm_shmem_task` |
| 3 | 录音控制 `rec_ctrl_task`、回放 `playback_task`、视频回放 `video_playback_task` |
| 2 | CFTP 客户端 handler |
| 1 | 视频录制 `video_task`、录音 `recorder_task` |

**设计原则**：相机 > LVGL > 人脸检测（相机最高避免丢帧，检测在空闲间隙跑）；LVGL 非线程安全，所有 LVGL API 必须在 LVGL 任务上下文；flash 写入绝不进 LVGL 回调（走 worker 任务）。

---

## 8. 关键约束与已知问题

- **LVGL 9.3 API**（非 v8）；`get_glyph_bitmap` 须返回整个 `lv_draw_buf_t*`。
- **运行时写 flash 前必须 `w25q256_open()`**：启动时 XIP 读取（字体/模型）后 OSPI_B 处于 `R_OSPI_B_Erase` 会卡住的状态，重新 open 重建 SPI 模式 + 配置（与 PCDC 下载的 open→erase→write 一致）。
- **WRITE_CUSTOM=1**：W25Q256 走 4 字节 DirectTransfer 写路径（WREN + 页编程），已验证可用。
- **SDRAM 地址移位 = 8**（CPK 板 7 列地址位）；若换 SDRAM 芯片需核对列宽。
- **GLCDC `clock_div_ratio` = 12**：降低像素时钟避免双图层 SDRAM 带宽饱和闪烁。
- **USB PCDC 控制类请求**：`R_USB_PeriControlDataSet/Get` 不能从 RTOS 回调（ISR 上下文）调用，须延迟到 PCDC 线程（否则 Windows COM 口打不开）。
- **摄像头帧缓冲区 MPU 非缓存**：避免 D-Cache 幽灵写入导致的显示噪点。
- **I2S 停止异步竞态**：stop 后延时 2ms 再 close，处理 `FSP_ERR_ALREADY_OPEN`。

---

## 9. 构建与部署

- **构建**：e2studio 中右键解决方案 `Titan-mini_RA8P1_multicore/` → Build Project（依次构建 CPU0/CPU1）。
- **调试**：J-Link（各工程根目录 `.jlink`）；CPU0 先启动，运行时 `R_BSP_SecondaryCoreStart()` 启动 CPU1。
- **AI 模型编译**：模型变更需走独立 ruhmi-framework-mcu 流水线（`deploy_v3/` 人脸、`deploy_hand_v2/` 手势），Vela 编译成 Ethos-U 命令流后经 PCDC 下载到 W25Q256。

### 整板烧录脚本（PC 端，需板子跑 `MODE_PCDC_ECHO`）

| 脚本 | 用途 |
|---|---|
| `download_all.ps1` | 全新板一次性编程：人脸模型 + 手势模型 + 开机 LOGO + 顶部 logo + 动态小人帧 + 指纹 loading + 资产目录 |
| `download_font.ps1` | 编程中文字体（bitmap @0xBE0000 / unicode @0xD11000 / glyph-dsc @0xD15000） |
| `scripts/pcdc_flash_tool.py` | PCDC flash 传输工具（write/read/erase/verify） |

---

## 10. 功能实现状态一览

| 功能 | 状态 |
|---|---|
| 双图层显示（摄像头 + LVGL UI） | ✅ |
| 人脸检测 + 手势检测（双模型 NPU） | ✅ |
| 声源定位粒子雷达 | ✅ |
| 实时 ASR 字幕 / AI 对话字幕 | ✅ |
| PPT 手势翻页 | ✅ |
| CI1302 离线语音控制 | ✅ |
| 舵机追踪（人脸 + 声源） | ✅ |
| 指纹录入 / 打卡 / 清空 + 名字映射 | ✅ |
| 音频录制（WAV）/ 视频录制（AVI，独立文件） | ✅ |
| 音频/视频回放（暂停/恢复/取消/进度） | ✅ |
| 会议记录笔记（ASR → txt，多参会人员） | ✅ |
| 以太网 / CFTP / WiFi | ⏳ 未启用（树内保留） |
