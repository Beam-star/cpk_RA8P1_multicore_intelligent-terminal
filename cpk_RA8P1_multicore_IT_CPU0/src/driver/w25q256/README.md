# W25Q64JV OSPI Flash Driver API 技术手册

RA8P1 平台 W25Q64JV (8MB) QSPI Flash 驱动，基于 Renesas FSP OSPI_B 底层驱动封装。

## 1. 硬件概述

| 参数 | 值 |
| ---- | ---- |
| 型号 | W25Q64JV (Winbond) |
| 容量 | 64 Mbit = 8 MB |
| 页大小 | 256 B |
| 扇区大小 | 4 KB |
| 块大小 | 32 KB / 64 KB |
| JEDEC ID | EF 40 17 |
| 接口 | OSPI0 CS0 |
| 内存映射基址 | 0x90000000 |

## 2. 文件结构

```
w25q64/
├── w25q64.h          驱动头文件（宏定义、类型、API 声明）
├── w25q64.c          驱动实现
├── w25q64_test.h     测试头文件
├── w25q64_test.c     功能测试 + 速度基准测试
└── README.md         本文档
```

## 3. e2studio 配置要求

| 配置项 | 值 | 说明 |
| ---- | ---- | ---- |
| OSPI_B Unit | 0 | |
| Channel | 0 (CS0) | |
| SPI Protocol | **1S-4S-4S** | Quad SPI 模式 |
| Page Size Bytes | **256** | 与 W25Q64 实际页大小一致 |
| Erase Commands | 0x20 (4KB), 0x52 (32KB), 0x60 (Chip) | |
| Read Command | 0xE8 | Quad Read |
| Write Command | 0x32 | Quad Page Program |
| **Status Dummy Cycles** | **0** | 必须为 0（见 9 节） |
| Write Enable Bit | 1 | WEL 在 SR1 bit 1 |
| **Combination Function** | **64_BYTE** | 关键! 与驱动内 64 字节分块配合（见 10 节） |
| **Prefetch Function** | **Enabled** | 加速内存映射读取 |
| **DMAC Support** | **Disabled** | DMAC 路径有 PUSH 缺失 bug（见 10.1 节） |
| Address MSB Mask | **0x00** | 两个命令集都必须设为 0x00，W25Q64 不需要地址替换 |

> **重要**:
> - **`Combination Function` 必须设为 `64_BYTE`**。驱动内部将每次 `R_OSPI_B_Write` 调用限制在 64 字节以内，与一个 combination burst 大小精确匹配。设为 Disable 或更小值会导致每次调用拆成多个 SPI 事务（见 10 节）。
> - **`Status Dummy Cycles` 必须设为 0**。如果设为 3，Quad 模式下状态寄存器读取会插入 3 个 dummy 周期，导致 WEL 位偏移，`R_OSPI_B_Write` 返回 `FSP_ERR_NOT_ENABLED`（err=19）。详见 9 节。

## 4. API 参考

### 4.1 初始化

```c
w25q64_err_t w25q64_open(void);   // 打开 OSPI 驱动（1S-4S-4S 模式）
w25q64_err_t w25q64_close(void);  // 关闭驱动
```

### 4.2 设备信息

```c
w25q64_err_t w25q64_read_jedec_id(w25q64_jedec_id_t *p_id);
w25q64_err_t w25q64_read_status_reg1(uint8_t *p_status);
```

### 4.3 擦除

```c
w25q64_err_t w25q64_erase_sector(uint32_t addr);     // 4KB 扇区擦除
w25q64_err_t w25q64_erase_block_64k(uint32_t addr);  // 64KB 块擦除
w25q64_err_t w25q64_erase_chip(void);                 // 整片擦除（最长 200s）
```

### 4.4 读写

```c
w25q64_err_t w25q64_write(uint32_t addr, const uint8_t *p_data, uint32_t length);
w25q64_err_t w25q64_read(uint32_t addr, uint8_t *p_data, uint32_t length);
w25q64_err_t w25q64_write_and_verify(uint32_t addr, const uint8_t *p_data, uint32_t length);
```

- `addr`: Flash 内部偏移（0 ~ 0x007FFFFF），非绝对地址
- `w25q64_write` 自动处理 256 字节页边界
- `w25q64_read` 通过内存映射直接读取，零开销
- 写入前必须先擦除（Flash 只能 1→0）

### 4.5 协议模式切换

```c
w25q64_err_t w25q64_set_spi_mode(void);   // 切换到 1S-1S-1S (SPI)
w25q64_err_t w25q64_set_quad_mode(void);  // 切换到 1S-4S-4S (Quad)
```

