# RGBLCD 双缓冲画面撕裂 Bug 记录

## 问题描述

将 RGBLCD 驱动从 CPU1 迁移到 CPU0 后，摄像头画面出现严重闪烁和撕裂：

- 画面在"当前帧"和"上一帧"之间快速闪烁
- 只显示了屏幕上半部分画面，下半部分基本没有显示
- 整体效果像是"只画了一半就被刷新"

## 根本原因

**R_GLCDC_BufferChange() 是异步操作**，它告诉 GLCDC "在下一个 vsync 时切换到指定缓冲区"，但函数返回时 GLCDC 还在读取当前缓冲区。

### 问题时序（错误）

```
CPU:     [写入 fb[0] 上半部分] ... [写入 fb[0] 下半部分] → [R_GLCDC_BufferChange(fb[0])]
GLCDC:   [读取 fb[1]]                          ↑ vsync 到了！开始读取 fb[0]
                                               此时 fb[0] 只写了一半 → 画面上下不一致
```

当 CPU 写入 framebuffer 的速度慢于 GLCDC 的 vsync 周期（16ms@60Hz）时，GLCDC 会在写入过程中切换到正在写入的缓冲区，导致：

1. 上半部分是新帧数据
2. 下半部分是旧帧数据（还没来得及写入）
3. 每帧都可能出现不同程度的撕裂 → 视觉上表现为闪烁

## 解决方案

**在开始写入前，先等待 vsync 完成**，确保 GLCDC 已经切换到另一个缓冲区，不再读取当前要写入的缓冲区。

### 正确时序

```
CPU:     [等待 vsync] → [写入 fb[0]] → [R_GLCDC_BufferChange(fb[0])]
GLCDC:   [读取 fb[1]]      ↑ vsync     [切换到 fb[0]] → [读取 fb[0]]
                           GLCDC 已切换走，可以安全写入 fb[0]
```

### 代码实现

```c
/* 全局 vsync 帧计数器 (由 DisplayVsyncCallback 递增) */
extern volatile uint32_t g_frame_count;

static void mipi_camera_lcd_task(void *pvParameters)
{
    uint32_t vsync_snapshot;

    while (1) {
        if (有新帧) {
            /* 关键: 等待 vsync，确保 GLCDC 已切换走 */
            vsync_snapshot = g_frame_count;
            while (g_frame_count == vsync_snapshot) {
                vTaskDelay(1);
            }

            /* 现在可以安全写入 framebuffer */
            写入操作...;

            /* 切换 GLCDC 显示到刚写入的缓冲区 */
            R_GLCDC_BufferChange(&g_display0_ctrl, fb, DISPLAY_FRAME_LAYER_1);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
```

## 关键要点

1. **R_GLCDC_BufferChange() 是异步的** — 它只是通知 GLCDC 在下一个 vsync 切换，不是立即切换
2. **必须等待 vsync 后才能写入** — 否则可能写入 GLCDC 正在读取的缓冲区
3. **D-Cache Clean 必须在 BufferChange 之前完成** — 确保 GLCDC 能读到最新数据
4. **VIN 缓冲区需要 D-Cache Invalidate** — DMA 直接写入 SDRAM，CPU 读取前必须刷新缓存

## 为什么 CPU1 上没有这个问题

原始 CPU1 代码没有等待 vsync 也能正常工作，原因是：

1. CPU1 没有开启 D-Cache，写入操作直接到达 SDRAM，速度更快
2. 摄像头和显示器分别在不同 CPU 上，CPU1 专门负责显示，没有其他任务干扰
3. CPU1 的任务调度更简单，写入操作通常能在 16ms 内完成

迁移到 CPU0 后，CPU0 同时负责摄像头采集、AI 推理、显示等多个任务，写入操作可能被其他任务打断，导致无法在一个 vsync 周期内完成。

## 相关文件

- `mipi_camera_lcd.c` — 显示任务，实现 vsync 等待逻辑
- `mipi_camera_test.c` — 摄像头采集任务
- `rgblcd.c` — LCD 驱动，包含 DisplayVsyncCallback 定义

---

# SDRAM 带宽不足导致的黑屏闪烁 Bug 记录

## 问题描述

摄像头画面显示到 RGBLCD 上存在轻微的黑色闪烁——正常画面与黑屏之间快速交替，无画面撕裂。闪烁频率与摄像头帧率相关，每帧都会闪一次。

