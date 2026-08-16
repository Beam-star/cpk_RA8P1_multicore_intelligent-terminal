/**
 ******************************************************************************
 * @file    video_player.c
 * @brief   视频回放实现 (见 video_player.h)
 *
 * 循环: 读 AVI 一帧 JPEG → jpeg_decode_rgb565 解码到共享双缓冲 →
 *       写 write_idx / 递增 frame_id → 按 dwMicroSecPerFrame 节拍延时。
 *
 * CPU1 无 D-Cache, 写共享 SDRAM 直达; CPU0 侧该区域已在 MPU 中设为
 * non-cacheable, 故无需任何 cache 维护, 仅 __DSB 保证写序。
 ******************************************************************************
 */

#include "video_player.h"
#include "avi_parser.h"
#include "jpeg_decoder.h"
#include "rpmsg_video.h"
#include "driver/sd_card/sdhi_driver.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

#define VPLAY_STATE_IDLE     0
#define VPLAY_STATE_PLAYING  1
#define VPLAY_STATE_STOPPING 2

/* JPEG 帧输入缓冲 (AVI 单帧 ≤ ~64KB) */
static uint8_t g_jpeg_buf[64 * 1024] __attribute__((section(".sdram_noinit")));

static volatile int      g_state = VPLAY_STATE_IDLE;
static volatile bool     g_paused = false;
static volatile uint32_t g_play_idx = 0;
static volatile uint32_t g_total_ms = 0;
static volatile uint32_t g_elapsed_ms = 0;

/* 当前打开的文件句柄 (NULL=已关闭)。崩溃时泄漏, 由 force_close 清理。 */
static void *g_active_fp = NULL;

bool video_player_init(void)
{
    printf("[VPLAY] Ready\r\n");
    return true;
}

bool video_player_play(const char *path)
{
    if (!path) return false;

    /* 忙守卫 (单实例, 与 audio_player 的 g_play_busy 在 rpmsg 层分开) */
    if (g_state != VPLAY_STATE_IDLE) {
        printf("[VPLAY] Busy\r\n");
        return false;
    }
    g_state = VPLAY_STATE_PLAYING;
    g_paused = false;

    avi_parser_t avi;
    if (!avi_parser_open(&avi, path)) {
        g_state = VPLAY_STATE_IDLE;
        return false;
    }
    g_active_fp = avi.file;

    printf("[VPLAY] Playing %s\r\n", path);
    bool ok = true;

    g_total_ms = (uint32_t)((uint64_t)avi.total_frames * avi.us_per_frame / 1000u);
    g_elapsed_ms = 0;

    while (g_state == VPLAY_STATE_PLAYING) {
        /* 暂停: 阻塞等待 resume / stop (在帧边界生效) */
        while (g_paused && g_state == VPLAY_STATE_PLAYING) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (g_state != VPLAY_STATE_PLAYING) break;

        TickType_t frame_start = xTaskGetTickCount();

        uint32_t jpeg_size = 0;
        if (!avi_parser_read_frame(&avi, g_jpeg_buf, sizeof(g_jpeg_buf),
                                   &jpeg_size)) {
            break;   /* EOF */
        }

        /* 解码直接写入共享双缓冲 (省一次拷贝) */
        uint32_t idx = g_play_idx ^ 1u;
        uint16_t *dst = (uint16_t *)((idx == 0) ? VIDEO_FRAME0_ADDR
                                                : VIDEO_FRAME1_ADDR);
        uint32_t w = 0, h = 0;
        if (!jpeg_decode_rgb565(g_jpeg_buf, jpeg_size, dst, &w, &h)) {
            printf("[VPLAY] decode failed @ frame %lu\r\n",
                   (unsigned long)avi.frames_read);
            ok = false;
            break;
        }
        (void)w; (void)h;

        /* 发布新帧: 先写 write_idx, 再递增 frame_id (CPU0 轮询 frame_id) */
        video_shmem_t *sh = video_shmem();
        sh->write_idx = idx;
        __DSB();
        sh->frame_id = sh->frame_id + 1u;
        __DSB();
        g_play_idx = idx;

        /* 更新播放进度 (毫秒) */
        g_elapsed_ms = (uint32_t)((uint64_t)avi.frames_read * avi.us_per_frame / 1000u);

        /* 节拍: 每帧总耗时 = us_per_frame。解码时间已计入 elapsed, 不再额外
         * 叠加固定延时, 否则回放会比录制慢整整一个解码周期。 */
        uint32_t target_ticks = avi.us_per_frame / 1000u;
        TickType_t elapsed = xTaskGetTickCount() - frame_start;
        if (elapsed < target_ticks) {
            vTaskDelay(target_ticks - elapsed);
        }
    }

    bool stopped = (g_state == VPLAY_STATE_STOPPING);
    avi_parser_close(&avi);
    g_active_fp = NULL;
    g_state = VPLAY_STATE_IDLE;
    g_total_ms = 0;
    g_elapsed_ms = 0;

    printf("[VPLAY] Done: %lu frames (%s)\r\n",
           (unsigned long)avi.frames_read, stopped ? "stopped" : "eof");
    return ok;
}

void video_player_stop(void)
{
    if (g_state == VPLAY_STATE_PLAYING) {
        g_state = VPLAY_STATE_STOPPING;
    }
}

void video_player_pause(void)
{
    if (g_state == VPLAY_STATE_PLAYING) {
        g_paused = true;
    }
}

void video_player_resume(void)
{
    g_paused = false;
}

bool video_player_get_progress(uint32_t *elapsed_ms, uint32_t *total_ms)
{
    if (g_total_ms == 0) return false;
    *elapsed_ms = g_elapsed_ms;
    *total_ms = g_total_ms;
    return true;
}

bool video_player_is_playing(void) { return g_state == VPLAY_STATE_PLAYING; }

void video_player_force_close(void)
{
    if (g_active_fp) {
        sd_card_fclose(g_active_fp);
        g_active_fp = NULL;
    }
    g_state      = VPLAY_STATE_IDLE;
    g_paused     = false;
    g_total_ms   = 0;
    g_elapsed_ms = 0;
    printf("[VPLAY] Force closed (recovery)\r\n");
}
