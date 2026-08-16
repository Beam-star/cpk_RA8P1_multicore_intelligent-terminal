/**
 ******************************************************************************
 * @file    i2s_driver.c
 * @brief   SSI/I2S 音频播放驱动 — DTC DMA + 中断回调
 *
 * FSP: I2S0 Master TX, 16-bit, DTC on SSI0_TXI
 * API: g_i2s0.p_api->open/write/stop (g_i2s_on_ssi)
 *
 * 播放流程: write(buf) → DTC DMA → TX_EMPTY 回调 → 给信号量 → 下一块
 *           最后一块发完 → IDLE 回调 → 给信号量 → 播放完成
 ******************************************************************************
 */

#include "i2s_driver.h"
#include "hal_data.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdio.h>

static SemaphoreHandle_t g_i2s_sem = NULL;
static volatile bool     g_i2s_open = false;
static bool              g_mclk_on  = false;

/* ---- I2S 中断回调 ---- */
void i2s0_callback(i2s_callback_args_t *p_args)
{
    if (!p_args || !g_i2s_sem) return;
    BaseType_t xHigher = pdFALSE;

    /*
     * 只在 TX_EMPTY(上一块已全部送入 FIFO,可以写下一块)时 give。
     * 千万不要在 IDLE 时也 give:块间 SD 读取间隙会让 FIFO 下溢触发
     * IDLE,多余的 give 会使下一次 i2s_wait_complete 立即返回,导致
     * 下下块提前写入 —— R_SSI_Write 没有 in-use 保护,会直接重置 DTC
     * 截断在播的数据,表现为播放变快 + 咔哒噪声。
     */
    if (p_args->event == I2S_EVENT_TX_EMPTY) {
        xSemaphoreGiveFromISR(g_i2s_sem, &xHigher);
    }
    portYIELD_FROM_ISR(xHigher);
}

/* ======================================================================== */
/*  Public                                                                   */
/* ======================================================================== */

/**
 * 启动 MCLK:GPT2 PWM 6.144MHz → PD06 → ES8156 MCLK 引脚 + SSI AUDIO_CLK。
 * FSP 只生成 g_timer2 实例,不会自动启动 —— 不启动它 SSI 就没有位时钟,
 * I2S 写永远不会完成(之前 "I2S blocks" 的根因)。
 * 必须在 es8156_init() / i2s_init() 之前调用;幂等。
 */
bool i2s_mclk_start(void)
{
    if (g_mclk_on) return true;

    fsp_err_t err = R_GPT_Open(&g_timer2_ctrl, &g_timer2_cfg);
    if (err != FSP_SUCCESS && err != FSP_ERR_ALREADY_OPEN) {
        printf("[I2S] MCLK GPT open failed: %ld\r\n", (long)err);
        return false;
    }
    err = R_GPT_Enable(&g_timer2_ctrl);
    if (err != FSP_SUCCESS) {
        printf("[I2S] MCLK GPT enable failed: %ld\r\n", (long)err);
        return false;
    }
    err = R_GPT_Start(&g_timer2_ctrl);
    if (err != FSP_SUCCESS) {
        printf("[I2S] MCLK GPT start failed: %ld\r\n", (long)err);
        return false;
    }

    g_mclk_on = true;
    printf("[I2S] MCLK 6.144MHz started (GPT2)\r\n");
    return true;
}

bool i2s_init(bool direction)
{
    if (g_i2s_open) return true;
    (void)direction;

    /* 防御:确保 MCLK 已在跑(正常应由入口函数先启动) */
    if (!i2s_mclk_start()) return false;

    printf("[I2S] Opening I2S0 (TX master, DTC DMA)...\r\n");

    g_i2s_sem = xSemaphoreCreateBinary();
    if (!g_i2s_sem) { printf("[I2S] Sem failed\r\n"); return false; }

    fsp_err_t err = g_i2s0.p_api->open(g_i2s0.p_ctrl, g_i2s0.p_cfg);
    if (err != FSP_SUCCESS) {
        printf("[I2S] Open failed: %ld\r\n", (long)err);
        return false;
    }

    g_i2s_open = true;
    printf("[I2S] Ready\r\n");
    return true;
}

