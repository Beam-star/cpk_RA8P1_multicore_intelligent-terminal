# GIF 动画转换为 W25Q64 Raw Flash 格式指南

## 概述

将 GIF 动画转换为原始 RGB565 像素数据，通过 PCDC 虚拟串口烧录到 W25Q64 Flash，供 LVGL 开机动画播放。

**与旧方案的区别**：
- ❌ 不再使用 LVGLImage.py（输出带 LVGL .bin 头 + LZ4 压缩，与当前代码不兼容）
- ❌ 不再使用 FatFS / USB MSC 模式部署
- ✅ 使用 `gif_to_raw_rgb565.py` 直接输出原始 RGB565（无头无压缩）
- ✅ 使用 `pcdc_flash_tool.py` 通过 USB 虚拟串口烧录
- ✅ 使用资产目录管理帧数据（支持变长帧、自动发现）

## 依赖安装

```bash
pip install Pillow pyserial
```

## 转换流程

### 步骤 1：GIF → 原始 RGB565 帧

使用 `scripts/gif_to_raw_rgb565.py`：

```bash
cd scripts
python gif_to_raw_rgb565.py ../Titan-mini_RA8P1_multicore_CPU0/src/lvgl_ui/GIF/handsome.gif 480 270 7
# 参数说明: <GIF路径> <宽度> <高度> [抽帧步长，默认1]
# 步长7 = 每7帧取1帧，用于减小总数据量
```

输出：
```
output_raw/
├── frame_000.bin    (259,200 字节 = 480×270×2)
├── frame_001.bin    (259,200 字节)
├── frame_002.bin    (259,200 字节)
...
└── frame_013.bin    (259,200 字节)
```

每帧为**原始 RGB565 像素数据**（小端序），无 LVGL 头、无压缩。开机动画代码在运行时自行构建 `lv_image_dsc_t` 描述符。

### 步骤 2：烧录到 W25Q64 Flash

```bash
# 确认设备连通
python pcdc_flash_tool.py COM10 ping
# 输出: Device responds: PCDC_FLASH/1.0

# 查询 Flash 信息
python pcdc_flash_tool.py COM10 info
```

**Flash 分区布局**（详见 `src/driver/w25q64/w25q64_partition.h`）：

```
0x000000 ─── AI 模型 (448KB)
0x070000 ─── 资产目录 (4KB, 最多 92 条记录)
0x071000 ─── LVGL 图像/动画数据 (~7.1MB)
0x7FF000 ─── 测试扇区 (保留)
```

**逐帧写入并注册资产目录**：

每帧大小 = `480 × 270 × 2 = 259,200 字节 = 0x3F480`

