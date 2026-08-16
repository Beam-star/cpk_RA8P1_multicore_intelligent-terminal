/**
 ******************************************************************************
 * @file    lvgl_ui_ppt.c
 * @brief   PPT 翻页手势识别（手掌滑动 → ESP32 翻页命令）
 *
 * 设计要点（安全冗余，避免微小抖动/边缘停留导致连发翻页）：
 *   1. 仅当「PPT 开关 ON」且「Hand 检测模式」才进入激活态；激活/退出
 *      边沿才发 0x06(进入)/0x09(退出)，其余时刻不重复发送。
 *   2. 手掌中心用 EMA 平滑，滤除检测抖动。
 *   3. 滑动判定：水平位移 ≥ PPT_SWIPE_MIN_DX 且 |dx| > 2·|dy|（拒绝斜向/纵向）。
 *   4. 滑动须在 PPT_SWIPE_MAX_MS 内完成，否则重置起点（慢速漂移不触发）。
 *   5. 触发后进入 REARM：手掌须回到起点附近（或丢失）才允许下一次滑动，
 *      同时受 PPT_COOLDOWN_MS 最小间隔保护，防止边缘停留连发。
 ******************************************************************************
 */

#include "lvgl_ui_ppt.h"
#include "driver/esp32/esp32_uart.h"
#include "ai_application/face_detection_task.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

/* ---- 可调参数（坐标均为相机空间 640×480，中心点像素） ---- */
#define PPT_TASK_PRIO          1
#define PPT_TASK_STACK         512
#define PPT_TASK_PERIOD_MS     20

#define PPT_EMA_ALPHA          0.40f   /* 中心点 EMA 平滑系数           */
#define PPT_SWIPE_MIN_DX       120     /* 触发滑动的最小水平位移(px)   */
#define PPT_SWIPE_VERT_RATIO   2       /* |dx| 须大于该倍数·|dy|       */
#define PPT_SWIPE_MAX_MS       800     /* 滑动须在该时间窗内完成       */
#define PPT_COOLDOWN_MS        1200    /* 两次翻页最小间隔             */
#define PPT_REARM_DX           60      /* 回到起点该距离内才重新武装   */
#define PPT_HAND_LOST_FRAMES   3       /* 连续丢手该帧数后整体复位     */

typedef enum {
    GEST_IDLE = 0,     /* 无手，等待出现 */
    GEST_TRACKING,     /* 正在跟踪滑动   */
    GEST_REARM,        /* 已触发，等回中 */
} gest_state_t;

static volatile bool g_ppt_on     = false;
static bool           g_ppt_active = false;   /* 任务上下文：ppt_on && HAND */

/* 手势状态 */
static gest_state_t g_state        = GEST_IDLE;
static bool         g_smooth_valid = false;
static float        g_smooth_x     = 0.0f;
static float        g_smooth_y     = 0.0f;
static float        g_ref_x        = 0.0f;
static float        g_ref_y        = 0.0f;
static TickType_t   g_track_start  = 0;
static TickType_t   g_last_fire    = 0;
static bool         g_has_fired    = false;   /* 首次滑动不受冷却限制 */
static uint32_t     g_lost_count   = 0;

/* ---- 内部工具 ---- */

/* 取面积最大的手掌中心（多手时只跟踪一个主手）。返回是否成功。 */
static bool pick_primary_hand(float *cx, float *cy)
{
    uint32_t count = g_face_detection_count;
    if (count == 0) return false;
    if (count > AI_MAX_DETECTION_NUM) count = AI_MAX_DETECTION_NUM;

    int32_t best_area = -1;
    int     best      = -1;
    for (uint32_t i = 0; i < count; i++) {
        face_detect_result_t *r = &g_face_detection_results[i];
        if (r->w <= 0 || r->h <= 0) continue;
        int32_t area = (int32_t)r->w * r->h;
        if (area > best_area) { best_area = area; best = (int)i; }
    }
    if (best < 0) return false;

    face_detect_result_t *r = &g_face_detection_results[best];
    *cx = (float)r->x + (float)r->w * 0.5f;
    *cy = (float)r->y + (float)r->h * 0.5f;
    return true;
}

static void smooth_update(float cx, float cy)
{
    if (!g_smooth_valid) {
        g_smooth_x = cx;
        g_smooth_y = cy;
        g_smooth_valid = true;
    } else {
        g_smooth_x += PPT_EMA_ALPHA * (cx - g_smooth_x);
        g_smooth_y += PPT_EMA_ALPHA * (cy - g_smooth_y);
    }
}

