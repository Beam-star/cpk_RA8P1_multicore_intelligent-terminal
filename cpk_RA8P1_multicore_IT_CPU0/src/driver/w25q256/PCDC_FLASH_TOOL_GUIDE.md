# PCDC Flash 传输工具使用指南

## 概述

`pcdc_flash_tool.py` 是 PC 端工具，通过 USB 虚拟串口（PCDC）将数据写入 Titan-mini 开发板的 W25Q64 QSPI Flash。

**适用场景**：
- 烧录 AI 人脸检测模型（YOLO 权重 + NPU 命令流）
- 烧录 LVGL 开机动画帧（原始 RGB565 像素数据）
- 烧录 LVGL UI 图片资源
- 所有需要通过 `.c` 数组或 `.bin` 文件写入 Flash 的场景

**无需**：FatFS、USB MSC、把大文件编译进固件。

---

## 环境准备

### PC 端

```bash
pip install pyserial
```

### 开发板端

确保固件编译时 `PCDC_ECHO_MODE=1`（在 `cpu0main_thread_entry.c` 中设置）：

```c
#define PCDC_ECHO_MODE      1
```

编译烧录后，用 USB 线连接开发板到 PC。PC 会识别出一个串口设备（如 COM10）。

## 什么是资产目录？

W25Q64 Flash 是**原始存储**，没有文件系统（不用 FatFS）。这意味着你往 Flash 里写了一堆数据后，MCU 代码并不知道"哪个数据在哪个位置、有多大"。

**资产目录**就是解决这个问题的——它是 Flash 上一个固定位置的"索引表"，每条记录存着：

| 字段 | 大小 | 说明 |
|------|------|------|
| 名称 | 32 字节 | 资产名，如 `boot_000`、`logo`、`model` |
| 偏移 | 4 字节 | 数据在 Flash 中的绝对地址 |
| 大小 | 4 字节 | 数据的字节数 |
| CRC32 | 4 字节 | 数据校验（暂未使用，填 0） |

```
类比: 资产目录 ≈ 文件系统的文件夹
      资产条目 ≈ 文件

传统文件系统:  打开 "/boot/frame_000.bin" → 查目录 → 找扇区 → 读取
资产目录方式:  扫描目录 → 找 "boot_000" → offset=0x400000, size=259200 → 读 Flash
```

**开机动画代码**启动时扫描资产目录，找到所有 `boot_000`、`boot_001`... 自动按序播放。目录为空（全是 0xFF）则回退硬编码偏移。

**AI 模型加载**也可按名字查找（如 `dir_add model_weights 0x000000 422048`），不再写死地址。

---

## Flash 分区布局

```
W25Q64 8MB (0x000000 ~ 0x7FFFFF)
┌──────────────────────────────────────────────┐
│ 0x000000  AI 模型区域 (448KB)                 │
│           ├─ 模型权重 422,048 字节             │
│           └─ NPU 命令流 11,252 字节            │
├──────────────────────────────────────────────┤
│ 0x070000  资产目录 (4KB, 1 个扇区)              │
│           最多 92 条记录                        │
├──────────────────────────────────────────────┤
│ 0x071000  LVGL 图像/动画数据 (~7.1MB)           │
│           ├─ 静态 UI 图片                       │
│           └─ 开机动画帧                         │
├──────────────────────────────────────────────┤
│ 0x7FF000  测试扇区 (4KB, 保留)                 │
└──────────────────────────────────────────────┘
```

---

## 命令参考

### 测试连接

```bash
python scripts/pcdc_flash_tool.py COM10 ping
```

输出示例：
```
Connecting to COM10...
Device responds: PCDC_FLASH/1.0
```

### 查询 Flash 信息

```bash
python scripts/pcdc_flash_tool.py COM10 info
```

输出示例：
```
Flash capacity:    8.0 MB (8388608 bytes)
Sector size:       4096 bytes (4 KB)
Page size:         256 bytes
Max payload/frame: 256 bytes
```

### 擦除 Flash

