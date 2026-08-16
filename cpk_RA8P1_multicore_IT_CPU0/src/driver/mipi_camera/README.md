# MIPI CSI 摄像头驱动 (OV5640)

## 概述

基于 Renesas RA8P1 的 MIPI CSI-2 接口驱动 OV5640 摄像头模组，实现 640×480 实时视频采集与 LCD 显示。

**硬件架构：**
- OV5640 → MIPI CSI-2 (2-lane) → VIN (Video Input) → SDRAM
- VIN 硬件 CSCE (色彩空间转换引擎) 完成 YUV→RGB565 转换
- CPU0 采集，CPU1 通过共享内存接收帧数据并显示到 RGBLCD

**数据流：**
```
OV5640 (UYVY) → MIPI CSI-2 → VIN DMA → SDRAM (RGB565)
                                        ↓ 共享内存 cam_shmem_t
                              CPU1 轮询 → 旋转 90° → fb_background[] → GLCDC → LCD
```

## 文件结构

| 文件 | 说明 |
|------|------|
| `mipi_camera.c/h` | OV5640 初始化、VIN 驱动、采集控制 |
| `mipi_i2c.c/h` | I2C 通信封装 (FSP IIC Master) |
| `mipi_camera_test.c/h` | CPU0 采集测试任务 |
| `ov5640_regs.h` | OV5640 寄存器地址定义 |
| `rpmsg_cam.h` | CPU0/CPU1 共享内存同步协议 (位于 `src/rpmsg/`) |
| `mipi_camera_lcd.c/h` | CPU1 LCD 显示任务 (位于 `src/driver/mipi_camera/` on CPU1) |

## FSP 配置

### VIN 模块

| 配置项 | 值 | 说明 |
|--------|-----|------|
| 输入格式 | YCbCr422 8-bit | 与 OV5640 UYVY 输出匹配 |
| CSCE | 启用 (BPS=0) | 硬件 YUV→RGB565 转换 |
| 输出字节交换 | 禁用 (DMR bit4=0) | 标准小端 RGB565 |
| IS (Image Stride) | 640 | 每行 640 字节 = 320 像素 |
| Preclip | 0,0 → 639,479 | 640×480 全幅采集 |
| 三缓冲 | MB1/MB2/MB3 | VIN 自动轮转，无需运行时切换 |

### MIPI CSI

| 配置项 | 值 |
|--------|-----|
| Lane 数 | 2 |
| 数据类型 | YUV422 8-bit |
| 虚拟通道 | VC0 |

### GLCDC 显示

| 配置项 | 值 |
|--------|-----|
| 分辨率 | 1024×600 |
| 颜色顺序 | BGR (硬件自动交换 R/B) |
| 双缓冲 | fb_background[0/1] |

## 关键设计决策

### 1. VIN 硬件 CSCE vs 软件转换

**选择：硬件 CSCE**

VIN 的 CSCE (Color Space Conversion Engine) 可以在数据写入 SDRAM 之前完成 YUV→RGB 转换，无需 CPU 参与。

```
                  VIN 硬件
OV5640 (UYVY) ──────────────→ SDRAM (RGB565)
               CSCE 转换
               
CPU1 直接读取 RGB565，无需任何转换，帧率显著提升。
```

**配置要点：**
- `BPS=0`：启用 CSCE（FSP 默认值）
- `byte_swap=0`：禁用输出字节交换，确保标准小端 RGB565

### 2. VIN DMR byte swap 语义

VIN DMR bit[4] = `output_data_byte_swap`：

| byte_swap | 效果 | 内存布局 |
|-----------|------|----------|
| 1 (FSP默认) | 交换 16-bit 字内字节 | YUYV [Y0,Cb,Y1,Cr] |
| 0 (代码清除) | 不交换 | UYVY [Cb,Y0,Cr,Y1] |

**重要：** 此位影响的是 VIN **输出**到内存的字节顺序，不是 CSI-2 输入的顺序。

### 3. 共享内存帧同步

使用 `frame_id` 单调递增计数器，避免 D-Cache 一致性问题：

```
CPU0 (生产者):                 CPU1 (消费者):
1. VIN DMA 完成                1. 轮询 frame_id != last_frame_id
2. 更新 frame_addr 等参数      2. 读取 frame_addr, 复制+旋转到 fb
3. __DMB()                     3. last_frame_id = frame_id
4. frame_id++
5. CleanDCache
```

### 4. 双缓冲 + VSync 同步显示

`R_GLCDC_BufferChange()` 在下一个 VSync 切换缓冲区，但不会立即生效。
如果在切换前就写入另一个 buffer，而 GLCDC 还在读它，就会产生撕裂。

**解决方案：** 利用 `DisplayVsyncCallback`（rgblcd.c 中的 GLCDC VSync 中断回调）的 `g_frame_count` 计数器，在写入前等待 VSync，确保 GLCDC 不在读取目标 buffer：

```
等待 VSync (g_frame_count 变化)
    → GLCDC 切换到另一个 buffer，当前 buffer 安全
    → memcpy 写入 fb[write_idx]
    → R_GLCDC_BufferChange(fb[write_idx])，请求下一个 VSync 切换
    → write_idx = 1 - write_idx
    → 回到等待 VSync
```