## 诊断过程（逐层排除）

### 第 1 层：怀疑跨任务竞态条件

**原始架构**：camera 采集任务和 LCD 显示任务是两个独立的 FreeRTOS 任务（相同优先级 3），通过共享变量 `g_cam_frame.frame_addr` / `frame_id` 传递 VIN DMA buffer 地址。

**问题**：Camera 任务写入 `frame_addr` 后递增 `frame_id`，LCD 任务在不同时刻分别读取这两个值。Cortex-M85 的乱序存储可能让 `frame_id` 先于 `frame_addr` 到达 cache，LCD 任务读到"新的 frame_id + 旧的 frame_addr"→ 旧 buffer 可能已被 VIN DMA 覆盖 → 读到全零数据 → 黑屏。

**修复**：合并两个任务为一个统一任务，消除跨任务共享变量。同时添加 `__DSB()` 屏障。

**结果**：比之前好了一些，但仍然闪屏。→ 跨任务竞态不是唯一原因。

### 第 2 层：怀疑 R_GLCDC_BufferChange() 本身产生黑闪

**排查**：阅读 `r_glcdc.c` 中 `R_GLCDC_BufferChange()` 的 FSP 源码——它只是写 FLM2（framebuffer 基地址）shadow register + 设 PVEN 标志在下个 vsync 生效，**没有任何禁用 GLCDC 输出或产生空白期的操作**。

**修复**：改为单缓冲方案——始终使用 `fb_background[0]`，vsync 后"追着电子束"写入，不调用 BufferChange。

**结果**：依然闪。→ BufferChange 本身不是原因。但单缓冲下 GLCDC 扫描和 CPU 写入发生在同一个 framebuffer，AEC 自动曝光导致的帧间亮度差被逐行扫出 → 视觉上就是每帧一闪。

**结论**：回到双缓冲 + BufferChange（确认 BufferChange 是干净的，原始闪屏来自跨任务竞态）。

### 第 3 层：怀疑 D-Cache Clean 突发写占满 SDRAM

**测试**：用纯红色填充替代摄像头旋转（`DIAG_SOLID_COLOR=1`），排除 VIN buffer 读取路径。

**结果**：依然闪。→ 问题不在 VIN/Camera 路径。

**测试**：跳过 D-Cache clean（`DIAG_SKIP_CLEAN=1`），仅用 `__DSB()` 排空 store buffer。

**结果**：依然闪。→ D-Cache clean 突发也非主因。

### 第 4 层：确定 SDRAM 总带宽超限

**测试**：完全关闭 VIN DMA（`DIAG_SKIP_VIN=1`），纯红色 + 不 clean + VIN 关。

**结果**：画面完全不闪！→ **确认 SDRAM 带宽是瓶颈**。

**SDRAM 带宽分析**（16-bit SDRAM，理论 240 MB/s，实际有效 ~65-70% ≈ 155-170 MB/s）：

| 数据源 | 带宽 | 特征 |
| ------ | ---- | ---- |
| GLCDC 读 (1024×600×2, 60Hz) | ~74 MB/s | 持续 |
| VIN DMA 写 (640×480×2, 30fps) | ~18 MB/s | 持续 |
| CPU 读 VIN buffer + 写 framebuffer + cache 逐出 | ~37 MB/s | 突发 |
| **合计** | **~129 MB/s** | |

虽然平均带宽在理论预算内，但 CPU cache 逐出是**突发性**的——瞬间产生大量写请求阻塞 SDRAM 总线，GLCDC 读请求被延迟，FIFO 断流 → 输出黑色像素。

关闭 VIN DMA 后，总带宽降到 ~111 MB/s，突发争抢减到 GLCDC 可容忍范围。

## 根本原因

**16-bit SDRAM 的可用带宽不足以同时支撑 GLCDC (74 MB/s) + VIN DMA (18 MB/s) + CPU framebuffer 操作 (37 MB/s) 三者的峰值并发需求。** CPU cache 逐出的突发写与 VIN DMA 持续写叠加时，GLCDC 读请求得不到及时响应，FIFO 断流产生黑闪。

## 解决方案

### 降低 GLCDC 像素时钟分频比

