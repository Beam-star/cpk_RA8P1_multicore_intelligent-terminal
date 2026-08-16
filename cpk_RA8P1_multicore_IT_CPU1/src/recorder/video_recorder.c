/**
 ******************************************************************************
 * @file    video_recorder.c
 * @brief   视频录像任务实现 (见 video_recorder.h)
 ******************************************************************************
 */

#include "video_recorder.h"
#include "encoder/mjpeg_encoder.h"
#include "encoder/avi_muxer.h"
#include "driver/sd_card/sdhi_driver.h"
#include "rpmsg_video.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

#define VIDEO_TASK_STACK  8192
#define VIDEO_TASK_PRIO   1    /* 与 recorder_task(音频) 同优先级, 避免饿死音频 (configUSE_TIME_SLICING=0) */

/* 注: CPU1 未启用 D-Cache, 读非缓存 SDRAM 视频帧无需 InvalidateDCache。 */

/* ---- 运行时状态 ---- */
static void         *g_avi_file  = NULL;
static TaskHandle_t  g_task      = NULL;
static volatile bool g_recording = false;
static volatile uint32_t g_frames = 0;
static uint8_t       g_quality   = 75;
static volatile uint32_t g_start_tick = 0;   /* 录制起始 tick (计算真实时长) */
static volatile uint32_t g_stop_tick  = 0;   /* 录制停止 tick */

/* JPEG 输出缓冲 (SDRAM, ≥128KB) */
static uint8_t g_jpeg_buf[128 * 1024] __attribute__((section(".sdram_noinit")));

static void video_task(void *pvParameters)
{
    (void)pvParameters;
    uint32_t last_id = 0;

    printf("[VIDEO] Recording task started\r\n");

    while (g_recording) {
        video_shmem_t *sh = video_shmem();
        uint32_t id  = sh->frame_id;
        uint32_t idx = sh->write_idx;

        if (id != last_id) {
            last_id = id;
            const uint16_t *frame = (const uint16_t *)((idx == 0)
                                        ? VIDEO_FRAME0_ADDR : VIDEO_FRAME1_ADDR);

            uint32_t jpeg_size = mjpeg_encode_frame(frame, g_jpeg_buf,
                                                    sizeof(g_jpeg_buf), g_quality);
            if (jpeg_size > 0) {
                avi_muxer_write_video_frame(g_avi_file, g_jpeg_buf, jpeg_size, true);
                g_frames++;
            }
            taskYIELD();   /* 每帧让出 CPU: MJPEG 编码是 0.1-0.5s 忙循环, 必须显式让音频录制任务运行 */
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));   /* 无新帧 → 短暂等待 */
        }
    }

    /* 真实录制时长 = stop_tick - start_tick (tick 回绕由无符号减法正确处理) */
    uint32_t duration_ms = (uint32_t)(g_stop_tick - g_start_tick)
                           * portTICK_PERIOD_MS;

    avi_muxer_finalize(g_avi_file, g_frames, duration_ms);
    sd_card_fclose(g_avi_file);
    g_avi_file = NULL;

    printf("[VIDEO] Saved %lu frames in %lu ms\r\n",
           (unsigned long)g_frames, (unsigned long)duration_ms);
    vTaskDelete(NULL);
}

bool video_recorder_init(void)
{
    mjpeg_encoder_init();
    printf("[VIDEO] Recorder ready\r\n");
    return true;
}

bool video_recorder_start(uint8_t quality, int meeting_num)
{
    if (g_recording) {
        printf("[VIDEO] Already recording\r\n");
        return false;
    }
    if (quality < MJPEG_QUALITY_MIN) quality = MJPEG_QUALITY_MIN;
    if (quality > MJPEG_QUALITY_MAX) quality = MJPEG_QUALITY_MAX;
    g_quality = quality;

    sd_card_mkdir("/meeting/video");

    char path[96];
    snprintf(path, sizeof(path), "/meeting/video/meeting_%02d.avi", meeting_num);

    g_avi_file = sd_card_fopen(path, "w+");   /* w+ = 可写可读, 供 finalize 回读校验 */
    if (!g_avi_file) {
        printf("[VIDEO] Failed to create %s\r\n", path);
        return false;
    }

    avi_muxer_create(g_avi_file);
    g_frames = 0;
    g_recording = true;
    g_start_tick = xTaskGetTickCount();

    if (xTaskCreate(video_task, "video_rec", VIDEO_TASK_STACK,
                    NULL, VIDEO_TASK_PRIO, &g_task) != pdPASS) {
        printf("[VIDEO] Task create failed\r\n");
        g_recording = false;
        sd_card_fclose(g_avi_file);
        g_avi_file = NULL;
        return false;
    }

    printf("[VIDEO] Started: %s (Q%d)\r\n", path, quality);
    return true;
}

void video_recorder_stop(void)
{
    if (!g_recording) return;
    printf("[VIDEO] Stopping...\r\n");
    g_stop_tick = xTaskGetTickCount();
    g_recording = false;
    /* 等待任务收尾 (最多 5s) */
    for (int i = 0; i < 50 && g_avi_file != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool video_recorder_is_recording(void) { return g_recording; }

uint32_t video_recorder_get_frame_count(void) { return g_frames; }