bool i2s_start_tx(const int16_t *buf, uint32_t samples)
{
    /* 自愈: 上次 i2s_stop 重开失败会置 g_i2s_open=false */
    if (!g_i2s_open) {
        printf("[I2S] Not open, attempting recovery...\r\n");
        fsp_err_t rerr = g_i2s0.p_api->open(g_i2s0.p_ctrl, g_i2s0.p_cfg);
        if (rerr != FSP_SUCCESS && rerr != FSP_ERR_ALREADY_OPEN) {
            printf("[I2S] Recovery open failed: %ld\r\n", (long)rerr);
            return false;
        }
        g_i2s_open = true;
    }

    fsp_err_t err = g_i2s0.p_api->write(g_i2s0.p_ctrl,
                                         (void *)buf,
                                         samples * sizeof(int16_t));
    /*
     * 块间(预读 SD 时)FIFO 可能下溢 → SSI 报 TUIRQ 后自动停传。
     * 换回顺序读虽然块间有空隙,但不存在"预读追赶播放进度"的竞态,
     * 不会累积出周期性下溢。
     *
     * 万一仍然下溢:FSP_ERR_UNDERFLOW → stop+close+reopen+重试一次。
     */
    if (err == FSP_ERR_UNDERFLOW) {
        printf("[I2S] Underflow, recovering...\r\n");
        /* stop 是异步的(等 IDLE 中断才真正停), 立刻 close 会留下半开状态,
         * reopen 后 DTC 未重配 → 写"成功"却无声音(播放到一半变静音)。 */
        g_i2s0.p_api->stop(g_i2s0.p_ctrl);
        vTaskDelay(pdMS_TO_TICKS(2));
        g_i2s0.p_api->close(g_i2s0.p_ctrl);
        fsp_err_t rerr = g_i2s0.p_api->open(g_i2s0.p_ctrl, g_i2s0.p_cfg);
        if (rerr == FSP_ERR_ALREADY_OPEN) {
            /* close 未生效 → 强制再关一次再开 */
            g_i2s0.p_api->close(g_i2s0.p_ctrl);
            rerr = g_i2s0.p_api->open(g_i2s0.p_ctrl, g_i2s0.p_cfg);
        }
        if (rerr != FSP_SUCCESS) {
            printf("[I2S] Underflow reopen failed: %ld\r\n", (long)rerr);
            g_i2s_open = false;
            return false;
        }
        g_i2s_open = true;
        /* reopen 可能立刻触发一次 TX_EMPTY, 稍等后再清残留信号 */
        vTaskDelay(pdMS_TO_TICKS(1));
        while (xSemaphoreTake(g_i2s_sem, 0) == pdTRUE) { /* drain stale */ }
        err = g_i2s0.p_api->write(g_i2s0.p_ctrl,
                                   (void *)buf, samples * sizeof(int16_t));
        if (err != FSP_SUCCESS) {
            printf("[I2S] Retry write failed: %ld\r\n", (long)err);
            return false;
        }
    } else if (err != FSP_SUCCESS) {
        printf("[I2S] Write failed: %ld\r\n", (long)err);
        return false;
    }
    return true;
}

void i2s_stop(void)
{
    if (!g_i2s_open) return;

    g_i2s0.p_api->stop(g_i2s0.p_ctrl);
    vTaskDelay(pdMS_TO_TICKS(2));   /* 等异步 stop 完成 */

    /*
     * R_SSI_Stop 是异步的:驱动要等 IDLE 中断才清除内部 in-use 状态,
     * 直接再次 write 会返回 FSP_ERR_IN_USE(表现为"提示音只能播一次")。
     * 关闭并重开,确保下次播放驱动状态干净。
     */
    g_i2s0.p_api->close(g_i2s0.p_ctrl);
    fsp_err_t err = g_i2s0.p_api->open(g_i2s0.p_ctrl, g_i2s0.p_cfg);
    if (err != FSP_SUCCESS) {
        printf("[I2S] Reopen after stop failed: %ld\r\n", (long)err);
        g_i2s_open = false;
        return;
    }

    /*
     * 清空残留的完成信号:TX_EMPTY 与 IDLE 都会 give 二值信号量,而播放
     * 循环每块只 take 一次,残留的 give 会让下一次播放的第一块被误判为
     * "已完成",导致第二块过早写入。close 之后中断已停,此时清空是安全的。
     */
    while (xSemaphoreTake(g_i2s_sem, 0) == pdTRUE) { /* drain */ }
}

bool i2s_wait_complete(uint32_t timeout_ms)
{
    if (!g_i2s_sem) return false;
    return (xSemaphoreTake(g_i2s_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
}
