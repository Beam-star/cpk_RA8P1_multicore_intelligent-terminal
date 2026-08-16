/**
 ******************************************************************************
 * @file    esp32_uart.c
 * @brief   RA8P1 <-> ESP32 串口通信驱动（UART2 / SCI2）
 *
 * 数据流：
 *   UART2 RX 中断 (UART2_Callback) → 环形缓冲 → parser_task 逐字节累积
 *   到 '\n' 为止 → 回调 esp32_line_cb_t(UTF-8 一行字幕)
 *
 * 命令（RA8P1 -> ESP32）：单字节 0x01~0x04（p_transfer_tx=NULL，一次写 1 字节）
 ******************************************************************************
 */

#include "esp32_uart.h"
#include "hal_data.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>
#include <stdio.h>

/* ---- 环形缓冲（单生产者=ISR，单消费者=parser_task） ---- */
#define ESP32_RING_SIZE     4096
#define ESP32_LINE_MAX      512     /* 单行字幕最大 UTF-8 字节数 */

static volatile uint8_t  g_rx_ring[ESP32_RING_SIZE];
static volatile uint32_t g_rx_head = 0;
static volatile uint32_t g_rx_tail = 0;

static char      s_line[ESP32_LINE_MAX];
static uint16_t  s_line_len = 0;
static esp32_line_cb_t g_line_cb = NULL;

/* 命令发送互斥锁：esp32_uart_send_cmd 现在会被 LVGL 任务（ASR/AI）和
 * PPT 手势任务（0x06~0x09）两个上下文调用，串行化 write 调用避免静态
 * s_cmd 缓冲被并发覆盖。 */
static SemaphoreHandle_t g_send_mutex = NULL;

/* FSP 声明的 UART2 回调（ISR 上下文，仅入环） */
void UART2_Callback(uart_callback_args_t *p_args)
{
    if (p_args->event != UART_EVENT_RX_CHAR) {
        return;
    }

    uint8_t  b    = (uint8_t)p_args->data;
    uint32_t next = (g_rx_head + 1) % ESP32_RING_SIZE;
    if (next != g_rx_tail) {
        g_rx_ring[g_rx_head] = b;
        g_rx_head = next;
    }
}

static bool ring_pop(uint8_t *out)
{
    if (g_rx_head == g_rx_tail) return false;
    *out = g_rx_ring[g_rx_tail];
    g_rx_tail = (g_rx_tail + 1) % ESP32_RING_SIZE;
    return true;
}

/* 逐字节解析：累积到 '\n' 视为一行完整字幕 */
static void parse_byte(uint8_t b)
{
    if (b == '\n') {
        if (s_line_len > 0) {
            s_line[s_line_len] = '\0';
            if (g_line_cb) g_line_cb(s_line);
            s_line_len = 0;
        }
    } else if (b == '\r') {
        /* 忽略 CR */
    } else {
        if (s_line_len < ESP32_LINE_MAX - 1) {
            s_line[s_line_len++] = (char)b;
        }
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

void esp32_uart_init(void)
{
    fsp_err_t err = g_uart2.p_api->open(&g_uart2_ctrl, &g_uart2_cfg);
    if (err != FSP_SUCCESS) {
        printf("[ESP32] UART2 open failed: %ld\r\n", (long)err);
        return;
    }
    printf("[ESP32] UART2 opened (115200 8N1)\r\n");
    if (xTaskCreate(parser_task, "esp32_uart", 1024, NULL, 2, NULL) != pdPASS) {
        printf("[ESP32] parser task create failed\r\n");
        return;
    }
    g_send_mutex = xSemaphoreCreateMutex();
}

bool esp32_uart_send_cmd(esp32_cmd_t cmd)
{
    /* 关键：write 是异步的（TXI 中断稍后才真正发字节），必须用静态缓冲，
     * 不能用栈上局部变量（函数返回后栈被覆盖，会发成 0x00）。 */
    static uint8_t s_cmd;
    if (!g_send_mutex) return false;   /* 尚未初始化（理论上不会走到） */
    if (xSemaphoreTake(g_send_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;

    s_cmd = (uint8_t)cmd;
    fsp_err_t err = g_uart2.p_api->write(&g_uart2_ctrl, &s_cmd, 1);

    xSemaphoreGive(g_send_mutex);
    return (err == FSP_SUCCESS);
}

void esp32_uart_set_line_cb(esp32_line_cb_t cb)
{
    g_line_cb = cb;
}
