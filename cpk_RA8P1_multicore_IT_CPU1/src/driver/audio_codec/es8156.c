/**
 ******************************************************************************
 * @file    es8156.c
 * @brief   ES8156 音频 DAC — I2C1 控制 (7-bit 地址 0x08, ADDR 引脚接地)
 *
 * 硬件: IIC1 (SCL=P512, SDA=P511, 100kHz), SSI0 I2S 数据, MCLK=GPT2 6.144MHz
 * 输出: 3.5mm 耳机 / 扬声器
 *
 * 寄存器序列取自已验证的参考工程 (Titan_Mini_wavplayer SDK es8156.c)。
 *
 * FSP 的 R_IIC_MASTER_Write/Read 是异步接口:必须等回调报告传输完成后才能
 * 发起下一次传输,否则返回 FSP_ERR_IN_USE(err=8)。configuration.xml 中
 * IIC1 的 p_callback 为 NULL,这里用 R_IIC_MASTER_CallbackSet() 在运行时
 * 注册回调(避免改动 ra_gen 生成代码),配合信号量实现同步读写。
 ******************************************************************************
 */

#include "es8156.h"
#include "hal_data.h"
#include "r_iic_master.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <stdio.h>

#define ES8156_ADDR       0x08    /* 7-bit, ADDR 引脚接地 (0x09 = 接 VDD)   */
#define ES8156_CHIP_ID    0x81    /* 寄存器 0xFD 期望值                      */
#define ES8156_VOL_0DB    191     /* 寄存器 0x14: 191 = 0dB, ±0.5dB/step     */
#define ES_I2C_TIMEOUT_MS 100

/* ---- 同步 I2C 状态 ---- */
static SemaphoreHandle_t            g_i2c_sem   = NULL;
static volatile i2c_master_event_t  g_i2c_event = (i2c_master_event_t)0;
static uint8_t                      g_cur_vol   = ES8156_VOL_0DB;

/* ---- I2C 完成回调 (ISR 上下文) ---- */
static void es_i2c_callback(i2c_master_callback_args_t *p_args)
{
    BaseType_t woken = pdFALSE;
    g_i2c_event = p_args->event;
    if (g_i2c_sem) {
        xSemaphoreGiveFromISR(g_i2c_sem, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

/* ---- 等待一次传输完成 ---- */
static bool es_i2c_wait(void)
{
    if (xSemaphoreTake(g_i2c_sem, pdMS_TO_TICKS(ES_I2C_TIMEOUT_MS)) != pdTRUE) {
        printf("[ES8156] I2C timeout\r\n");
        return false;
    }
    if (g_i2c_event == I2C_MASTER_EVENT_ABORTED) {
        printf("[ES8156] I2C aborted (NACK?)\r\n");
        return false;
    }
    return true;
}

/* ---- 内部: 写寄存器 (同步) ---- */
static bool es_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    fsp_err_t err = R_IIC_MASTER_Write(&g_i2c_master1_ctrl, buf, 2, false);
    if (err != FSP_SUCCESS) {
        printf("[ES8156] I2C write reg 0x%02X failed: %ld\r\n",
               (unsigned)reg, (long)err);
        return false;
    }
    return es_i2c_wait();
}

/* ---- 内部: 读寄存器 (同步) ---- */
static bool es_read(uint8_t reg, uint8_t *val)
{
    fsp_err_t err = R_IIC_MASTER_Write(&g_i2c_master1_ctrl, &reg, 1, true);
    if (err != FSP_SUCCESS || !es_i2c_wait()) return false;
    err = R_IIC_MASTER_Read(&g_i2c_master1_ctrl, val, 1, false);
    if (err != FSP_SUCCESS) return false;
    return es_i2c_wait();
}

/* ======================================================================== */
/*  Public                                                                   */
/* ======================================================================== */

bool es8156_init(void)
{
    printf("[ES8156] Init via IIC1 (addr 0x%02X)...\r\n", ES8156_ADDR);

    if (!g_i2c_sem) {
        g_i2c_sem = xSemaphoreCreateBinary();
        if (!g_i2c_sem) return false;
    }

    /* Open IIC1 (容忍重复初始化) */
    fsp_err_t err = R_IIC_MASTER_Open(&g_i2c_master1_ctrl, &g_i2c_master1_cfg);
    if (err != FSP_SUCCESS && err != FSP_ERR_ALREADY_OPEN) {
        printf("[ES8156] I2C open failed: %ld\r\n", (long)err);
        return false;
    }

    /* 运行时注册回调 (FSP 配置里 p_callback=NULL, 不改 ra_gen) */
    err = R_IIC_MASTER_CallbackSet(&g_i2c_master1_ctrl,
                                   es_i2c_callback, NULL, NULL);
    if (err != FSP_SUCCESS) {
        printf("[ES8156] CallbackSet failed: %ld\r\n", (long)err);
        return false;
    }

    err = R_IIC_MASTER_SlaveAddressSet(&g_i2c_master1_ctrl,
                                       ES8156_ADDR, I2C_MASTER_ADDR_MODE_7BIT);
    if (err != FSP_SUCCESS) {
        printf("[ES8156] SlaveAddressSet failed: %ld\r\n", (long)err);
        return false;
    }

    /* ---- 参考工程验证过的初始化序列 (顺序精确, 无需延时) ---- */
    static const uint8_t seq[][2] = {
        {0x02, 0x04},   /* mode control                                  */
        {0x13, 0x00},
        {0x20, 0x2A},   /* analog: PA/LOUT path, HP driver off           */
        {0x21, 0x3C},
        {0x22, 0x02},   /* charge pump                                   */
        {0x24, 0x07},   /* analog trim                                   */
        {0x23, 0x40},   /* VDDA select: 3.3V                             */
        {0x0A, 0x01},
        {0x0B, 0x01},
        {0x06, 0x11},
        {0x18, 0x10},
        {0x11, 0x30},   /* IF format: 16-bit, normal I2S                 */
        {0x0D, 0x14},   /* DAC ctrl                                      */
        {0x18, 0x08},
        {0x19, 0x00},
        {0x08, 0x3F},   /* DAC power                                     */
        {0x00, 0x02},   /* soft reset                                    */
        {0x00, 0x03},   /* release reset / start                         */
        {0x25, 0x20},   /* output enable                                 */
    };
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        if (!es_write(seq[i][0], seq[i][1])) {
            printf("[ES8156] Init failed at step %u (reg 0x%02X)\r\n",
                   (unsigned)i, (unsigned)seq[i][0]);
            return false;
        }
    }

    /* 默认音量 0dB */
    g_cur_vol = ES8156_VOL_0DB;
    if (!es_write(0x14, g_cur_vol)) return false;

    /* Sanity check: chip id (不符仅告警) */
    uint8_t id = 0;
    if (es_read(0xFD, &id)) {
        printf("[ES8156] chip id=0x%02X (expect 0x%02X)\r\n",
               (unsigned)id, ES8156_CHIP_ID);
    }

    printf("[ES8156] Ready (I2S 16-bit, vol 0dB)\r\n");
    return true;
}

void es8156_set_volume(uint8_t vol)
{
    /* vol 0-100 → 寄存器 0x14 值 0-191 (191 = 0dB 最大) */
    if (vol > 100) vol = 100;
    g_cur_vol = (uint8_t)((uint32_t)vol * ES8156_VOL_0DB / 100U);
    es_write(0x14, g_cur_vol);
}

void es8156_set_mute(bool mute)
{
    /* 复用音量寄存器: 静音写 0, 解除写回当前音量 */
    es_write(0x14, mute ? 0x00 : g_cur_vol);
}
