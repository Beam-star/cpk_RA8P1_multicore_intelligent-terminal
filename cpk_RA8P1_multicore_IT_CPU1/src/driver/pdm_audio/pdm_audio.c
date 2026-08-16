/**
 ******************************************************************************
 * @file    pdm_audio.c
 * @brief   PDM 麦克风阵列音频驱动实现
 ******************************************************************************
 */

#include "pdm_audio.h"
#include "rpmsg_pdm.h"       /* 共享内存结构 (CPU0 读取) */
#include "hal_data.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

/*
 * ---- PDM 环形缓冲区 ----
 *
 * 重要: FSP r_pdm 驱动把**每个采样存为 32 位字**(FIFO 寄存器 PDDRR 原样
 * 拷贝, PCM 数据在低 16 位), 且只采集配置的单通道 (channel 2, 一只麦克风)。
 * 之前按 int16_t[4][3][512] 解释缓冲区, 录进 WAV 的是 "PCM低半字+符号高半字"
 * 交替的数据 → 半速+严重失真。
 *
 * 环形布局: PDM_BUFFER_COUNT 个槽 × PDM_FRAME_SIZE 个 uint32 采样,
 * R_PDM_Start 的 number_of_data_to_callback = PDM_FRAME_SIZE, 每中断
 * 恰好推进一个槽, 与 g_pdm_write_idx 对齐。
 */
static uint32_t g_pdm_ring_buf[PDM_BUFFER_COUNT][PDM_FRAME_SIZE];
static volatile uint32_t g_pdm_write_idx = 0;
static volatile uint32_t g_pdm_frame_id  = 0;

/* ---- 同步信号量 ---- */
static SemaphoreHandle_t g_pdm_sem = NULL;

/* ---- PDM 共享内存任务句柄 ---- */
static TaskHandle_t g_pdm_shmem_task_handle = NULL;

/* ---- PDM DMA 完成中断回调 ---- */
void pdm_callback(pdm_callback_args_t *p_args)
{
    if (p_args->event == PDM_EVENT_DATA) {
        g_pdm_frame_id++;
        g_pdm_write_idx = (g_pdm_write_idx + 1) % PDM_BUFFER_COUNT;

        BaseType_t higher_priority_woken = pdFALSE;
        xSemaphoreGiveFromISR(g_pdm_sem, &higher_priority_woken);
        portYIELD_FROM_ISR(higher_priority_woken);
    }
}

/*
 * PDM → 共享内存 写入任务
 *
 * 功能:
 *   1. 等待 PDM DMA 完成一帧 (32ms, 信号量)
 *   2. 从环形缓冲区拷贝 PCM 数据到 SDRAM 共享内存 (pdm_shmem_t)
 *   3. 递增 write_counter (CPU0 声源定位任务以轮询方式检测新数据)
 *   4. 同时将 Mic0 数据放入本地环形缓冲供录制任务读取
 *
 * 为什么不用 RPMsg: PDM 数据量大 (3×512×2=3KB/帧, ~96KB/s),
 * RPMsg 队列不适合高频大数据传输, 共享内存零拷贝更高效
 */