内部自动处理可写配置副本和 `write_enable_bit` 设置：

- SPI 模式：`write_enable_bit = 1`
- Quad 模式：`write_enable_bit = 5`（在 `R_OSPI_B_SpiProtocolSet` 之后设置）

### 4.6 工具

```c
w25q64_err_t w25q64_wait_busy(void);
const char *  w25q64_err_str(w25q64_err_t err);
```

### 4.7 内存映射直接读取

```c
uint8_t  val8  = W25Q64_READ_U8(0x1000);    // 读 1 字节
uint32_t val32 = W25Q64_READ_U32(0x1000);   // 读 4 字节
```

## 5. 错误码

| 枚举值 | 含义 |
| ---- | ---- |
| `W25Q64_OK` | 操作成功 |
| `W25Q64_ERR_OPEN` | OSPI 驱动打开失败 |
| `W25Q64_ERR_ERASE` | 擦除操作失败 |
| `W25Q64_ERR_WRITE` | 写入操作失败 |
| `W25Q64_ERR_READ` | 读取操作失败 |
| `W25Q64_ERR_BUSY_TIMEOUT` | 等待 BUSY 超时 |
| `W25Q64_ERR_VERIFY` | 回读校验不匹配 |
| `W25Q64_ERR_ADDR_RANGE` | 地址超出 8MB |

## 6. 测试

### 6.1 功能测试 (`w25q64_test_run`)

```c
#include "w25q64_test.h"
w25q64_test_run();
```

测试项：Open → JEDEC ID → SR1 → Erase Sector → Write 256B → Read-back Verify → Write+Verify 128B

使用最后一个扇区（0x007FF000），不会覆盖固件。

### 6.2 速度基准测试 (`w25q64_test_speed`)

```c
w25q64_test_speed();
```

在 1S-4S-4S（Quad SPI）模式下测试 4MB 读写吞吐量。

测试流程：

1. 32KB 块擦除 4MB
2. 4KB 块写入 4MB（`R_OSPI_B_Write` 直接调用）
3. 内存映射读取 4MB

预期性能：

| 操作 | 速度 |
| ---- | ---- |
| 写入 | ~3000-6000 KB/s |
| 读取 | ~12000-16000 KB/s |
| 32KB 块擦除 | ~10-15 s / 4MB |

> **注意**: 速度测试会擦除并写入 Flash 前 4MB 区域，会覆盖该区域数据。

## 7. 典型使用

```c
#include "w25q64.h"

void example(void)
{
    w25q64_open();

    w25q64_jedec_id_t id;
    w25q64_read_jedec_id(&id);

    w25q64_erase_sector(0x1000);
    uint8_t data[256] = {0x00, 0x01, 0x02};
    w25q64_write(0x1000, data, sizeof(data));

    uint8_t readback[256];
    w25q64_read(0x1000, readback, sizeof(readback));

    w25q64_close();
}
```

## 8. 依赖

- Renesas FSP `r_ospi_b` 驱动
- `hal_data.h` / `hal_data.c`（OSPI 实例 `g_ospi0`）
- `perf_counter` 库（仅速度测试需要）

## 9. 故障排查：status_dummy_cycles 与 err=19

### 问题现象

`R_OSPI_B_Write` 在 Quad 模式 (1S-4S-4S) 下返回 `FSP_ERR_NOT_ENABLED`（err=19），但 SPI 模式正常工作。

### 根因

FSP OSPI_B 驱动在发出 Write Enable 命令 (0x06) 后，需要通过读状态寄存器来确认 WEL 位是否被置位。读取方式由当前命令集（command set）的 `status_dummy_cycles` 决定：

```text
r_ospi_b_write_enable()
  → 发送 0x06 (Write Enable)
  → r_ospi_b_status_sub()
    → 发送 0x05 (Read Status Register)
    → 插入 N 个 dummy 周期 (status_dummy_cycles)
    → 读取 1 字节状态数据
    → 检查 (data >> write_enable_bit) & 1
```

**Quad 模式下 `status_dummy_cycles` 对比：**

| status_dummy_cycles | 状态读取时序 | WEL 位位置 | 结果 |
| --- | --- | --- | --- |
| **0**（正确） | 0x05 → addr → **立即读** 1 字节 | bit 1 | ✓ WEL 正确检出 |
| **3**（错误） | 0x05 → addr → **3 dummy 周期** → 读 1 字节 | 偏移到其他位 | ✗ err=19 |