```bash
# 擦除 0x400000 起始的 0x3A0000 字节（3.625 MB）
python scripts/pcdc_flash_tool.py COM10 erase 0x400000 0x3A0000

# 擦除整个 AI 模型区域
python scripts/pcdc_flash_tool.py COM10 erase 0x000000 0x070000
```

擦除粒度自动向上取整到 4KB 扇区边界。

### 写入数据

```bash
# 支持 .c 数组文件（自动解析十六进制值）
python scripts/pcdc_flash_tool.py COM10 write model_weights.c 0x000000

# 支持 .bin 原始二进制文件
python scripts/pcdc_flash_tool.py COM10 write frame_000.bin 0x400000

# 支持任意包含 C 数组的 .h 文件
python scripts/pcdc_flash_tool.py COM10 write image_data.h 0x071000
```

输出示例：
```
File:  model_weights.c
Size:  422048 bytes (412.2 KB)
Addr:  0x00000000
Chunks: 1692 (252 B payload each)
  10%  42/412 KB  0.5 KB/s  ETA 740s
  20%  84/412 KB  0.6 KB/s  ETA 560s
  ...
Done. 422048 bytes in 720.5s (0.6 KB/s)
```

> **注意**：写入速度约为 0.5~1.5 KB/s（受限于 FSP OSPI_B 驱动的 64 字节突写工作区）。一个 433KB 的模型大约需要 5~15 分钟。请耐心等待。

### 计算 C 数组二进制大小

`.c` 数组文件在磁盘上是文本格式（含 `0xNN`、逗号、空格），实际写入 Flash 的是解析后的**原始二进制**。用 `carray_size.py` 查看精确的二进制大小：

```bash
# 查看单个文件的大小
python scripts/carray_size.py sub_0000_model_data.c

# 查看大小 + 自动推算下一个文件的写入偏移（链式写入）
python scripts/carray_size.py sub_0000_model_data.c --next-offset 0x000000
```

输出示例：
```
File:    sub_0000_model_data.c
Entries: 422,048  bytes  (412.16 KB)
         0X000670A0

Offset chain:
  Write @ 0X00000000
  Size   0X000670A0  (422,048 B)
  ─────────────────
  Next → 0X000670A0              ← 下一个文件应写入此地址
```

### 验证数据

```bash
python scripts/pcdc_flash_tool.py COM10 verify model_weights.c 0x000000
```

输出示例：
```
Verifying model_weights.c (422048 bytes) against flash @ 0x00000000...
  25%
  50%
  75%
VERIFY PASS — 422048 bytes match
```

### 读取 Flash 到文件

```bash
python scripts/pcdc_flash_tool.py COM10 read 0x000000 256 dump.bin
```

### 资产管理

**查看资产目录**：
```bash
python scripts/pcdc_flash_tool.py COM10 dir_read
```

输出示例：
```
Directory magic: 0x41535354 (OK)
Entry count:     14

Name                             Offset       Size       CRC32
------------------------------------------------------------------
boot_000                    0x00400000     259200  0x00000000
boot_001                    0x0043F400     259200  0x00000000
...
```

**添加资产条目**：
```bash
python scripts/pcdc_flash_tool.py COM10 dir_add boot_000 0x400000 259200
python scripts/pcdc_flash_tool.py COM10 dir_add logo    0x071000  76800
```

参数：`dir_add <COM口> <名称(最长31字符)> <Flash偏移量> <字节大小>`

### 重启 MCU

```bash
python scripts/pcdc_flash_tool.py COM10 reset
```

---

## 完整操作流程

### 场景 A：烧录 AI 模型

模型文件位于 `Titan-mini_RA8P1_multicore_CPU0/src/ai_application/model/`：

| 文件 | 磁盘大小 | 二进制大小 | Flash 偏移 |
| ---- | -------- | ---------- | ---------- |
| `sub_0000_model_data.c` | ~2.4 MB（文本） | 422,048 字节 | `0x000000` |
| `sub_0000_command_stream.c` | ~68 KB（文本） | 11,252 字节 | `0x000670A0` |