static void pdm_shmem_task(void *pvParameters)
{
    (void)pvParameters;
    pdm_shmem_t *p_shm = pdm_shmem_get();

    printf("[PDM] Shared memory writer task started (→SDRAM @ 0x%08lX)\r\n",
           (unsigned long)p_shm);

    /* 只有 Mic0 有真实数据 (单通道硬件), ch1/ch2 清零一次 */
    memset((void *)p_shm->pcm_data[1], 0, sizeof(p_shm->pcm_data[1]));
    memset((void *)p_shm->pcm_data[2], 0, sizeof(p_shm->pcm_data[2]));

    while (1) {
        /* 等待 DMA 完成 (阻塞, 32ms 超时) */
        if (xSemaphoreTake(g_pdm_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        /* 获取最新完成帧的索引 */
        uint32_t read_idx = (g_pdm_write_idx > 0) ?
                            (g_pdm_write_idx - 1) : (PDM_BUFFER_COUNT - 1);

        /* 32 位 FIFO 字 → 16 位 PCM (低 16 位有效), 写入共享内存 Mic0 */
        const uint32_t *src = g_pdm_ring_buf[read_idx];
        volatile int16_t *dst = p_shm->pcm_data[0];
        for (uint32_t i = 0; i < PDM_FRAME_SIZE; i++) {
            dst[i] = (int16_t)(src[i] & 0xFFFFu);
        }

        /* 写时间戳和帧信息 */
        p_shm->frame_id     = g_pdm_frame_id;
        p_shm->timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        /* 内存屏障: 确保 PCM 数据在 counter 递增前对 CPU0 可见 */
        __DSB();

        /* 递增写计数器 (CPU0 通过轮询此变量检测新帧) */
        p_shm->write_counter++;

        __DSB();
    }
}

/* ---- Public API ---- */

bool pdm_audio_init(void)
{
    printf("[PDM] Initializing 3-channel PDM microphone array (CPU1)...\r\n");

    g_pdm_sem = xSemaphoreCreateBinary();
    if (g_pdm_sem == NULL) {
        printf("[PDM] Failed to create semaphore\r\n");
        return false;
    }

    /* Open and start the PDM hardware */
    fsp_err_t err = R_PDM_Open(&g_pdm0_ctrl, &g_pdm0_cfg);
    if (err != FSP_SUCCESS) {
        printf("[PDM] R_PDM_Open failed: %ld\r\n", (long)err);
        return false;
    }

    err = R_PDM_Start(&g_pdm0_ctrl, g_pdm_ring_buf,
                      sizeof(g_pdm_ring_buf), PDM_FRAME_SIZE);
    if (err != FSP_SUCCESS) {
        printf("[PDM] R_PDM_Start failed: %ld\r\n", (long)err);
        R_PDM_Close(&g_pdm0_ctrl);
        return false;
    }
    printf("[PDM] Hardware started (1ch x 16kHz, 32-bit FIFO words, DMA)\r\n");

    /* 启动共享内存写入任务 (优先级 4, 确保及时写入不丢帧) */
    BaseType_t ret = xTaskCreate(pdm_shmem_task, "pdm_shmem",
                                  2048, NULL, 4,
                                  &g_pdm_shmem_task_handle);
    if (ret != pdPASS) {
        printf("[PDM] Failed to create shmem task\r\n");
        return false;
    }

    printf("[PDM] Initialization complete (3ch×16kHz×16-bit → SDRAM shmem)\r\n");
    return true;
}

bool pdm_audio_get_frame(pdm_audio_frame_t *frame, uint32_t timeout_ms)
{
    /* 从共享内存读取 (被录制任务调用, 读取 Mic0 用于 WAV 编码) */
    pdm_shmem_t *p_shm = pdm_shmem_get();
    static uint32_t last_counter = 0;
    uint32_t elapsed = 0;

    while (p_shm->write_counter == last_counter && elapsed < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(1));
        elapsed++;
    }

    if (elapsed >= timeout_ms) return false;

    last_counter = p_shm->write_counter;

    frame->frame_id     = p_shm->frame_id;
    frame->timestamp_ms = p_shm->timestamp_ms;
    memcpy(frame->data, (const void *)p_shm->pcm_data, sizeof(frame->data));

    return true;
}

uint32_t pdm_audio_read_channel(uint8_t channel, int16_t *buf, uint32_t samples)
{
    if (channel >= PDM_CHANNELS || buf == NULL || samples == 0) {
        return 0;
    }

    pdm_shmem_t *p_shm = pdm_shmem_get();
    uint32_t copy_samples = (samples > PDM_FRAME_SIZE) ? PDM_FRAME_SIZE : samples;

    memcpy(buf, (const void *)p_shm->pcm_data[channel],
           copy_samples * sizeof(int16_t));

    return copy_samples;
}
