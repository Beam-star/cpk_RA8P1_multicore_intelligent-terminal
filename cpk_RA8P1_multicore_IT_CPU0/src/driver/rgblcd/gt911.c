/**
 ******************************************************************************
 * @file    gt911.c
 * @brief   GT911 电容触摸屏驱动实现 (Goodix GT911, I2C + 硬件中断)
 *
 * 硬件平台: Titan-mini (RA8P1, Cortex-M85)
 *
 * I2C 总线共享:
 *   本驱动与 IMX415 摄像头共享 I2C0 总线 (g_i2c_master0)。
 *   通过从机地址区分: IMX415=0x1A, GT911=0x14。
 *   使用 mipi_i2c.c 导出的 i2c_bus_write/read 函数,
 *   每次传输前自动切换从机地址, 确保多设备共存。
 *
 * 中断 INT 引脚: P007 (BSP_IO_PORT_00_PIN_07)
 *   - pin_data.c 配置: IRQ 使能 + 输入 + 上拉
 *   - e2studio FSP 配置: IRQ28 → g_external_irq28 → gt911_int_isr 回调
 *   - FSP ICU 驱动 (r_icu.c) 处理所有底层细节:
 *     IRQCR 下降沿配置、IELSR 事件链接、NVIC 使能、中断状态清除
 *   - 用户只需: 打开 g_external_irq28 实例 + 实现 gt911_int_isr 回调
 *
 * RST 引脚: P412 (BSP_IO_PORT_04_PIN_12)
 *   - pin_data.c 配置: 输出, 默认低电平
 *   - 高电平 = 释放复位, 低电平 = 复位
 *
 * 参考:
 *   - bsp_gt9147.c/h (IMX6ULL, GT9147, 寄存器兼容)
 *   - gt9xxx.c/h (ESP32-S3, GT911, FreeRTOS)
 *   - GT911 数据手册 v1.3
 ******************************************************************************
 */

#include "gt911.h"
#include "mipi_i2c.h"
#include "common_data.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

/* ======================================================================== */
/*  全局变量                                                                  */
/* ======================================================================== */

gt911_touch_data_t g_gt911_touch = {0};
bool g_gt911_init_ok = false;
volatile uint8_t g_gt911_touch_flag = 0;  /* ISR 回调置 1, gt911_scan 清 0 */

/** 运行时检测到的 GT911 I2C 地址 (7-bit) */
static uint8_t g_gt911_addr = 0;

/** 触摸坐标寄存器地址查找表 (TP1~TP5) */
const uint16_t GT911_TPX_TBL[GT911_MAX_TOUCH_POINTS] = {
    GT911_TP1_REG, GT911_TP2_REG, GT911_TP3_REG, GT911_TP4_REG, GT911_TP5_REG
};

/* ======================================================================== */
/*  GT911 配置表 (184 字节)                                                   */
/*                                                                          */
/*  基于 GT9147 参考配置表适配, 修改分辨率参数为 1024×600。                    */
/*  大多数参数保持参考值 (灵敏度、抖动过滤等已针对 7 寸电容屏优化)。           */
/*                                                                          */
/*  关键字节:                                                                */
/*    [0]     = 0x42  配置版本号 (必须 ≥ flash 中已有版本才更新)              */
/*    [1..2]  = X 输出最大值 (1024 = 0x0400, little-endian)                 */
/*    [3..4]  = Y 输出最大值 (600  = 0x0258, little-endian)                 */
/* ======================================================================== */
static const uint8_t GT911_CFG_TBL[184] = {
    0x42, 0x00, 0x04, 0x58, 0x02, 0x05, 0x0D, 0x00, 0x01, 0x08,
    0x28, 0x05, 0x50, 0x32, 0x03, 0x05, 0x00, 0x00, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x89, 0x28, 0x0A,
    0x17, 0x15, 0x31, 0x0D, 0x00, 0x00, 0x02, 0x9B, 0x03, 0x25,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00,
    0x00, 0x0F, 0x94, 0x94, 0xC5, 0x02, 0x07, 0x00, 0x00, 0x04,
    0x8D, 0x13, 0x00, 0x5C, 0x1E, 0x00, 0x3C, 0x30, 0x00, 0x29,
    0x4C, 0x00, 0x1E, 0x78, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x08, 0x0A, 0x0C, 0x0E, 0x10, 0x12, 0x14, 0x16,
    0x18, 0x1A, 0x00, 0x00, 0x00, 0x00, 0x1F, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0x00, 0x02, 0x04, 0x05, 0x06, 0x08, 0x0A, 0x0C,
    0x0E, 0x1D, 0x1E, 0x1F, 0x20, 0x22, 0x24, 0x28, 0x29, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF
};