```
时间轴:
VSync    VSync    VSync    VSync
  |        |        |        |
  | 写fb[0]|        | 写fb[1]|
  |------->|Buffer  |------->|Buffer
  |        |Change  |        |Change
  |        |→fb[0]  |        |→fb[1]
  |        |        |        |
  GLCDC:   fb[1]    fb[0]    fb[1]
```

## 调试过程中解决的 Bug

### Bug 1: I2C 通信失败 (Product ID = 0x00 0x00)

**现象：** 读取 OV5640 Product ID 全为 0x00。

**根因：** FSP IIC Master 的 slave 地址来自 `open()` 的 cfg，而 `hal_data.c` 中地址为 0x00。OV5640 的 I2C 地址是 0x3C。

**修复：** `open()` 后调用 `slaveAddressSet(&ctrl, 0x3C, I2C_MASTER_ADDR_MODE_7BIT)`。

### Bug 2: 共享内存读到垃圾数据 (0x55555555)

**现象：** CPU1 读取 cam_shmem_t 得到 0x55555555。

**根因：** 共享内存地址 0x69FFF000 在 RPMsg 共享内存区域 (0x69E00000~0x69FFFFFF) 内，RPMsg 初始化时清零了该区域。

**修复：** 将共享内存移到 RPMsg 区域之前：`CAM_SHMEM_ADDR = RPMSG_LITE_SHMEM_BASE - 0x1000`。

### Bug 3: 画面全粉 / 色彩完全错误

**现象：** LCD 显示全品红色，或只有绿色和品红色。

**根因（多个叠加）：**

1. **VIN byte swap 语义理解错误：** 代码清除 DMR bit4 后，VIN 输出 YUYV 而非 UYVY，但显示代码按 UYVY 读取 → Cb/Y 互换 → 色彩全错。

2. **VIN CSCE 未禁用导致双重转换：** FSP 配置了 CSCE (BPS=0)，VIN 已将 YUV 转为 RGB。显示代码又把 RGB 当 YUV 做了一次转换 → 颜色全乱。

3. **UYVY 字节偏移计算错误：** `src_col & ~3` 把 4 像素映射到同一 4 字节组，但 UYVY 是 2 像素/4 字节。导致所有像素读取同一区域数据。

**修复：**
- 清除 byte swap (DMR bit4=0) → VIN 输出 UYVY
- 设置 BPS=1 禁用 CSCE → VIN 直出原始 YUV
- 修正偏移计算：`(src_col >> 1) << 2` → 正确的 2 像素/组

**最终方案（硬件 CSCE）：**
- 保持 BPS=0 (CSCE 启用) → VIN 硬件做 YUV→RGB
- 清除 byte swap (bit4=0) → 输出标准小端 RGB565
- 显示代码直接读取 uint16_t，无需软件转换

### Bug 4: 画面卡死不刷新

**现象：** 显示一帧后画面冻结。

**根因：** 循环中每次调用 `mipi_camera_capture_start()`，但 VIN 第一次启动后进入连续采集模式 (FC.CC=1)，后续调用因 `MC.ME=1` 返回 `FSP_ERR_INVALID_STATE`。虽然 VIN 继续运行，但存在时序问题。

**修复：** 将 `mipi_camera_capture_start()` 移到循环外，只调用一次。

### Bug 5: 帧率低

**现象：** 画面更新缓慢。

**根因：** 显示任务优先级太低 (priority=2)，被其他任务抢占；软件 YUV→RGB 转换耗 CPU。

**修复：**
- 提升显示任务优先级到 3
- 使用硬件 CSCE 免除软件转换
- 用 `memcpy` 替代逐像素复制

## 使用方法

### 测试彩条模式

```c
// cpu0main_thread_entry.c
mipi_camera_test_start(true);   // true = OV5640 输出八色彩条
```

### 正常摄像头模式

```c
mipi_camera_test_start(false);  // false = 正常摄像头采集
```

### CPU1 启动显示

```c
// cpu1main_thread_entry.c, 在 rgblcd_init() 之后调用
mipi_camera_lcd_start();
```

## 性能

| 配置 | 帧率 | CPU 占用 |
|------|------|----------|
| 硬件 CSCE + memcpy | ~15fps | 低 |
| 软件 UYVY→RGB565 | ~8fps | 高 |

## 已知限制

1. **帧率受限于 VSync：** VSync 同步确保无撕裂，但每帧必须等待 VSync，最大帧率等于 GLCDC 刷新率 (60fps)。摄像头 15fps 远低于此限制，不影响实际帧率。

2. **IS 寄存器单位：** VIN IS=640 (字节)，每行存 320 像素。640 像素的摄像头需要 2 个 VIN 行存 1 行数据。显示代码读 1280 字节/行，跨 2 个 VIN 行，数据完整。

3. **摄像头方向：** OV5640 默认竖屏输出 (480×640)，显示代码旋转 90° CW 显示为横屏 (640×480)。旋转导致源数据非顺序访问，影响效率。如摄像头可物理旋转为横屏，去掉旋转可进一步提升性能。
