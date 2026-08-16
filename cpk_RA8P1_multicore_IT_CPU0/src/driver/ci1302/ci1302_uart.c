/**
 ******************************************************************************
 * @file    ci1302_uart.c
 * @brief   CI1302 离线语音识别模块驱动（UART0 / SCI0, P602=RX P603=TX, 115200 8N1）
 *
 * 数据流：
 *   UART0 RX 中断 (UART0_Callback) → 环形缓冲 → parser_task 逐字节扫描
 *   到 5 字节帧 AA 55 CMD DATA FB 为止 → 回调 ci1302_cmd_cb_t。
 *
 * 发送（RA8P1 → CI1302）：静态 5 字节缓冲 + 互斥锁（触发播报）。
 ******************************************************************************
 */

#include "ci1302_uart.h"
#include "hal_data.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>
#include <stdio.h>

/* ---- 环形缓冲（单生产者=ISR，单消费者=parser_task） ---- */
#define CI1302_RING_SIZE    512

static volatile uint8_t  g_rx_ring[CI1302_RING_SIZE];
static volatile uint32_t g_rx_head = 0;
static volatile uint32_t g_rx_tail = 0;

static ci1302_cmd_cb_t g_cmd_cb = NULL;

/* 发送互斥锁：避免静态发送缓冲被并发覆盖 */
static SemaphoreHandle_t g_send_mutex = NULL;

/* FSP 声明的 UART0 回调（ISR 上下文，仅入环） */
void UART0_Callback(uart_callback_args_t *p_args)
{
    if (p_args->event != UART_EVENT_RX_CHAR) {
        return;
    }

    uint8_t  b    = (uint8_t)p_args->data;
    uint32_t next = (g_rx_head + 1) % CI1302_RING_SIZE;
    if (next != g_rx_tail) {
        g_rx_ring[g_rx_head] = b;
        g_rx_head = next;
    }
}

static bool ring_pop(uint8_t *out)
{
    if (g_rx_head == g_rx_tail) return false;
    *out = g_rx_ring[g_rx_tail];
    g_rx_tail = (g_rx_tail + 1) % CI1302_RING_SIZE;
    return true;
}

/* ---- 5 字节帧解析状态机 ---- */
typedef enum {
    P_IDLE = 0,   /* 未入帧，等待 0xAA            */
    P_AA,         /* 已收 0xAA，等待 0x55         */
    P_CMD,        /* 已收头，等待 CMD             */
    P_DATA,       /* 已收 CMD，等待 DATA          */
    P_TAIL,       /* 已收 DATA，等待 FB           */
} parse_state_t;

static parse_state_t g_state = P_IDLE;
static uint8_t       g_cmd   = 0;
static uint8_t       g_data  = 0;

static void parse_byte(uint8_t b)
{
    switch (g_state) {
    case P_IDLE:
        if (b == CI1302_HEAD0) g_state = P_AA;
        break;

    case P_AA:
        if (b == CI1302_HEAD1)      g_state = P_CMD;
        else if (b == CI1302_HEAD0) g_state = P_AA;    /* AA AA → 保持 */
        else                        g_state = P_IDLE;  /* 非帧头，重新同步 */
        break;

    case P_CMD:
        g_cmd   = b;
        g_state = P_DATA;
        break;

    case P_DATA:
        g_data  = b;
        g_state = P_TAIL;
        break;

    case P_TAIL:
        if (b == CI1302_TAIL) {
            ci1302_cmd_t cmd = (ci1302_cmd_t)(((uint16_t)g_cmd << 8) | g_data);
            if (g_cmd_cb) g_cmd_cb(cmd);
            g_state = P_IDLE;
        } else if (b == CI1302_HEAD0) {
            g_state = P_AA;   /* 尾不对，但可能是新帧头 */
        } else {
            g_state = P_IDLE;
        }
        break;
    }
}

static void parser_task(void *pv)
{
    (void)pv;
    uint8_t b;
    while (1) {
        if (ring_pop(&b)) {
            parse_byte(b);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

/* ======================================================================== */
/*  Public                                                                   */
/* ======================================================================== */

void ci1302_uart_init(void)
{
    fsp_err_t err = g_uart0.p_api->open(&g_uart0_ctrl, &g_uart0_cfg);
    if (err != FSP_SUCCESS) {
        printf("[CI1302] UART0 open failed: %ld\r\n", (long)err);
        return;
    }
    printf("[CI1302] UART0 opened (115200 8N1)\r\n");

    if (xTaskCreate(parser_task, "ci1302", 512, NULL, 2, NULL) != pdPASS) {
        printf("[CI1302] parser task create failed\r\n");
        return;
    }
    g_send_mutex = xSemaphoreCreateMutex();
}

void ci1302_uart_set_cmd_cb(ci1302_cmd_cb_t cb)
{
    g_cmd_cb = cb;
}

bool ci1302_uart_send(ci1302_cmd_t cmd)
{
    /* write 为异步（TXI 中断稍后才真正发字节），必须用静态缓冲 */
    static uint8_t s_buf[5];
    if (!g_send_mutex) return false;
    if (xSemaphoreTake(g_send_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;

    s_buf[0] = CI1302_HEAD0;
    s_buf[1] = CI1302_HEAD1;
    s_buf[2] = (uint8_t)(cmd >> 8);
    s_buf[3] = (uint8_t)(cmd & 0xFF);
    s_buf[4] = CI1302_TAIL;

    fsp_err_t err = g_uart0.p_api->write(&g_uart0_ctrl, s_buf, 5);

    xSemaphoreGive(g_send_mutex);
    return (err == FSP_SUCCESS);
}
