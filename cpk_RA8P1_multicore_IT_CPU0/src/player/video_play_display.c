/**
 ******************************************************************************
 * @file    video_play_display.c
 * @brief   视频回放显示实现 (见 video_play_display.h)
 ******************************************************************************
 */

#include "video_play_display.h"
#include "common_data.h"
#include "rgblcd.h"
#include "rpmsg_video.h"
#include "mipi_camera_lcd.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

/* GLCDC vsync 计数器 (rgblcd.c 中定义) */
extern volatile uint32_t g_frame_count;
extern void SCB_CleanDCache_by_Addr(volatile void *addr, int32_t dsize);

/* 摄像头 640×480 显示区域 (与 mipi_camera_lcd.c 保持一致) */
#if GLCDC_CFG_LAYER_2_ENABLE
  #define LCD_X_OFF   0
#else
  #define LCD_X_OFF   ((RGBLCD_WIDTH - 640) / 2)
#endif
#define LCD_Y_OFF   (RGBLCD_HEIGHT - 480)   /* 底部对齐，顶部 120px 留给 top logo */

#define DISP_TASK_STACK  2048
#define DISP_TASK_PRIO   4

static TaskHandle_t  g_task    = NULL;
static volatile bool g_active  = false;
static volatile bool g_running = false;
static uint8_t       g_write_idx = 0;

/* 显示任务: 轮询 frame_id → 2× 放大 → 写 layer1 → buffer change */
static void display_task(void *pvParameters)
{
    (void)pvParameters;
    video_shmem_t *sh = video_shmem();
    uint32_t last_id = sh->frame_id;   /* 以当前 frame_id 为基线, 等下一帧 */
    TickType_t last_tick = xTaskGetTickCount();

    while (g_running) {
        uint32_t id = sh->frame_id;
        if (id != last_id) {
            last_id = id;
            last_tick = xTaskGetTickCount();
            uint32_t idx = sh->write_idx;
            const uint16_t *src = (const uint16_t *)((idx == 0)
                                        ? VIDEO_FRAME0_ADDR : VIDEO_FRAME1_ADDR);

            /* 等一个 vsync: 确保 GLCDC 已切离目标缓冲 */
            uint32_t vs = g_frame_count;
            while (g_frame_count == vs) { vTaskDelay(1); }

            uint8_t w = (uint8_t)(g_write_idx ^ 1u);
            uint16_t *fb = (uint16_t *)fb_background[w];

            /* 2× 最近邻放大: 320×240 → 640×480, 写入 (LCD_X_OFF, LCD_Y_OFF) */
            for (int y = 0; y < VIDEO_PLAY_H; y++) {
                uint16_t *d0 = &fb[(LCD_Y_OFF + y * 2) * RGBLCD_WIDTH + LCD_X_OFF];
                uint16_t *d1 = d0 + RGBLCD_WIDTH;
                const uint16_t *s = &src[y * VIDEO_PLAY_W];
                for (int x = 0; x < VIDEO_PLAY_W; x++) {
                    uint16_t p = s[x];
                    d0[x * 2]     = p;
                    d0[x * 2 + 1] = p;
                    d1[x * 2]     = p;
                    d1[x * 2 + 1] = p;
                }
            }

            SCB_CleanDCache_by_Addr((volatile void *)fb,
                                    RGBLCD_STRIDE_BYTES * RGBLCD_HEIGHT);
            __DSB();
            R_GLCDC_BufferChange(&g_display0_ctrl, fb, DISPLAY_FRAME_LAYER_1);
            __DSB();
            g_write_idx = w;
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
            /* 超时兜底: 5s 无新帧 (播放器打开/解码失败或 CPU1 挂死)
             * → 退出并恢复摄像头, 避免 layer1 永久冻结 */
            if ((xTaskGetTickCount() - last_tick) > pdMS_TO_TICKS(5000)) {
                printf("[VPLAY-DISP] timeout (no frame), restoring camera\r\n");
                break;
            }
        }
    }

    /* 退出时恢复摄像头显示 (防御: 播放器失败也要让摄像头回来) */
    mipi_camera_lcd_set_playback(false);
    g_active  = false;
    g_running = false;
    vTaskDelete(NULL);
}

void video_play_display_start(void)
{
    if (g_active) return;
    g_active  = true;
    g_running = true;
    g_write_idx = 0;
    mipi_camera_lcd_set_playback(true);   /* 暂停摄像头写 layer1 */

    if (xTaskCreate(display_task, "vplay_disp", DISP_TASK_STACK,
                    NULL, DISP_TASK_PRIO, &g_task) != pdPASS) {
        g_active = false;
        g_running = false;
        mipi_camera_lcd_set_playback(false);
        printf("[VPLAY-DISP] task create FAILED\r\n");
        return;
    }
    printf("[VPLAY-DISP] display started\r\n");
}

void video_play_display_stop(void)
{
    if (!g_active) return;
    g_running = false;
    g_active  = false;
    mipi_camera_lcd_set_playback(false);  /* 恢复摄像头显示 */
    printf("[VPLAY-DISP] display stopped\r\n");
}

bool video_play_display_is_active(void) { return g_active; }