/**
 * @brief 发送配置表到 GT911
 *
 * 将 GT911_CFG_TBL 写入 GT911 配置寄存器 (起始地址 0x8047),
 * 附带校验和和更新标记到 CHECK_REG (0x80FF)。
 *
 * I2C 写帧格式 (FSP IIC Master):
 *   [START] [slave+W] [reg_hi] [reg_lo] [data...] [STOP]
 * 即寄存器地址作为数据的前 2 字节。
 *
 * @param mode  0=不保存到 flash (掉电丢失), 1=保存到 flash (永久)
 */
static void gt911_send_cfg(uint8_t mode)
{
    uint8_t checksum = 0;
    int i;

    /* 计算配置表校验和 (所有 184 字节之和取补) */
    for (i = 0; i < (int)sizeof(GT911_CFG_TBL); i++) {
        checksum += GT911_CFG_TBL[i];
    }
    checksum = (~checksum) + 1;

    /*
     * 写入配置表到 GT911 (寄存器 0x8047):
     * 数据格式: [0x80][0x47][184_bytes_config]
     */
    {
        uint8_t cfg_buf[2 + sizeof(GT911_CFG_TBL)];
        cfg_buf[0] = (uint8_t)(GT911_CFGS_REG >> 8);   /* 0x80 */
        cfg_buf[1] = (uint8_t)(GT911_CFGS_REG & 0xFF); /* 0x47 */
        memcpy(&cfg_buf[2], GT911_CFG_TBL, sizeof(GT911_CFG_TBL));
        i2c_bus_write(g_gt911_addr, cfg_buf, sizeof(cfg_buf));
    }

    /*
     * 写入校验和 + 更新标记到 CHECK_REG (0x80FF):
     * 数据格式: [0x80][0xFF][checksum][mode]
     * checksum == 配置表校验和
     * mode    == 0 (不保存 flash) 或 1 (保存到 flash)
     */
    {
        uint8_t check_buf[4] = {
            (uint8_t)(GT911_CHECK_REG >> 8),   /* 0x80 */
            (uint8_t)(GT911_CHECK_REG & 0xFF), /* 0xFF */
            checksum,
            mode
        };
        i2c_bus_write(g_gt911_addr, check_buf, sizeof(check_buf));
    }

    printf("[GT911] Config sent (checksum=0x%02X, flash_save=%d)\r\n",
           checksum, mode);
}

/* ======================================================================== */
/*  内部辅助函数 — I2C 寄存器访问                                             */
/* ======================================================================== */

/**
 * @brief 写 GT911 单个寄存器 (16-bit 地址, 8-bit 数据)
 */
static bool gt911_write_reg(uint16_t reg, uint8_t data)
{
    uint8_t buf[3] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xFF),
        data
    };
    return i2c_bus_write(g_gt911_addr, buf, sizeof(buf));
}

/**
 * @brief 读 GT911 多个连续寄存器 (16-bit 起始地址)
 */
static bool gt911_read_reg(uint16_t reg, uint8_t *data, uint32_t len)
{
    uint8_t addr[2] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xFF)
    };
    return i2c_bus_write_then_read(g_gt911_addr, addr, 2, data, len);
}

/* ======================================================================== */
/*  内部辅助函数 — GPIO 控制                                                   */
/* ======================================================================== */

#define GT911_RST_PIN   BSP_IO_PORT_04_PIN_12   /* GT911 复位引脚: P412 */
#define GT911_INT_PIN   BSP_IO_PORT_00_PIN_07   /* GT911 中断引脚: P007 */