```bash
# 假设有 14 帧 (frame_000 ~ frame_013)

# 1. 擦除动画区域 (14 × 259200 = 3,628,800 字节 ≈ 0x376000)
python pcdc_flash_tool.py COM10 erase 0x400000 0x380000

# 2. 逐帧写入
python pcdc_flash_tool.py COM10 write output_raw/frame_000.bin 0x400000
python pcdc_flash_tool.py COM10 write output_raw/frame_001.bin 0x43F400
python pcdc_flash_tool.py COM10 write output_raw/frame_002.bin 0x47E800
python pcdc_flash_tool.py COM10 write output_raw/frame_003.bin 0x4BDC00
python pcdc_flash_tool.py COM10 write output_raw/frame_004.bin 0x4FD000
python pcdc_flash_tool.py COM10 write output_raw/frame_005.bin 0x53C400
python pcdc_flash_tool.py COM10 write output_raw/frame_006.bin 0x57B800
python pcdc_flash_tool.py COM10 write output_raw/frame_007.bin 0x5BAC00
python pcdc_flash_tool.py COM10 write output_raw/frame_008.bin 0x5FA000
python pcdc_flash_tool.py COM10 write output_raw/frame_009.bin 0x639400
python pcdc_flash_tool.py COM10 write output_raw/frame_010.bin 0x678800
python pcdc_flash_tool.py COM10 write output_raw/frame_011.bin 0x6B7C00
python pcdc_flash_tool.py COM10 write output_raw/frame_012.bin 0x6F7000
python pcdc_flash_tool.py COM10 write output_raw/frame_013.bin 0x736400
# 偏移计算公式: 0x400000 + N × 0x3F480

# 3. 注册到资产目录（名称格式: boot_NNN）
python pcdc_flash_tool.py COM10 dir_add boot_000 0x400000 259200
python pcdc_flash_tool.py COM10 dir_add boot_001 0x43F400 259200
python pcdc_flash_tool.py COM10 dir_add boot_002 0x47E800 259200
python pcdc_flash_tool.py COM10 dir_add boot_003 0x4BDC00 259200
python pcdc_flash_tool.py COM10 dir_add boot_004 0x4FD000 259200
python pcdc_flash_tool.py COM10 dir_add boot_005 0x53C400 259200
python pcdc_flash_tool.py COM10 dir_add boot_006 0x57B800 259200
python pcdc_flash_tool.py COM10 dir_add boot_007 0x5BAC00 259200
python pcdc_flash_tool.py COM10 dir_add boot_008 0x5FA000 259200
python pcdc_flash_tool.py COM10 dir_add boot_009 0x639400 259200
python pcdc_flash_tool.py COM10 dir_add boot_010 0x678800 259200
python pcdc_flash_tool.py COM10 dir_add boot_011 0x6B7C00 259200
python pcdc_flash_tool.py COM10 dir_add boot_012 0x6F7000 259200
python pcdc_flash_tool.py COM10 dir_add boot_013 0x736400 259200

# 4. 确认目录
python pcdc_flash_tool.py COM10 dir_read
```

### 步骤 3：修改开机动画配置

编辑 `src/lvgl_ui/lvgl_ui_boot_anim.h`，确保参数与实际 GIF 匹配：

```c
#define BOOT_ANIM_WIDTH         480         /* 与 GIF 宽度一致       */
#define BOOT_ANIM_HEIGHT        270         /* 与 GIF 高度一致       */
#define BOOT_ANIM_FPS           4           /* 目标帧率              */
#define BOOT_ANIM_FRAME_COUNT   14          /* 硬编码回退帧数（目录为空时使用） */
#define BOOT_ANIM_FLASH_OFFSET  0x400000UL  /* 硬编码回退偏移（目录为空时使用） */
```

### 步骤 4：编译运行

1. 修改 `cpu0main_thread_entry.c`：
```c
#define PCDC_ECHO_MODE            0   /* 关闭 PCDC Echo */
#define LVGL_KEYPAD_ENCODER_DEMO  1   /* 开启 LVGL Demo */
```

2. e2studio 编译烧录

3. 上电 → 开机动画自动播放

**加载逻辑**：`boot_anim_start()` 先扫描资产目录中所有 `boot_NNN` 条目 → 有则按目录偏移播放 → 目录为空则回退硬编码 `BOOT_ANIM_FLASH_OFFSET`。

---

## W25Q64 空间预估

W25Q64 可用空间约 **7.6MB**（8MB 减去 AI 模型 433KB + 资产目录 4KB）。

### 单帧大小

| 分辨率 | 原始 RGB565 | 说明 |
|--------|------------|------|
| 480×270 | 259,200 字节 (253 KB) | 当前使用 |
| 320×180 | 115,200 字节 (112 KB) | — |
| 240×135 | 64,800 字节 (63 KB) | — |

> `帧大小 = 宽度 × 高度 × 2`（RGB565 每像素 2 字节，无压缩无头）

### 不同帧数的空间需求 (480×270)

| 帧数 | 总大小 | 可用？ |
|------|--------|--------|
| 14 | 3.63 MB | ✅ |
| 20 | 5.18 MB | ✅ |
| 30 | 7.78 MB | ❌ 超过可用空间 |

---

## 一键脚本

保存为 `convert_and_flash.sh`：