#### 第一步：用 `carray_size.py` 确认偏移量

```bash
# 推算命令流的写入偏移
python scripts/carray_size.py Titan-mini_RA8P1_multicore_CPU0/src/ai_application/model/sub_0000_model_data.c --next-offset 0x000000
# 输出: Next → 0X000670A0   ← 这就是命令流地址
```

#### 第二步：擦除 + 写入

```bash
# 擦除 AI 模型区域（448KB）
python scripts/pcdc_flash_tool.py COM10 erase 0x000000 0x070000

# 写入模型权重
python scripts/pcdc_flash_tool.py COM10 write Titan-mini_RA8P1_multicore_CPU0/src/ai_application/model/sub_0000_model_data.c 0x000000

# 写入 NPU 命令流（偏移由上一步 carray_size.py 输出确定）
python scripts/pcdc_flash_tool.py COM10 write Titan-mini_RA8P1_multicore_CPU0/src/ai_application/model/sub_0000_command_stream.c 0x000670A0
```

#### 第三步：验证

```bash
python scripts/pcdc_flash_tool.py COM10 verify Titan-mini_RA8P1_multicore_CPU0/src/ai_application/model/sub_0000_model_data.c 0x000000
python scripts/pcdc_flash_tool.py COM10 verify Titan-mini_RA8P1_multicore_CPU0/src/ai_application/model/sub_0000_command_stream.c 0x000670A0
```

#### 第四步：切换到摄像头模式运行

修改 `cpu0main_thread_entry.c`：

```c
#define OPERATING_MODE  MODE_CAMERA
// 确保 FACE_DETECTION_ENABLE = 1
```

编译烧录，上电后控制台应输出 `[FACE_DET] Model loaded successfully`。

### 场景 B：烧录开机动画帧

```bash
# 1. 准备帧文件
#    使用 LVGLImage.py 将 GIF 转为 PNG 帧，再用 --ofmt C --cf RGB565 --compress NONE 生成 .c 文件
#    或者直接使用 raw RGB565 .bin 文件（每帧 480×270×2 = 259,200 字节）

# 2. 擦除动画区域（14帧 × 259,200 = 3,628,800 字节 ≈ 0x376000）
python scripts/pcdc_flash_tool.py COM10 erase 0x400000 0x380000

# 3. 逐帧写入
python scripts/pcdc_flash_tool.py COM10 write frame_000.bin 0x400000
python scripts/pcdc_flash_tool.py COM10 write frame_001.bin 0x43F400
python scripts/pcdc_flash_tool.py COM10 write frame_002.bin 0x47E800
# ... 每帧间隔 = 259200 字节 = 0x3F480
# 计算公式: offset_n = 0x400000 + n * 259200
# 第 n 帧: 起始偏移 = 0x400000 + n × 0x3F480

# 4. 注册到资产目录（开机动画代码会自动扫描）
python scripts/pcdc_flash_tool.py COM10 dir_add boot_000 0x400000 259200
python scripts/pcdc_flash_tool.py COM10 dir_add boot_001 0x43F400 259200
# ... 逐帧添加（名称必须为 boot_000, boot_001, ... boot_013）

# 5. 查看目录确认
python scripts/pcdc_flash_tool.py COM10 dir_read

# 6. 重新编译固件（LVGL_KEYPAD_ENCODER_DEMO=1），烧录，上电即可看到开机动画
```

### 场景 C：烧录 LVGL UI 图片

```bash
# 1. 用 LVGLImage.py 生成 .c 数组（raw RGB565，无压缩）
python LVGLImage.py --ofmt C --cf RGB565 --compress NONE -o ./output/ images/

# 2. 写入到 LVGL 数据区域
python scripts/pcdc_flash_tool.py COM10 write logo.c    0x071000
python scripts/pcdc_flash_tool.py COM10 write icon_01.c 0x072000
python scripts/pcdc_flash_tool.py COM10 write icon_02.c 0x073000

# 3. 注册到资产目录
python scripts/pcdc_flash_tool.py COM10 dir_add logo    0x071000 76800
python scripts/pcdc_flash_tool.py COM10 dir_add icon_01 0x072000  4800
python scripts/pcdc_flash_tool.py COM10 dir_add icon_02 0x073000  4800
```

