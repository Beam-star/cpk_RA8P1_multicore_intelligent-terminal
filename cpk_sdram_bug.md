# CPK SDRAM 调试记录 — 画面重复（别名）与噪点残影

## 问题概述

从 Titan-mini RA8P1 项目移植到 CPK 板后，GLCDC 双图层显示出现两个独立问题：

1. **画面水平重复**：Layer 2 (384×600 LVGL 面板) 内容水平方向出现 3 次重复
2. **数码卡顿残影/噪点**：渐变和复杂图像有明显噪点，纯色正常

---

## 问题 1：SDRAM 地址别名 → 画面重复

### 症状

- Layer 2 LVGL UI 水平方向渲染 3 次
- 开机 Logo 右半边也重复
- **绕过 D/AVE 2D 直接用 CPU 写 `fb_foreground` 也重复**（排除了 GPU/渲染问题）
- **绕过所有软件直接 CPU 写 SDRAM 再回读也重复**（排除了 GLCDC 硬件问题）

### 诊断过程

在 `cpu0main_thread_entry.c` 中编写 SDRAM 地址别名测试，直接向 `fb_background` 不同偏移写唯一值后回读：

```
向 fb[0] 写 0x1234
向 fb[512] 写 0xABCD   (512像素偏移 = 1024字节)
读回 fb[0]→0xABCD     ← 被 512px 处的写入覆盖了！
```

**确认为 1024 字节（512 像素）地址别名**：写入地址 X 的数据覆盖了地址 X-1024 的数据。

进一步测试 SDRAM 控制器的 MXC（列宽）设置：

| SDADR.MXC | 列宽 | 别名边界 | 结果 |
|-----------|------|---------|------|
| 0 | 512 B | 512 B | **OK** ✓ |
| 1 | 1024 B | 1024 B | **ALIAS** ✗ |
| 2 | 2048 B | 2048 B | **OK** ✓ |

**MXC=1（FSP 默认 addr_shift=9，8 位列）出问题，MXC=0 和 MXC=2 正常。**

### 根因

CPK 板上的 SDRAM 芯片只有 **7 位列地址位**（128 列 × 4 字节 = **512 字节/行**），但 FSP 默认配置 `addr_shift=9` 假定有 **8 位列地址位**（256 列 × 4 字节 = 1024 字节/行）。

在这个错误配置下，SDRAM 控制器的列/行地址分界线在 CPU 地址 bit 10（1024 字节边界），但 SDRAM 芯片只有 7 位列地址，bit 9 才是实际的列/行分界。bit 10 这个"多余"的列地址位在 SDRAM 芯片上被当作行地址 bit 0 处理——但这个引脚实际上不被 SDRAM 芯片识别，导致地址每 1024 字节就回绕到同一物理存储单元。

```
期望 (addr_shift=9, 8列位): | 列[7:0] CPU[9:2] | 行[11:0] CPU[21:10] |
实际 (addr_shift=8, 7列位): | 列[6:0] CPU[8:2] | 行[11:0] CPU[20:9]  |
```

错误配置下 bit 10 处于"真空地带"——SDRAMC 认为是行地址，但 SDRAM 芯片早已完成了列寻址。bit 10 的变化不影响实际 SDRAM 物理地址，造成 1024 字节粒度的别名。

### 修复

**e2studio FSP 配置器 → BSP → SDRAM → Address Shift = 8**（对应 SDADR.MXC=0，7 位列地址，512 字节/行）。

涉及的生成文件：
- `configuration.xml`: `sdram.addr_shift` = `addr_shift.8`
- `ra_cfg/fsp_cfg/bsp/bsp_mcu_family_cfg.h`: `BSP_CFG_SDRAM_MULTIPLEX_ADDR_SHIFT` = `0`
- 硬件寄存器：`R_BUS->SDRAM.SDADR` = 0

### 注意事项

- Titan-mini 使用 `addr_shift=9` 是正确的（Titan 的 SDRAM 有 8 位列）
- CPK 必须使用 `addr_shift=8`（SDRAM 芯片只有 7 位列）
- 如果 CPK 更换 SDRAM 芯片，需要根据新芯片的列地址宽度重新调整此参数
- **与 PC15(P608)、PD00 引脚的 Bank Select 配置无关**——这些引脚在 RA8P1 上复用为 `sdram.a14/a15` 是正确的（Renesas 标准设计）

---