```bash
#!/bin/bash
# convert_and_flash.sh — GIF → raw RGB565 → W25Q64 flash
# 用法: ./convert_and_flash.sh handsome.gif COM10 7
#        GIF路径       COM口  抽帧步长

GIF="$1"
PORT="${2:-COM10}"
STEP="${3:-1}"
WIDTH=480
HEIGHT=270
FLASH_BASE=0x400000   # 动画帧起始偏移
FRAME_SIZE=$((WIDTH * HEIGHT * 2))
FRAME_SIZE_HEX=$(printf "0x%X" $FRAME_SIZE)

echo "=== Step 1: Convert GIF to raw RGB565 frames ==="
python gif_to_raw_rgb565.py "$GIF" $WIDTH $HEIGHT $STEP
FRAME_COUNT=$(ls output_raw/*.bin 2>/dev/null | wc -l)
echo "Frames: $FRAME_COUNT"

echo "=== Step 2: Erase flash region ==="
TOTAL_SIZE=$((FRAME_COUNT * FRAME_SIZE))
TOTAL_HEX=$(printf "0x%X" $TOTAL_SIZE)
# Round up to nearest 4KB
ERASE_SIZE=$(( ((TOTAL_SIZE + 4095) / 4096) * 4096 ))
ERASE_HEX=$(printf "0x%X" $ERASE_SIZE)
python pcdc_flash_tool.py "$PORT" erase $FLASH_BASE $ERASE_HEX

echo "=== Step 3: Write frames ==="
for i in $(seq 0 $((FRAME_COUNT - 1))); do
    FN=$(printf "output_raw/frame_%03d.bin" $i)
    OFFSET=$((FLASH_BASE + i * FRAME_SIZE))
    OFFSET_HEX=$(printf "0x%X" $OFFSET)
    echo "  Frame $i → offset $OFFSET_HEX"
    python pcdc_flash_tool.py "$PORT" write "$FN" $OFFSET_HEX
done

echo "=== Step 4: Register in asset directory ==="
for i in $(seq 0 $((FRAME_COUNT - 1))); do
    NAME=$(printf "boot_%03d" $i)
    OFFSET=$((FLASH_BASE + i * FRAME_SIZE))
    OFFSET_HEX=$(printf "0x%X" $OFFSET)
    python pcdc_flash_tool.py "$PORT" dir_add "$NAME" $OFFSET_HEX $FRAME_SIZE
    echo "  Added $NAME @ $OFFSET_HEX"
done

echo "=== Done! ==="
echo "Now set LVGL_KEYPAD_ENCODER_DEMO=1, PCDC_ECHO_MODE=0 in cpu0main_thread_entry.c, rebuild and flash."
```

> **注意**：Flash 写入速度约 0.5~1.5 KB/s。14 帧 (~3.6MB) 大约需要 40~120 分钟。建议先用少量帧验证流程。

---

## 更改开机动画

要更换新的 GIF 动画：

1. 用 `gif_to_raw_rgb565.py` 转换新的 GIF
2. 修改 `lvgl_ui_boot_anim.h` 中的 `BOOT_ANIM_WIDTH/HEIGHT/FPS`
3. 重新擦除、写入、注册资产目录
4. 修改 `BOOT_ANIM_FRAME_COUNT` 和 `BOOT_ANIM_FLASH_OFFSET` 作为回退值
5. 重新编译烧录

## 故障排查

### 开机动画不播放

1. 检查 RTT 控制台输出是否有 `[ANIM]` 日志
2. 资产目录为空时会打印 `[ANIM] No animation data — skipping`
3. 硬编码回退检测到空 Flash 也会跳过
4. 确认 `PCDC_ECHO_MODE=0`（PCDC Echo 模式下不初始化 LVGL）

### 播放卡顿或画面撕裂

- 当前使用双缓冲 + VSync 同步，不应出现撕裂
- 如卡顿，检查 `BOOT_ANIM_FPS` 是否过高（建议 ≤4）
- 检查 `load_frame()` 中的 `memcpy` 是否从 memory-mapped XIP 正确读取

### 资产目录损坏

```bash
# 擦除目录扇区并重新注册
python pcdc_flash_tool.py COM10 erase 0x070000 0x1000
# 然后重新 dir_add 每一帧
```
