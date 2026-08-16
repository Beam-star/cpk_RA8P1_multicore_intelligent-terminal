/**
 ******************************************************************************
 * @file    mipi_i2c.c
 * @brief   MIPI Camera I2C bus wrapper using FSP IIC Master API
 * @note    Shared I2C bus for IMX415 (0x1A) and GT911 touch (0x5D/0x14)
 ******************************************************************************
 */

#include "mipi_i2c.h"
#include "hal_data.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <stdio.h>

/*
 * OV5645 I2C 从机地址 (7-bit)
 * 0x3C = OmniVision OV5645 default address
 */
#define CAM_I2C_ADDR        0x3C

/* I2C 传输超时时间 (毫秒) — FSP IIC Master 为异步 API, 需要等待回调 */
#define I2C_TIMEOUT_MS      100

/*
 * 异步 I2C 传输同步机制
 *
 * FSP IIC Master API 采用异步模式:
 *   调用 write/read → 硬件执行传输 → ISR 回调通知完成
 * 使用 FreeRTOS 二进制信号量实现阻塞等待, 替代 RT-Thread 的 rt_completion
 */
SemaphoreHandle_t g_i2c_bus_sem = NULL;               /* 传输完成信号量 (供所有 I2C 设备共享) */
static volatile bool g_i2c_transfer_done = false;    /* 传输是否已结束 (含成功和失败) */
static volatile bool g_i2c_transfer_success = false; /* 传输是否成功 */

/**
 * @brief I2C 传输完成回调 (由 FSP IIC Master 在 ISR 中调用)
 *
 * FSP 在以下事件时调用此回调:
 *   - I2C_MASTER_EVENT_TX_COMPLETE: 写操作完成
 *   - I2C_MASTER_EVENT_RX_COMPLETE: 读操作完成
 *   - I2C_MASTER_EVENT_ABORTED: 传输中止 (NACK、总线错误等)
 *
 * @param p_args  回调参数, p_args->event 包含事件类型
 *
 * 注意: 函数名必须与 hal_data.c 中 g_i2c_master0_cfg.p_callback 一致
 */