---

## C 数组文件格式说明

工具支持解析标准 C 数组格式：

```c
// 单行注释会被忽略
/* 多行注释也会被忽略 */

const unsigned char my_data[] = {
    0x00, 0x01, 0x02, 0x03,
    0x04, 0x05, 0xFF, 0xFE,
    // 支持十进制
    255, 128, 64, 32,
};

// 或 uint32_t 数组
const uint32_t big_data[] = {
    0x12345678, 0x9ABCDEF0,
};
```

解析规则：
- 提取第一个 `{` 和最后一个 `}` 之间的所有数值
- 十六进制 `0xNN` 直接转换
- 十进制数字直接转换
- 超过 255 的值按小端序拆分为多个字节
- 忽略注释、空白符

---

## 常见问题

### Q: 写入速度为什么这么慢？

A: FSP OSPI_B 驱动存在 DMAC 写入路径 bug，当前通过 `R_OSPI_B_DirectTransfer` 以 64 字节为一批写入 W25Q64，导致实际速度约 0.5~1.5 KB/s。详细分析见 `w25q64/README.md`。

### Q: 写入过程中断开 USB 会怎样？

A: W25Q64 会保留已写入的部分数据。重新连接后，需要重新擦除并从头写入。Flash 写入是"先擦后写"的，已擦除未写入的区域为 0xFF。

### Q: 能否在 LVGL 显示状态下同时传输？

A: 不建议。当前 PCDC Echo 模式下，LVGL 和摄像头不启动（见 `cpu0main_thread_entry.c` 的 `PCDC_ECHO_MODE` 分支）。Flash 写入期间 OSPI_B 被独占，如果 LVGL 同时从 Flash 读取图像（memory-mapped XIP），会产生总线冲突。

### Q: 资产目录满了怎么办（超过 127 条）？

A: 一个 4KB 扇区最多存 127 条记录。通常够用。如果确实需要更多，可以修改 `FLASH_ASSET_MAX_ENTRIES` 并扩大 `PART_ASSET_DIR_SIZE`。

### Q: 如何找到 Renesas PCDC 设备对应的 COM 口号？

A:
- 工具支持自动检测：不指定 `--port` 参数时，自动查找 VID:045B PID:5001 的设备
- 手动查找：Windows 设备管理器 → 端口(COM和LPT) → USB Serial Device (COMxx)
- 或运行 `python -c "import serial.tools.list_ports; [print(p.device, hex(p.vid), hex(p.pid)) for p in serial.tools.list_ports.comports()]"`

### Q: 串口助手打开时能同时使用此工具吗？

A: 不能。一个 COM 口只能被一个程序打开。使用此工具前请关闭串口助手。

---

## 技术细节

### 协议帧格式

```
字节 0      : STX (0xAA) — 帧起始标记
字节 1      : CMD — 命令码
字节 2-3    : LEN — 负载长度 (小端序 uint16)
字节 4..    : PAYLOAD — 负载数据 (LEN 字节)
末尾 2 字节 : CRC16 — XMODEM 校验 (小端序)
```

### CRC16 参数

- 多项式：0x8005 (XMODEM)
- 初始值：0x0000
- 覆盖范围：CMD + LEN + PAYLOAD（不含 STX）

### 流控

停等协议。PC 端每发一帧等待 MCU 回复 ACK/NACK，5 秒超时，最多重试 3 次。

### 写入策略

每条 `WRITE` 命令最多携带 252 字节负载（256 减去 4 字节地址头）。MCU 端对每条 WRITE 执行 `w25q64_open()` → `w25q64_write()` → `w25q64_close()`，确保 OSPI_B 不会长时间占用。
