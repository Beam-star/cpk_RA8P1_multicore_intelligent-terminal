/**
 ******************************************************************************
 * @file    micarray_driver.c
 * @brief   Sipeed MA-USB8 麦克风阵列 UART1 驱动 + 16x16 热力图帧解析
 *
 * 数据流（正常模式）：
 *   UART1 RX 中断 (UART1_Callback) → 环形缓冲 → parser_task 逐字节扫描
 *   16x0xFF 帧头 → 收 256 字节热力图 → lvgl_ui_page3_set_heatmap()
 *
 * 热力图是持续流，无帧号/CRC，丢帧后靠下一组 16x0xFF 自动重新同步。
 ******************************************************************************
 */

#include "micarray_driver.h"
#include "hal_data.h"
#include "lvgl_ui/lvgl_ui_page3.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

#if MICARRAY_TEST_MODE

/* ======================================================================== */
/*  测试模式：不使用 UART，仅模拟旋转声源                                      */
/* ======================================================================== */

/* hal_data.c 的 g_uart1_cfg.p_callback 仍引用 UART1_Callback，需提供空实现 */
void UART1_Callback(uart_callback_args_t *p_args)
{
    (void)p_args;
}

void micarray_init(void)
{
    printf("[MICARRAY] TEST MODE (simulated rotating source)\r\n");
    lvgl_ui_page3_test_start();
}

bool micarray_send_cmd(uint8_t cmd)
{
    (void)cmd;
    return false;
}

#else  /* MICARRAY_TEST_MODE == 0 */

/* ======================================================================== */
/*  正常模式：UART1 持续接收热力图                                             */
/* ======================================================================== */

/* ---- 环形缓冲（单生产者=ISR，单消费者=parser_task，无需锁） ---- */
#define MICARRAY_RING_SIZE  4096

static volatile uint8_t  g_rx_ring[MICARRAY_RING_SIZE];
static volatile uint32_t g_rx_head = 0;    /* ISR 写 */
static volatile uint32_t g_rx_tail = 0;    /* parser 读 */

/* FSP 声明的 UART1 回调（ISR 上下文，极轻量：仅入环） */
void UART1_Callback(uart_callback_args_t *p_args)
{
    if (p_args->event != UART_EVENT_RX_CHAR) {
        return;
    }

    uint8_t  b    = (uint8_t)p_args->data;
    uint32_t next = (g_rx_head + 1) % MICARRAY_RING_SIZE;
    if (next != g_rx_tail) {                 /* 未满才写入 */
        g_rx_ring[g_rx_head] = b;
        g_rx_head = next;
    }
    /* 溢出则丢弃该字节，等下一帧头重新同步 */
}

static bool ring_pop(uint8_t *out)
{
    if (g_rx_head == g_rx_tail) return false;   /* 空 */
    *out = g_rx_ring[g_rx_tail];
    g_rx_tail = (g_rx_tail + 1) % MICARRAY_RING_SIZE;
    return true;
}

/* ---- 帧同步状态机：先找 16x0xFF，再收 256 字节热力图 ---- */
static uint8_t  s_heatmap[256];
static uint8_t  s_state      = 0;   /* 0=找帧头, 1=收数据 */
static uint8_t  s_sync_count = 0;
static uint16_t s_frame_idx  = 0;

static void parse_byte(uint8_t b)
{
    if (s_state == 0) {
        if (b == 0xFF) {
            if (++s_sync_count >= 16) {
                s_state     = 1;
                s_frame_idx = 0;
            }
        } else {
            s_sync_count = 0;
        }
    } else {
        s_heatmap[s_frame_idx++] = b;
        if (s_frame_idx >= 256) {
            lvgl_ui_page3_set_heatmap(s_heatmap);
            s_state      = 0;
            s_sync_count = 0;
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

void micarray_init(void)
{
    fsp_err_t err = g_uart1.p_api->open(&g_uart1_ctrl, &g_uart1_cfg);
    if (err != FSP_SUCCESS) {
        printf("[MICARRAY] UART1 open failed: %ld\r\n", (long)err);
        return;
    }
    printf("[MICARRAY] UART1 opened (2Mbps), parser task starting...\r\n");
    if (xTaskCreate(parser_task, "micarray", 1024, NULL, 2, NULL) != pdPASS) {
        printf("[MICARRAY] parser task create failed\r\n");
    }
}

bool micarray_send_cmd(uint8_t cmd)
{
    /* 单字节命令，p_transfer_tx=NULL 时一次只能写 1 字节。
     * 用静态缓冲：write 是异步的，栈变量会失效。 */
    static uint8_t s_cmd;
    s_cmd = cmd;
    fsp_err_t err = g_uart1.p_api->write(&g_uart1_ctrl, &s_cmd, 1);
    return (err == FSP_SUCCESS);
}

#endif  /* MICARRAY_TEST_MODE */