static inline void gt911_rst_set(bool high)
{
    R_IOPORT_PinWrite(&g_ioport_ctrl, GT911_RST_PIN,
                      high ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW);
}

/**
 * @brief 临时将 INT 引脚配置为输出 (用于 GT911 初始化握手)
 */
static void gt911_int_set_output(bool high)
{
    uint32_t cfg = (uint32_t)IOPORT_CFG_PORT_DIRECTION_OUTPUT
                 | (uint32_t)(high ? IOPORT_CFG_PORT_OUTPUT_HIGH
                                   : IOPORT_CFG_PORT_OUTPUT_LOW);
    R_IOPORT_PinCfg(&g_ioport_ctrl, GT911_INT_PIN, cfg);
}

/**
 * @brief 恢复 INT 引脚为输入 + 上拉 + IRQ 模式 (pin_data.c 原始配置)
 */
static void gt911_int_restore_input(void)
{
    uint32_t cfg = (uint32_t)IOPORT_CFG_IRQ_ENABLE
                 | (uint32_t)IOPORT_CFG_PORT_DIRECTION_INPUT
                 | (uint32_t)IOPORT_CFG_PULLUP_ENABLE;
    R_IOPORT_PinCfg(&g_ioport_ctrl, GT911_INT_PIN, cfg);
}

/* ======================================================================== */
/*  FSP ICU 中断回调 (由 r_icu_isr 调用, 函数名对应 common_data.c 中的配置)       */
/* ======================================================================== */

/**
 * @brief GT911 INT 触摸事件回调 (由 FSP ICU 驱动 ISR 调用)
 *
 * e2studio FSP 配置: g_external_irq28 → r_icu_isr() → 此回调。
 *
 * GT911 有触摸时 INT 下降沿 → ICU IRQ28 → r_icu_isr 清除状态
 * → 调用此回调 → 设置 g_gt911_touch_flag = 1。
 *
 * 此回调在 ISR 上下文中执行, 极轻量: 仅设置 volatile 标志。
 * 所有 I2C 触摸坐标读取操作在 gt911_scan() 中由 FreeRTOS 任务完成。
 *
 * @param p_args  FSP 回调参数 (channel + p_context, 此处未使用)
 */
void gt911_int_isr(external_irq_callback_args_t *p_args)
{
    (void)p_args;  /* 未使用回调参数 */

    /* 设置触摸标志, 通知 gt911_scan() 读取坐标 */
    g_gt911_touch_flag = 1;
}

/* ======================================================================== */
/*  Public API — 初始化                                                        */
/* ======================================================================== */

/**
 * @brief 探测 GT911 是否在指定 I2C 地址上响应
 *
 * 尝试读取 PID 寄存器 (0x8140, 4 字节), 检查是否以 "911" 开头。
 *
 * @param addr  7-bit I2C 地址
 * @param pid_out  [out] 读取到的 PID 数据 (至少 4 字节)
 * @return true=探测成功 (PID 以 "911" 开头), false=探测失败
 */
static bool gt911_probe_i2c(uint8_t addr, uint8_t pid_out[4])
{
    uint8_t pid_addr[2] = {
        (uint8_t)(GT911_PID_REG >> 8),
        (uint8_t)(GT911_PID_REG & 0xFF)
    };

    /* Try to read 4 bytes of PID at the given address */
    if (!i2c_bus_write_then_read(addr, pid_addr, 2, pid_out, 4)) {
        return false;
    }

    /* Check if PID starts with "911" */
    if (pid_out[0] == '9' && pid_out[1] == '1' && pid_out[2] == '1') {
        return true;
    }

    return false;
}

/**
 * @brief 初始化 GT911 触摸屏
 *
 * 初始化流程:
 *   1. 打开 I2C0 (如已由摄像头打开则跳过)
 *   2. 硬件复位: RST 脉冲 (INT 保持高阻/上拉 → GT911 使用默认地址 0x5D)
 *   3. I2C 地址探测: 先尝试 0x5D, 再尝试 0x14
 *   4. 读取 PID + 固件版本 + 配置版本
 *   5. 打开 FSP ICU 实例 g_external_irq28 (使能下降沿中断)
 *   6. 发送 GT911 初始化完成命令
 *
 * @return true=成功, false=失败
 */