直接修改 `R_GLCDC->SYSCNT.PANEL_CLK` 寄存器中的 DCDR 字段（bits [5:0]），将分频比从默认的 4 提高到 8：

```c
/* FSP 默认 divisor = 4 → 像素时钟 = PLL/4 → ~60 Hz 刷新 → ~74 MB/s SDRAM 读 */
#define GLCDC_CLK_DIV  8   /* 改为 8 → 像素时钟 = PLL/8 → ~30 Hz → ~37 MB/s */

uint32_t panel_clk = R_GLCDC->SYSCNT.PANEL_CLK;
panel_clk &= ~0x3FU;                    /* 清除 DCDR bits [5:0] */
panel_clk |= (GLCDC_CLK_DIV & 0x3FU);   /* 设置新的分频比 */
R_GLCDC->SYSCNT.PANEL_CLK = panel_clk;
```

### 效果

| 场景 | GLCDC 读 | VIN DMA 写 | CPU 操作 | 总计 | 结果 |
| ---- | -------- | ---------- | -------- | ---- | ---- |
| 修复前 (div=4) | ~74 MB/s | ~18 MB/s | ~37 MB/s | ~129 MB/s | **超限 → 黑闪** |
| 修复后 (div=8) | ~37 MB/s | ~18 MB/s | ~37 MB/s | ~92 MB/s | **安全 → 不闪** |

### 可调参数

`GLCDC_CLK_DIV` 可在 1-63 范围内调整（见 `r_glcdc.h` 中 `glcdc_panel_clk_div_t` 枚举）：

| 分频比 | 约刷新率 | GLCDC 带宽 | 适用场景 |
| ------ | -------- | ---------- | -------- |
| 4 | 60 Hz | ~74 MB/s | 默认值，带宽不足时闪屏 |
| 5 | 48 Hz | ~59 MB/s | 轻度降频 |
| 6 | 40 Hz | ~49 MB/s | 平衡点 |
| 7 | 34 Hz | ~42 MB/s | 较大降频 |
| 8 | 30 Hz | ~37 MB/s | 最大安全余量 |

### 配套改动

在降低 GLCDC 时钟的同时，做了以下配套优化：

1. **合并 camera 和 LCD 任务为单任务** — 消除 `g_cam_frame` 跨任务竞态条件
2. **双缓冲 + BufferChange** — 写入 off-screen buffer，原子切换无撕裂
3. **`write_idx` 初始值 1** — 首帧写入 fb[1] 避免与 GLCDC 正在读取的 fb[0] 冲突
4. **`__DSB()` 屏障** — VIN buffer 无效化后、framebuffer clean 后、BufferChange 后均插入内存屏障
5. **`FRAME_SKIP`** — 可选帧跳过开关，进一步降低 CPU 侧 SDRAM 流量（当前设为 1，不需要）

## 关键要点

1. **SDRAM 带宽是嵌入式多媒体的常见瓶颈**：GPU/Display controller + Camera DMA + CPU 三者同时访问 SDRAM 时竞争不可避免
2. **平均带宽 ≠ 峰值带宽**：即使平均利用率在理论值内，突发访问模式可能导致瞬时争抢
3. **降低 GLCDC 时钟分频是最直接的解决手段**：直接减少单一最大带宽消耗者（GLCDC 占总带宽 ~60%）
4. **分频器寄存器可在运行时修改**：`R_GLCDC->SYSCNT.PANEL_CLK` 的 DCDR 字段不需要重启 GLCDC
5. **分频比为 4 时 60Hz，为 8 时 30Hz**：LCD 面板在 30Hz 下可能出现可见闪烁，建议不低于 30Hz（需要的话可选择 5-7）

## 相关文件

- `mipi_camera_lcd.c` — 统一采集+显示任务，包含 GLCDC 时钟分频修改和 `DIAG_*` 诊断开关
- `mipi_camera_test.c` — 薄封装，转发到 `mipi_camera_lcd_start()`
- `mipi_camera.c` — OV5640 驱动和 VIN 配置
- `rgblcd.c` — LCD 初始化，GLCDC 启动
- `ra/fsp/src/r_glcdc/r_glcdc.c` — FSP GLCDC 驱动源码（`R_GLCDC_BufferChange` 实现，确认无黑闪操作）
- `ra/fsp/inc/instances/r_glcdc.h` — `glcdc_panel_clk_div_t` 枚举定义