## 问题 2：D-Cache 一致性 → 噪点残影

### 症状

- 纯色画面正常（如全绿），但渐变和复杂图像有明显的"数码卡顿"式噪点
- 噪点看起来像前几帧的残影叠加在当前画面上
- 所有图层和内容都受影响（开机 Logo、条纹测试图案等）

### 诊断

在 SDRAM 别名修复后，噪点仍然存在。分析数据流：

```
CPU 写帧缓冲 → D-Cache (写回) → SDRAM
GLCDC 显示 ← 直接读 SDRAM (绕过 D-Cache)
```

当 `CAMERA_ENABLE=1` 时，D-Cache 在 `rpmsg_core_init()` 后重新开启。帧缓冲位于 SDRAM，属于 Cacheable 区域。CPU 写入进入 D-Cache，需要显式 `SCB_CleanDCache_by_Addr()` 刷到 SDRAM 才能被 GLCDC 看到。

现有代码确实有 Clean 调用（`boot_logo.c`、`cpu0main_thread_entry.c`），但 Clean 操作存在时序窗口问题：
1. CPU 写数据 → Cache line dirty
2. SCB_CleanDCache → 数据写入 SDRAM
3. CPU 继续写同一区域 → Cache line 再次 dirty
4. 旧的 dirty line 被 Cache 替换算法写回 → 覆盖了步骤 3 的新数据

### 修复

在 `src/rpmsg/rpmsg_core.c` 中增加第二个 MPU region，将 SDRAM 帧缓冲区域设为 **non-cacheable**：

```c
// Region 1: SDRAM framebuffer area — RW, XN, non-cacheable
#define SDRAM_NC_BASE         0x68000000UL
#define SDRAM_NC_SIZE         0x00800000UL  // 8MB
#define SDRAM_NC_MPU_REGION   1U
#define SDRAM_NC_MPU_ATTR_IDX 1U

ARM_MPU_SetMemAttr(SDRAM_NC_MPU_ATTR_IDX,
                   ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE,
                                ARM_MPU_ATTR_NON_CACHEABLE));
ARM_MPU_SetRegion(SDRAM_NC_MPU_REGION,
                  ARM_MPU_RBAR(SDRAM_NC_BASE, ARM_MPU_SH_OUTER, 0U, 1U, 1U),
                  ARM_MPU_RLAR(SDRAM_NC_BASE + SDRAM_NC_SIZE - 1UL,
                               SDRAM_NC_MPU_ATTR_IDX));
```

Non-cacheable 区域覆盖范围：`0x68000000` – `0x687FFFFF`（8MB），包含：
- `fb_background[0..1]` — GLCDC Layer 1 帧缓冲
- `fb_foreground[0..1]` — GLCDC Layer 2 帧缓冲
- VIN DMA 缓冲区
- NPU tensor arena 和模型数据

**效果**：CPU 写直达 SDRAM，GLCDC 始终读到最新数据。不再需要 `SCB_CleanDCache_by_Addr()`。

---

## 修改文件清单

| 文件 | 修改内容 | 问题 |
|------|----------|------|
| `configuration.xml` | `addr_shift` 9→8 | 问题 1 |
| `ra_cfg/fsp_cfg/bsp/bsp_mcu_family_cfg.h` | `BSP_CFG_SDRAM_MULTIPLEX_ADDR_SHIFT` 1→0（自动生成） | 问题 1 |
| `src/rpmsg/rpmsg_core.c` | 新增 SDRAM NC MPU region | 问题 2 |
| `CLAUDE.md` | 记录两个问题和修复方案 | 文档 |

## 未修改文件（排查过但确认正确）

| 文件 | 说明 |
|------|------|
| `ra/lvgl/.../lv_draw_dave2d_utils.c` | `d2_framebuffer_from_layer` 的 pitch 计算正确（像素值） |
| `ra/lvgl/.../lv_draw_dave2d_image.c` | `d2_framebuffer` 的 pitch 计算正确 |
| `ra/fsp/src/rm_lvgl_port/rm_lvgl_port.c` | flush callback 的 cache 操作正确 |
| `ra_gen/common_data.c/h` | GLCDC Layer 2 配置正确（hsize=384, hstride=384） |
| `src/lvgl_ui/lvgl_ui_assets.c` | 资产加载逻辑正确 |
| `src/driver/mipi_camera/mipi_i2c.c` | I2C 共享总线地址切换逻辑正确 |
