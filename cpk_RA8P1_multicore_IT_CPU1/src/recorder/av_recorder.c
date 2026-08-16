/**
 ******************************************************************************
 * @file    av_recorder.c
 * @brief   音频录制任务 — PDM → WAV + 视频占位符
 *
 * 纯音频模式: PDM Mic0 16kHz 16-bit mono → WAV 文件
 * 视频占位:   创建一个假的 .avi_dummy 文件表示"已录制视频"
 *
 * 录制时在 /meeting/audio/ 和 /meeting/video/ 下分别保存。
 ******************************************************************************
 */

#include "av_recorder.h"
#include "wav_encoder.h"
#include "sdhi_driver.h"
#include "pdm_audio.h"
#include "ff_stdio.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

/* ---- 录制任务 ---- */
#define REC_TASK_STACK_SIZE    4096
#define REC_TASK_PRIORITY      1

/* ---- 运行时状态 ---- */
static rec_info_t   g_rec_info       = {0};
static rec_config_t g_rec_config     = {75, 0, 0};  /* fps unused */
static void        *g_wav_file       = NULL;
static TaskHandle_t g_rec_task_handle = NULL;
static uint32_t     g_pcm_total      = 0;  /* total PCM bytes written */

/*
 * 批量写缓冲 (SDRAM): 每帧 1KB/32ms 直接写 SD 会高频占用 FAT 锁,
 * 与播放任务的预读争抢,导致播放 I2S 下溢。积累 16KB (~512ms) 再落盘,
 * 锁占用频率降低 16 倍。
 */
#define REC_BATCH_BYTES  (16U * 1024U)
static uint8_t  g_rec_batch[REC_BATCH_BYTES] __attribute__((section(".sdram_noinit")));
static uint32_t g_rec_batch_pos = 0;

/* 开录延迟: 等"开始录制"提示音播完再采集, 避免把提示音录进去 */
#define REC_START_DELAY_MS  (2000U)

/* ======================================================================== */
/*  视频占位符                                                                */
/* ======================================================================== */

/* ======================================================================== */
/*  录制主循环                                                                */
/* ======================================================================== */