static void gest_reset(void)
{
    g_state        = GEST_IDLE;
    g_smooth_valid = false;
    g_lost_count   = 0;
}

/* 触发一次翻页 */
static void gest_fire(int dx)
{
    esp32_cmd_t cmd = (dx > 0) ? ESP32_CMD_PPT_PREV : ESP32_CMD_PPT_NEXT;
    esp32_uart_send_cmd(cmd);
    g_last_fire = xTaskGetTickCount();
    g_has_fired = true;
    printf("[PPT] swipe %s -> cmd 0x%02X\r\n",
           (dx > 0) ? "L->R" : "R->L", (unsigned)cmd);
}

static void ppt_gesture_tick(void)
{
    bool hand_mode = (face_detection_get_mode() == DETECTION_MODE_HAND);
    bool active    = g_ppt_on && hand_mode;

    /* 激活态边沿：进入/退出 PPT 模式 */
    if (active != g_ppt_active) {
        g_ppt_active = active;
        gest_reset();
        if (active) {
            esp32_uart_send_cmd(ESP32_CMD_PPT_ON);
            printf("[PPT] enter PPT mode\r\n");
        } else {
            esp32_uart_send_cmd(ESP32_CMD_PPT_OFF);
            printf("[PPT] exit PPT mode\r\n");
        }
    }

    if (!active) return;

    float cx, cy;
    if (!pick_primary_hand(&cx, &cy)) {
        /* 无手：连续若干帧后整体复位（短暂闪烁不复位） */
        if (g_state != GEST_IDLE) {
            if (++g_lost_count >= PPT_HAND_LOST_FRAMES) {
                gest_reset();
            }
        }
        return;
    }
    g_lost_count = 0;
    smooth_update(cx, cy);

    TickType_t now = xTaskGetTickCount();

    switch (g_state) {
    case GEST_IDLE:
        /* 首次出现手掌 → 记录起点开始跟踪 */
        g_ref_x       = g_smooth_x;
        g_ref_y       = g_smooth_y;
        g_track_start = now;
        g_state       = GEST_TRACKING;
        break;

    case GEST_TRACKING: {
        float dx  = g_smooth_x - g_ref_x;
        float dy  = g_smooth_y - g_ref_y;
        float adx = dx < 0 ? -dx : dx;
        float ady = dy < 0 ? -dy : dy;

        /* 时间窗超时：慢速漂移 → 以当前位置重新起算，不触发 */
        if ((now - g_track_start) >= pdMS_TO_TICKS(PPT_SWIPE_MAX_MS)) {
            g_ref_x       = g_smooth_x;
            g_ref_y       = g_smooth_y;
            g_track_start = now;
            break;
        }

        if (adx >= (float)PPT_SWIPE_MIN_DX &&
            adx >  (float)PPT_SWIPE_VERT_RATIO * ady) {
            if (!g_has_fired ||
                (now - g_last_fire) >= pdMS_TO_TICKS(PPT_COOLDOWN_MS)) {
                gest_fire(dx > 0 ? 1 : -1);
                g_state = GEST_REARM;   /* 须回中才能再次触发 */
            } else {
                /* 冷却中：丢弃本次位移，重新起算，避免冷却结束立即误触发 */
                g_ref_x       = g_smooth_x;
                g_ref_y       = g_smooth_y;
                g_track_start = now;
            }
        }
        break;
    }

    case GEST_REARM: {
        float dx  = g_smooth_x - g_ref_x;
        float adx = dx < 0 ? -dx : dx;
        /* 回到起点附近即重新武装，起点刷新为当前位置 */
        if (adx < (float)PPT_REARM_DX) {
            g_ref_x       = g_smooth_x;
            g_ref_y       = g_smooth_y;
            g_track_start = now;
            g_state       = GEST_TRACKING;
        }
        break;
    }
    }
}

static void ppt_task(void *pv)
{
    (void)pv;
    while (1) {
        ppt_gesture_tick();
        vTaskDelay(pdMS_TO_TICKS(PPT_TASK_PERIOD_MS));
    }
}

/* ---- 对外 API ---- */

void lvgl_ui_ppt_init(void)
{
    if (xTaskCreate(ppt_task, "ppt_gest", PPT_TASK_STACK, NULL,
                    PPT_TASK_PRIO, NULL) != pdPASS) {
        printf("[PPT] gesture task create failed\r\n");
        return;
    }
    printf("[PPT] gesture task started\r\n");
}

void lvgl_ui_ppt_set_on(bool on)
{
    g_ppt_on = on;
}

bool lvgl_ui_ppt_is_on(void)
{
    return g_ppt_on;
}