void i2c_master_callback(i2c_master_callback_args_t *p_args)
{
    BaseType_t woken = pdFALSE;
    if (p_args) {
        g_i2c_transfer_done = true;
        /* 只有 TX_COMPLETE 或 RX_COMPLETE 才算成功 */
        g_i2c_transfer_success = (p_args->event == I2C_MASTER_EVENT_TX_COMPLETE ||
                                  p_args->event == I2C_MASTER_EVENT_RX_COMPLETE);
    }
    /* 释放信号量唤醒等待的调用者 */
    if (g_i2c_bus_sem) {
        xSemaphoreGiveFromISR(g_i2c_bus_sem, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

static bool g_i2c_opened = false;

bool mipi_i2c_init(void)
{
    fsp_err_t err;

    if (g_i2c_bus_sem == NULL) {
        g_i2c_bus_sem = xSemaphoreCreateBinary();
        if (g_i2c_bus_sem == NULL) return false;
    }

    /* I2C master may already be open (shared with GT911) — skip re-opening */
    if (!g_i2c_opened) {
        err = g_i2c_master0.p_api->open(&g_i2c_master0_ctrl, &g_i2c_master0_cfg);
        if (err != FSP_SUCCESS) {
            printf("[CAM I2C] open failed: %ld\r\n", (long)err);
            return false;
        }
        g_i2c_opened = true;
    }

    /* Ensure slave address is set to camera */
    err = g_i2c_master0.p_api->slaveAddressSet(&g_i2c_master0_ctrl,
                                                CAM_I2C_ADDR,
                                                I2C_MASTER_ADDR_MODE_7BIT);
    if (err != FSP_SUCCESS) {
        printf("[CAM I2C] slaveAddressSet failed: %ld\r\n", (long)err);
        return false;
    }

    return true;
}

bool mipi_i2c_is_ready(void)
{
    return (g_i2c_bus_sem != NULL);
}

/**
 * @brief 切换 I2C 从机地址
 * @param slave_addr  7-bit I2C 地址
 * @return true=成功
 */
bool i2c_set_slave(uint8_t slave_addr)
{
    fsp_err_t err = g_i2c_master0.p_api->slaveAddressSet(&g_i2c_master0_ctrl,
                                                           slave_addr,
                                                           I2C_MASTER_ADDR_MODE_7BIT);
    return (err == FSP_SUCCESS);
}

/**
 * @brief 阻塞式 I2C 写操作
 *
 * 将 FSP 异步 IIC Master API 封装为阻塞调用:
 *   1. 发起 write 请求 (数据已在 data 缓冲区)
 *   2. 通过信号量等待 ISR 回调 (最多 I2C_TIMEOUT_MS)
 *   3. 返回传输结果
 *
 * @param data  待发送数据 (对于 IMX415: [寄存器地址高字节, 低字节, 数据])
 * @param len   数据长度
 * @return true=成功, false=超时或失败
 */
static bool i2c_write(const uint8_t *data, uint32_t len)
{
    fsp_err_t err;

    g_i2c_transfer_done = false;
    /* false = 发送 STOP 条件 (写操作完成后释放总线)
     * 注: FSP write() 参数声明为 uint8_t *const, 实际不会修改数据,
     *     这里强制转换 const 以匹配函数签名 */
    err = g_i2c_master0.p_api->write(&g_i2c_master0_ctrl, (uint8_t *)data, len, false);
    if (err != FSP_SUCCESS) return false;

    /* 阻塞等待 ISR 回调释放信号量 */
    if (xSemaphoreTake(g_i2c_bus_sem, pdMS_TO_TICKS(I2C_TIMEOUT_MS)) != pdTRUE) {
        return false;  /* 超时 — 可能总线挂死或设备无应答 */
    }
    return g_i2c_transfer_success;
}

/**
 * @brief 阻塞式 I2C 写后读操作 (用于读取 IMX415 寄存器)
 *
 * IMX415 寄存器读取需要两阶段 I2C 传输:
 *   阶段1: [START] → 写寄存器地址 (2字节) → [不发STOP, 发RESTART]
 *   阶段2: [RESTART] → 读数据 (1字节或多字节) → [STOP]
 *
 * FSP write() 的 third 参数控制是否发 STOP:
 *   true  = 不发 STOP (保持总线, 准备 RESTART)
 *   false = 发 STOP (释放总线)
 *
 * @param reg_addr   寄存器地址缓冲区 (2字节, 高字节在前)
 * @param addr_len   地址长度 (固定为2)
 * @param data       读取数据的存放缓冲区
 * @param data_len   要读取的字节数
 * @return true=成功, false=超时或失败
 */
static bool i2c_write_then_read(const uint8_t *reg_addr, uint32_t addr_len,
                                 uint8_t *data, uint32_t data_len)
{
    fsp_err_t err;

    /* 阶段1: 写寄存器地址, 不发 STOP (true = no stop, 保持总线)
     * 注: FSP write() 参数声明为 uint8_t *const, 实际不会修改数据,
     *     这里强制转换 const 以匹配函数签名 */
    g_i2c_transfer_done = false;
    err = g_i2c_master0.p_api->write(&g_i2c_master0_ctrl, (uint8_t *)reg_addr, addr_len, true);
    if (err != FSP_SUCCESS) return false;

    if (xSemaphoreTake(g_i2c_bus_sem, pdMS_TO_TICKS(I2C_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
    if (!g_i2c_transfer_success) return false;

    /* 阶段2: 读数据, 发 STOP (false = stop, 释放总线) */
    g_i2c_transfer_done = false;
    err = g_i2c_master0.p_api->read(&g_i2c_master0_ctrl, data, data_len, false);
    if (err != FSP_SUCCESS) return false;

    if (xSemaphoreTake(g_i2c_bus_sem, pdMS_TO_TICKS(I2C_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
    return g_i2c_transfer_success;
}

/**
 * @brief 写 8-bit 寄存器 (8-bit 地址) — IMX415 SCCB 扩展寄存器
 * @param regID   8-bit 寄存器地址
 * @param regDat  8-bit 写入值
 */
bool wrSensorReg8_8(int regID, int regDat)
{
    /* I2C 写帧: [START] [slave_addr+W] [regID] [regDat] [STOP] */
    uint8_t data[2] = { (uint8_t)regID, (uint8_t)regDat };
    return i2c_write(data, sizeof(data));
}

/**
 * @brief 写 8-bit 寄存器 (16-bit 地址) — IMX415 标准寄存器访问
 * @param regID   16-bit 寄存器地址 (如 0x3008 = SYSTEM_CTROL0)
 * @param regDat  8-bit 写入值
 *
 * I2C 写帧: [START] [0x3C+W] [addr_hi] [addr_lo] [data] [STOP]
 */
bool wrSensorReg16_8(int regID, int regDat)
{
    /* 切换回摄像头 I2C 地址 (共享总线可能被 GT911 改过) */
    if (!i2c_set_slave(CAM_I2C_ADDR)) return false;
    uint8_t data[3] = {
        (uint8_t)(regID >> 8),
        (uint8_t)(regID & 0xFF),
        (uint8_t)regDat
    };
    return i2c_write(data, sizeof(data));
}

/**
 * @brief 读 8-bit 寄存器 (16-bit 地址) — 读取单个寄存器
 * @param regID   16-bit 寄存器地址
 * @param regDat  [out] 读取到的 8-bit 值
 *
 * I2C 传输序列:
 *   [START] [0x3C+W] [addr_hi] [addr_lo]   ← 写寄存器地址
 *   [RESTART] [0x3C+R] [data] [STOP]        ← 读数据
 */
bool rdSensorReg16_8(uint16_t regID, uint8_t *regDat)
{
    /* 切换回摄像头 I2C 地址 (共享总线可能被 GT911 改过) */
    if (!i2c_set_slave(CAM_I2C_ADDR)) return false;
    uint8_t addr[2] = {
        (uint8_t)(regID >> 8),
        (uint8_t)(regID & 0xFF)
    };
    return i2c_write_then_read(addr, 2, regDat, 1);
}

/**
 * @brief 连续读取多个寄存器 (16-bit 地址)
 * @param regID   起始寄存器地址
 * @param regDat  [out] 读取数据的存放缓冲区
 * @param len     要读取的字节数
 *
 * IMX415 支持地址自动递增的连续读取:
 *   [START] [0x3C+W] [addr_hi] [addr_lo]
 *   [RESTART] [0x3C+R] [data0] [data1] ... [dataN] [STOP]
 */
bool rdSensorReg16_Multi(uint16_t regID, uint8_t *regDat, uint32_t len)
{
    uint8_t addr[2] = {
        (uint8_t)(regID >> 8),
        (uint8_t)(regID & 0xFF)
    };
    return i2c_write_then_read(addr, 2, regDat, len);
}

/* ======================================================================== */
/*  通用 I2C 总线访问函数 (供触摸屏等共享 I2C 总线的设备使用)                    */
/* ======================================================================== */

/**
 * @brief 通用 I2C 写操作 (阻塞, 自动设置从机地址)
 *
 * 与 i2c_write() 相同, 但在传输前先调用 slaveAddressSet 切换到目标设备。
 * 适用于共享 I2C 总线的多设备场景 (如 IMX415 + GT911)。
 */
bool i2c_bus_write(uint8_t slave_addr, const uint8_t *data, uint32_t len)
{
    if (!i2c_set_slave(slave_addr)) return false;
    return i2c_write(data, len);
}

/**
 * @brief 通用 I2C 写后读操作 (阻塞, 自动设置从机地址)
 *
 * 先写寄存器地址 (不发 STOP), 再 RESTART 读数据。
 * 适用于 GT911 等 16-bit 寄存器地址的设备。
 */
bool i2c_bus_write_then_read(uint8_t slave_addr,
                              const uint8_t *reg_addr, uint32_t addr_len,
                              uint8_t *rx_data, uint32_t rx_len)
{
    if (!i2c_set_slave(slave_addr)) return false;
    return i2c_write_then_read(reg_addr, addr_len, rx_data, rx_len);
}