W25Q64 的状态寄存器读取不支持 dummy 周期。当 `status_dummy_cycles = 3` 时，OSPI 控制器在 Quad I/O 总线上插入 3 个 dummy 周期（4 线 × 3 时钟 = 12 个 dummy 位），导致状态字节在返回数据中偏移。`write_enable_bit = 1` 无法正确命中 WEL 位，驱动判定 Write Enable 失败。

### 修复

在 e2studio 的 OSPI 配置中，将 1S-4S-4S 命令集的 **Status Dummy Cycles** 改为 **0**。

## 10. 故障排查：R_OSPI_B_Write 写入数据不正确（已修复）

### 现象

`R_OSPI_B_Write` 返回 `FSP_SUCCESS`，但回读数据只有前几个字节匹配，其余全是 `0xFF`（擦除后默认值）或部分写入。

典型测试输出（256 字节写入，Combination = 64BYTE 时）：

```text
Wr[0..7]:   00 01 02 03 04 05 06 07   (期望值)
Rd[0..7]:   00 01 02 03 04 05 06 07   (前 64B 匹配 ✓)
Rd[64..71]: FF FF FF FF FF FF FF FF   (第 2 个 burst: 全部丢失 ✗)
Rd[128..135]: FF FF FF FF FF FF FF FF (第 3 个 burst: 全部丢失 ✗)
Rd[192..199]: FF FF FF FF FF FF FF FF (第 4 个 burst: 全部丢失 ✗)
```

### 根因

在分析根因之前，先解释几个关键术语：

| 术语 | 全称 | 含义 |
| ---- | ---- | ---- |
| **WREN** | Write Enable | SPI Flash 命令 (0x06)，用于置位 Flash 芯片内部的 WEL (Write Enable Latch) 锁存器。W25Q64 要求在执行任何写入/擦除操作之前必须先发 WREN 将 WEL 置 1，操作完成后 WEL 自动归零。 |
| **PP** | Page Program | SPI Flash 命令 (0x02)，将数据写入 Flash 的页缓冲区并启动编程。W25Q64 每页 256 字节，一次 PP 可写入 1~256 字节（必须在同一页内）。PP 执行后 WEL 自动清除。 |
| **burst** | 组合突发 | XSPI 控制器的 Combination Function 将 CPU 连续多次小写入（如 8 字节 store）合并为一次 SPI 总线事务。一个 burst 对应一次完整的 SPI 帧：CS 拉低 → 命令+地址+数据 → CS 拉高。burst 大小由 Combination Function 决定，最大 64 字节。 |

**核心矛盾**：一次 `R_OSPI_B_Write` 调用只发 **一次 WREN**，但 XSPI 硬件可能将其拆成 **多次 SPI PP**（取决于写入大小和 Combination 设置）。第一次 PP 执行后 WEL 归零，后续 PP 因缺少 WEL 被 Flash 芯片静默忽略。

```c
// r_ospi_b.c CPU 写入路径（简化）
r_ospi_b_write_enable(p_ctrl);        // ← 只发一次 WREN! WEL=1
while (byte_count >= 8) {
    *p_dest64 = *p_src64;             // 每次 8B store → XSPI 内存映射区
    p_dest64++; p_src64++;
    byte_count -= 8;
}
p_reg->BMCTL1 = PUSH;
```

XSPI 控制器的行为取决于 **Combination Function** 设置：

- **Combination = DISABLE**：每次 8 字节 store 立即触发一次独立 SPI 事务（Page Program 0x02 + addr + 8B）。256 字节 = 32 次独立 PP。
- **Combination = 64BYTE**：每 8 次 store（64 字节）合并为一个 burst，自动 flush 为一次 SPI PP。256 字节 = 4 次 PP。
- **Combination 最大值仅 64BYTE**（枚举 `OSPI_B_COMBINATION_FUNCTION_64BYTE = 0x1F`），无法覆盖 W25Q64 的 256 字节页。

**W25Q64（以及所有 W25Q 系列）在每次 Page Program 完成后自动清除 WEL 位。** 所以：

```text
SPI 总线时序（Combination = 64BYTE, 256B 写入）:
  WREN(0x06) → WEL=1
  PP(0x02)+addr+0x00..0x3F  → CS↑ → 编程中 → WEL=0 ✓ (64B 成功)
  PP(0x02)+addr+0x40..0x7F  → CS↑ → WEL=0 → 被芯片忽略 ✗
  PP(0x02)+addr+0x80..0xBF  → CS↑ → WEL=0 → 被芯片忽略 ✗
  PP(0x02)+addr+0xC0..0xFF  → CS↑ → WEL=0 → 被芯片忽略 ✗
```