static void recorder_task(void *pvParameters)
{
    (void)pvParameters;

    printf("[REC] Audio recording task started\r\n");

    /* 延迟开录: 等提示音播完 (期间按 Stop 可提前结束) */
    for (uint32_t d = 0; d < REC_START_DELAY_MS / 100U; d++) {
        if (g_rec_info.state == REC_IDLE ||
            g_rec_info.state == REC_FINALIZING) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    printf("[REC] Capture begins\r\n");

    /* 写 WAV 头 */
    wav_write_header(g_wav_file);
    g_pcm_total = 0;
    g_rec_batch_pos = 0;

    /* 主循环: 采集 PDM 帧 → 积累到批量缓冲 → 满 16KB 落盘 */
    while (g_rec_info.state != REC_IDLE &&
           g_rec_info.state != REC_FINALIZING) {

        if (g_rec_info.state == REC_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* 获取一帧 PDM 音频 (512 samples × 16-bit = 1024 bytes, ~32ms) */
        pdm_audio_frame_t frame;
        if (pdm_audio_get_frame(&frame, 500)) {
            /* Mic0 → 批量缓冲 (mono, 16-bit PCM) */
            memcpy(&g_rec_batch[g_rec_batch_pos], frame.data[0],
                   PDM_FRAME_SIZE * sizeof(int16_t));
            g_rec_batch_pos += PDM_FRAME_SIZE * sizeof(int16_t);
            g_pcm_total     += PDM_FRAME_SIZE * sizeof(int16_t);

            /* 缓冲将满 → 落盘 */
            if (g_rec_batch_pos + PDM_FRAME_SIZE * sizeof(int16_t)
                    > REC_BATCH_BYTES) {
                sd_card_fwrite(g_wav_file, g_rec_batch, g_rec_batch_pos);
                g_rec_batch_pos = 0;
            }

            g_rec_info.audio_bytes = g_pcm_total;
            g_rec_info.duration_sec = g_pcm_total / (WAV_SAMPLE_RATE *
                                     (WAV_BITS_PER_SAMPLE / 8) * WAV_NUM_CHANNELS);
        }

        /* 检查最大录制时长 */
        if (g_rec_config.max_duration_sec > 0 &&
            g_rec_info.duration_sec >= g_rec_config.max_duration_sec) {
            printf("[REC] Max duration reached (%us)\r\n",
                   (unsigned)g_rec_config.max_duration_sec);
            g_rec_info.state = REC_FINALIZING;
        }
    }

    /* 录制结束: 刷掉批量缓冲余量 + 回填 WAV 头 */
    if (g_rec_batch_pos > 0) {
        sd_card_fwrite(g_wav_file, g_rec_batch, g_rec_batch_pos);
        g_rec_batch_pos = 0;
    }
    printf("[REC] Finalizing WAV: %lu PCM bytes, %lu sec\r\n",
           (unsigned long)g_pcm_total,
           (unsigned long)g_rec_info.duration_sec);
    wav_update_header(g_wav_file, g_pcm_total);

    sd_card_fclose(g_wav_file);
    g_wav_file = NULL;

    g_rec_info.state = REC_IDLE;
    printf("[REC] Recording saved\r\n");
    vTaskDelete(NULL);
}

/* ======================================================================== */
/*  Public API                                                               */
/* ======================================================================== */

bool av_recorder_init(void)
{
    printf("[REC] Audio recorder ready (PDM→WAV 16kHz mono)\r\n");
    return true;
}

bool av_recorder_start(const rec_config_t *config, int meeting_num)
{
    if (g_rec_info.state != REC_IDLE) {
        printf("[REC] Already recording!\r\n");
        return false;
    }

    if (config) {
        memcpy(&g_rec_config, config, sizeof(rec_config_t));
    }

    /* 创建目录: /meeting/audio/ 和 /meeting/video/ */
    sd_card_mkdir("/meeting/audio");
    sd_card_mkdir("/meeting/video");

    /* 顺序命名: meeting_01.wav, meeting_02.wav, ... (与视频同名配对) */
    char wav_path[128];
    snprintf(wav_path, sizeof(wav_path),
             "/meeting/audio/meeting_%02d.wav", meeting_num);

    /* 创建 WAV 文件 */
    g_wav_file = sd_card_fopen(wav_path, "w");
    if (!g_wav_file) {
        printf("[REC] Failed to create: %s\r\n", wav_path);
        return false;
    }

    /* 启动录制任务 */
    memset(&g_rec_info, 0, sizeof(g_rec_info));
    g_rec_info.state = REC_RECORDING;
    strncpy(g_rec_info.filename, wav_path, sizeof(g_rec_info.filename) - 1);

    if (xTaskCreate(recorder_task, "recorder", REC_TASK_STACK_SIZE,
                    NULL, REC_TASK_PRIORITY, &g_rec_task_handle) != pdPASS) {
        g_rec_info.state = REC_IDLE;
        sd_card_fclose(g_wav_file);
        g_wav_file = NULL;
        printf("[REC] Task create failed\r\n");
        return false;
    }

    printf("[REC] Started: %s\r\n", wav_path);
    return true;
}

void av_recorder_pause(void)
{
    if (g_rec_info.state == REC_RECORDING) {
        g_rec_info.state = REC_PAUSED;
        printf("[REC] Paused\r\n");
    }
}

void av_recorder_resume(void)
{
    if (g_rec_info.state == REC_PAUSED) {
        g_rec_info.state = REC_RECORDING;
        printf("[REC] Resumed\r\n");
    }
}

void av_recorder_stop(void)
{
    if (g_rec_info.state == REC_IDLE) return;
    printf("[REC] Stopping...\r\n");
    g_rec_info.state = REC_FINALIZING;
    /* Wait for task to finish (max 5s) */
    for (int i = 0; i < 50 && g_rec_info.state != REC_IDLE; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

rec_info_t av_recorder_get_info(void) { return g_rec_info; }

bool av_recorder_is_recording(void)
{
    return (g_rec_info.state == REC_RECORDING ||
            g_rec_info.state == REC_PAUSED);
}