bool gt911_init(void)
{
    uint8_t pid_data[6] = {0};
    uint8_t cfg_ver = 0;
    fsp_err_t fsp_err;

    printf("[GT911] Initializing GT911 touch controller...\r\n");

    /* ---- 1. 确保 I2C 总线已初始化 ---- */
    printf("[GT911] step1: I2C check...\r\n");
    if (!mipi_i2c_is_ready()) {
        printf("[GT911] step1a: I2C init...\r\n");
        if (!mipi_i2c_init()) {
            printf("[GT911] I2C bus init failed\r\n");
            return false;
        }
    }
    printf("[GT911] step1 done\r\n");

    /* ---- 2. 硬件复位 + INT 握手时序 (参考 ESP32 gt9xxx_init) ----
     *
     * GT911 初始化需要严格的 INT/RST 时序:
     *
     *   INT  ──────┐        ┌──────────────────  (外部上拉恢复)
     *               │________│
     *   RST  ───┐      ┌──────────────────────
     *            │______│
     *           │      │
     *           │      └── tRST ≥ 100μs
     *           │
     *           └───────── INT 低持续 ≥ 50ms
     *                      → GT911 内部固件启动必需!
     *
     * 关键: INT 低电平持续 50ms 不仅是 I2C 地址选择,
     *       更是 GT911 固件初始化完成的必要条件。
     *       参考 ESP32 代码中 gpio_set_level(INT, 0) + vTaskDelay(50)。
     */

    /* 2a. INT 拉低 + RST 拉低 → 开始复位 */
    printf("[GT911] step2: HW reset...\r\n");
    gt911_int_set_output(false);
    vTaskDelay(pdMS_TO_TICKS(5));
    gt911_rst_set(false);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 2b. RST 拉高 → GT911 退出复位, 采样 INT=低 → 锁存 I2C 地址 0x14 */
    gt911_rst_set(true);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 2c. 保持 INT 低 ≥ 50ms → GT911 固件内部初始化 (关键!) */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 2d. 释放 INT (恢复为输入+上拉+IRQ) → GT911 固件接管 INT 引脚 */
    gt911_int_restore_input();
    vTaskDelay(pdMS_TO_TICKS(50));
    printf("[GT911] step2 done\r\n");

    /* ---- 3. I2C 地址探测 ----
     *
     * GT911 的 I2C 地址由 INT 引脚在 RST↑ 时的电平决定:
     *   INT=HIGH → 0x5D (默认, INT 有外部/内部上拉)
     *   INT=LOW  → 0x14 (备选, 需主动拉低 INT)
     *
     * 先尝试默认地址 0x5D, 失败了再尝试备选地址 0x14。
     */
    printf("[GT911] step3: I2C probe...\r\n");
    g_gt911_addr = 0;
    /* INT 已拉低, GT911 应使用备选地址 0x14, 先试它 */
    if (gt911_probe_i2c(GT911_I2C_ADDR_ALT, pid_data)) {
        g_gt911_addr = GT911_I2C_ADDR_ALT;
        printf("[GT911] Detected at I2C addr 0x%02X (alternate, INT=low)\r\n",
               g_gt911_addr);
    } else if (gt911_probe_i2c(GT911_I2C_ADDR_DEFAULT, pid_data)) {
        g_gt911_addr = GT911_I2C_ADDR_DEFAULT;
        printf("[GT911] Detected at I2C addr 0x%02X (default, INT=high)\r\n",
               g_gt911_addr);
    } else {
        printf("[GT911] No GT911 found at 0x%02X or 0x%02X — "
               "check wiring and power\r\n",
               GT911_I2C_ADDR_DEFAULT, GT911_I2C_ADDR_ALT);
        g_gt911_init_ok = false;
        return false;
    }

    /* ---- 4. 读取 PID、固件版本、配置版本 ---- */
    printf("[GT911] step4: read PID/firmware...\r\n");

    /* PID 前 4 字节已在 gt911_probe_i2c 中读取, 再读 2 字节得到完整 6 字节 */
    if (!gt911_read_reg(GT911_PID_REG, pid_data, 6)) {
        printf("[GT911] Failed to read full PID\r\n");
        g_gt911_init_ok = false;
        return false;
    }

    printf("[GT911] PID raw: %02X %02X %02X %02X %02X %02X\r\n",
           pid_data[0], pid_data[1], pid_data[2],
           pid_data[3], pid_data[4], pid_data[5]);

    pid_data[3] = '\0';
    printf("[GT911] Product ID: %s\r\n", pid_data);

    uint16_t fw_ver = ((uint16_t)pid_data[5] << 8) | pid_data[4];
    printf("[GT911] Firmware Version: 0x%04X\r\n", fw_ver);

    /* ---- 5. 读取配置版本 ---- */
    printf("[GT911] step5: read config version...\r\n");
    if (gt911_read_reg(GT911_CFGS_REG, &cfg_ver, 1)) {
        printf("[GT911] Config Version: 0x%02X\r\n", cfg_ver);
    }
    printf("[GT911] step5 done\r\n");

    /* ---- 6. 打开 FSP ICU 外部中断实例 ----
     *
     * g_external_irq28 在 e2studio FSP 中预先配置:
     *   .channel = 28
     *   .trigger = EXTERNAL_IRQ_TRIG_FALLING (下降沿)
     *   .p_callback = gt911_int_isr
     *   .irq = VECTOR_NUMBER_ICU_IRQ28
     *
     * FSP open() 会:
     *   - 配置 IRQCR 寄存器 (下降沿检测)
     *   - 注册回调到 ICU 控制块
     *   - 使能 NVIC 中断
     *
     * 后续 GT911 触摸时: INT↓ → ICU IRQ28 → r_icu_isr()
     * → gt911_int_isr() → g_gt911_touch_flag = 1
     */
    printf("[GT911] step6: ICU open...\r\n");
    fsp_err = g_external_irq28.p_api->open(g_external_irq28.p_ctrl,
                                            g_external_irq28.p_cfg);
    if (fsp_err != FSP_SUCCESS) {
        printf("[GT911] ICU open failed: %d\r\n", (int)fsp_err);
        g_gt911_init_ok = false;
        return false;
    }
    printf("[GT911] IRQ enabled: channel 28, falling edge\r\n");
    printf("[GT911] step6 done\r\n");

    /* ---- 7. 发送配置表 (分辨率、灵敏度等参数) ---- */
    printf("[GT911] step7: send config...\r\n");
    gt911_send_cfg(0);  /* mode=0: 不保存到 flash, 掉电后需重新发送 */
    printf("[GT911] step7 done\r\n");

    /* ---- 8. 软复位并进入正常模式 ----
     *
     * 写 0x02 → GT911 软复位并加载新配置
     * 写 0x00 → 退出复位, 开始正常触摸检测
     */
    if (!gt911_write_reg(GT911_CTRL_REG, 0x02)) {
        printf("[GT911] Soft reset (write 0x02) failed\r\n");
        g_gt911_init_ok = false;
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    if (!gt911_write_reg(GT911_CTRL_REG, 0x00)) {
        printf("[GT911] Exit reset (write 0x00) failed\r\n");
        g_gt911_init_ok = false;
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    /*
     * ---- 9. 清除 GT911 初始中断状态 ----
     *
     * GT911 软复位后可能立即拉低 INT 表示"就绪"。
     * 如果不清除, INT 会一直卡在 LOW:
     *   - ICU 配置为下降沿触发 → 没有 HIGH→LOW 跳变 → ISR 永不触发
     *   - 后续触摸也产生不了新的下降沿
     *
     * 读状态寄存器 → 写 0x00 清除 → GT911 释放 INT 回到 HIGH。
     * 这样下一次触摸时 INT HIGH→LOW 产生干净的下降沿 → ISR 正常触发。
     */
    {
        uint8_t init_status = 0;
        if (gt911_read_reg(GT911_GSTID_REG, &init_status, 1)) {
            if (init_status & 0x80) {
                gt911_write_reg(GT911_GSTID_REG, 0x00);
                printf("[GT911] Cleared initial INT (status was 0x%02X)\r\n",
                       init_status);
            }
        }
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    printf("[GT911] Init OK (I2C addr=0x%02X)\r\n", g_gt911_addr);
    g_gt911_init_ok = true;
    return true;
}

/* ======================================================================== */
/*  Public API — 触摸扫描                                                      */
/* ======================================================================== */

/**
 * @brief 扫描触摸屏 (检查硬件中断标志)
 *
 * GT911 有触摸时: INT↓ → ICU IRQ28 → r_icu_isr → gt911_int_isr
 * → g_gt911_touch_flag = 1 → 本函数检查标志 → 读 I2C 坐标 → 清除标志
 *
 * @return true=有新触摸数据, false=无数据或读取失败
 */
bool gt911_scan(void)
{
    uint8_t status = 0;
    uint8_t buf[4];
    uint8_t i;
    bool result = false;

    if (!g_gt911_init_ok) {
        return false;
    }

    /* 检查中断标志 (ISR 回调设置), fallback 读 INT GPIO 电平 */
    if (g_gt911_touch_flag == 0) {
        bsp_io_level_t int_level = BSP_IO_LEVEL_HIGH;
        R_IOPORT_PinRead(&g_ioport_ctrl, BSP_IO_PORT_00_PIN_07, &int_level);
        if (int_level != BSP_IO_LEVEL_LOW) {
            g_gt911_touch.data_ready = false;
            return false;
        }
    }

    /* 读取触摸状态寄存器 */
    if (!gt911_read_reg(GT911_GSTID_REG, &status, 1)) {
        printf("[GT911] Read status failed\r\n");
        return false;
    }

    /* 检查缓冲区状态标志 (bit7) */
    if (!(status & 0x80)) {
        uint8_t zero = 0;
        gt911_write_reg(GT911_GSTID_REG, zero);
        g_gt911_touch_flag = 0;
        g_gt911_touch.data_ready = false;
        return false;
    }

    /* 触摸点数量 (bits 3-0) */
    g_gt911_touch.point_num = status & 0x0F;

    if (g_gt911_touch.point_num > GT911_MAX_TOUCH_POINTS) {
        g_gt911_touch.point_num = GT911_MAX_TOUCH_POINTS;
    }

    /* 读取每个触摸点的坐标 */
    if (g_gt911_touch.point_num > 0) {
        for (i = 0; i < g_gt911_touch.point_num; i++) {
            if (!gt911_read_reg(GT911_TPX_TBL[i], buf, 4)) {
                printf("[GT911] Read TP%d failed\r\n", i + 1);
                g_gt911_touch.point_num = i;
                break;
            }

            g_gt911_touch.points[i].id      = buf[0] & 0x0F;
            g_gt911_touch.points[i].x       = ((uint16_t)buf[1] << 8) | buf[0];
            g_gt911_touch.points[i].y       = ((uint16_t)buf[3] << 8) | buf[2];
            g_gt911_touch.points[i].pressed = true;
        }

        result = true;
    }

    /* 清除未使用的触摸点 */
    for (i = g_gt911_touch.point_num; i < GT911_MAX_TOUCH_POINTS; i++) {
        g_gt911_touch.points[i].pressed = false;
        g_gt911_touch.points[i].x = 0;
        g_gt911_touch.points[i].y = 0;
    }

    /* 清除 GT911 状态寄存器 (GT911 释放 INT 拉高, 准备下一次触摸) */
    {
        uint8_t zero = 0;
        gt911_write_reg(GT911_GSTID_REG, zero);
    }

    /* 清除中断标志 (准备下一次回调触发) */
    g_gt911_touch_flag = 0;

    g_gt911_touch.data_ready = result;
    return result;
}

/* ======================================================================== */
/*  Public API — 分辨率查询                                                   */
/* ======================================================================== */

/* ======================================================================== */
/*  Public API — 分辨率查询                                                   */
/* ======================================================================== */

uint16_t gt911_get_width(void)
{
    return 1024;
}

uint16_t gt911_get_height(void)
{
    return 600;
}