**结论**：无论 Combination 设为 Disable 还是 64BYTE，`R_OSPI_B_Write` 写入超过一个 burst 大小的数据时，只有第一个 burst 被真正写入 Flash。

### 修复方案

利用 **每次 `R_OSPI_B_Write` 调用都会执行一次 `r_ospi_b_write_enable()`** 的特性，在 `w25q64_write()` 中将页内写入进一步拆分为 64 字节（一个 combination burst）的块：

```c
// w25q64.c — 页内 64 字节分块
while (length > 0) {
    uint32_t page_chunk = min(length, page_remaining);   // ≤ 256B

    while (page_chunk > 0) {
        uint32_t burst_chunk = min(page_chunk, 64);      // ≤ 64B = 1 burst

        // 每次 R_OSPI_B_Write 调用 → 独立 WREN → 独立 PP → 正确!
        R_OSPI_B_Write(&g_ospi0_ctrl, p_data,
                       W25Q64_MEM_BASE + addr, burst_chunk);
        w25q64_wait_busy();

        addr += burst_chunk;
        p_data += burst_chunk;
        page_chunk -= burst_chunk;
    }
}
```

SPI 总线时序变为：

```text
  R_OSPI_B_Write(64B) #1:
    WREN → WEL=1 → PP(64B) → WEL=0 ✓
  R_OSPI_B_Write(64B) #2:
    WREN → WEL=1 → PP(64B) → WEL=0 ✓
  R_OSPI_B_Write(64B) #3:
    WREN → WEL=1 → PP(64B) → WEL=0 ✓
  R_OSPI_B_Write(64B) #4:
    WREN → WEL=1 → PP(64B) → WEL=0 ✓
```

**每次 PP 之前都有独立的 WREN，全部 256 字节正确写入。**

### 必要的 e2studio 配置（与 R_OSPI_B_Write 配合）

| 配置项 | 值 | 原因 |
| ---- | ---- | ---- |
| Combination Function | **64_BYTE** | 8 次 CPU store 合并为 1 次 SPI PP（而非 1 次 store = 1 次 PP） |
| DMAC Support | **Disabled** | DMAC 路径有 PUSH 缺失 bug，且同样存在 WEL 问题 |
| Prefetch Function | **Enabled** | 加速内存映射读取 |
| Address MSB Mask | **0x00** | 两个命令集都必须设 0x00 |

### 注意事项

1. **`R_OSPI_B_Write` 要求写入大小是 8 的倍数，且目标地址 8 字节对齐。** 当前分块使用 64 字节（满足 8 字节对齐）。如果调用者传入非对齐地址或非 8 倍数的长度，需要额外处理（如用 DirectTransfer 补齐不对齐的头尾）。

2. **Combination Function 必须与代码中的 `W25Q64_BURST_SIZE` (64) 一致。** 如果在 e2studio 中改了 Combination 大小，需要同步修改代码。

3. **此方案仍使用 `R_OSPI_B_Write` API**，未来 FatFS 集成时如果 FSP block media 直接调用 `R_OSPI_B_Write` 且写入量 > 64 字节，需要在 block media 层或 `w25q64_write` 层做同样的分块处理。

### 备选方案：DirectTransfer（WRITE_CUSTOM = 1）

设置 `WRITE_CUSTOM 1` 可以切换到完全使用 `R_OSPI_B_DirectTransfer` 的写入路径：

- 每次循环：WREN → PP(4 字节) → Wait Busy
- 256 字节需要 64 次循环
- 写入速度 ~10-30 KB/s（vs chunked `R_OSPI_B_Write` 的 ~100-200 KB/s）
- 不依赖 Combination Function 设置
- 适用于调试或 Combination Function 不可用的场景

### 10.1 补充：DMAC 路径的 PUSH 缺失 bug

`R_OSPI_B_Write` DMAC 路径在 DMA 传输完成后，仅当 `byte_count < combo_bytes` 时才写 `BMCTL1` 的 PUSH 位：

```c
// r_ospi_b.c DMAC 路径
if (byte_count < combo_bytes)           // combo_bytes = 64
{
    p_reg->BMCTL1 = ...PUSH...;         // 仅当 < 64B 才 PUSH
}
```

当 `byte_count >= 64` 时，PUSH 被跳过，数据留在 XSPI 缓冲区但不触发 SPI 事务。这是 FSP 的已知 bug，RA8P1 硬件手册中规定 PUSH 必须在所有内存映射写入后执行。当前配置中 DMAC 已禁用，不受影响。
