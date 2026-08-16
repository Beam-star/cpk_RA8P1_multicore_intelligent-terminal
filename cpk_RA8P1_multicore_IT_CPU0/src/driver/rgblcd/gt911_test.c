/**
 ******************************************************************************
 * @file    gt911_test.c
 * @brief   GT911 触摸屏测试程序
 *
 * 功能:
 *   - 创建 FreeRTOS 任务, 以 10ms 周期轮询触摸屏
 *   - 检测到触摸时:
 *     * 打印触摸点数量和坐标到控制台
 *     * 可选地在 LCD 上绘制触摸轨迹 (小圆点)
 *   - 检测到抬起时打印释放信息
 *
 * 触摸坐标映射:
 *   GT911 原始坐标 → LCD 坐标 (直接对应, GT911 配置为 1024×600)
 *
 * 注: LCD 绘制功能与摄像头显示任务共享 framebuffer,
 *     触摸点绘制可能被摄像头画面覆盖, 属于正常现象。
 ******************************************************************************
 */

#include "gt911_test.h"
#include "gt911.h"
#include "rgblcd.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

/* ======================================================================== */
/*  任务配置                                                                  */
/* ======================================================================== */

#define TOUCH_TEST_TASK_STACK_SIZE  2048
#define TOUCH_TEST_TASK_PRIORITY    2       /* 低于摄像头任务(4)和触摸屏本身 */
#define TOUCH_TEST_POLL_INTERVAL_MS 10      /* 轮询间隔 (~100Hz) */

/* ======================================================================== */
/*  触摸点 LCD 绘制配置                                                        */
/* ======================================================================== */

#define TOUCH_DOT_RADIUS     6              /* 触摸点绘制半径(像素) */
#define TOUCH_DOT_COLOR       RGBLCD_COLOR_RED    /* 触摸点颜色 */
#define TOUCH_DOT_COLOR_OLD   RGBLCD_COLOR_BLACK  /* 旧触摸点擦除色 */

/* ======================================================================== */
/*  内部状态                                                                  */
/* ======================================================================== */

static bool g_lcd_draw_enabled = false;

/* 用于擦除上一帧的触摸点 (避免残影) */
static struct {
    bool     valid;
    uint16_t x;
    uint16_t y;
} g_prev_points[GT911_MAX_TOUCH_POINTS] = {0};

/* ======================================================================== */
/*  触摸测试任务                                                               */
/* ======================================================================== */

static void gt911_test_task(void *pvParameters)
{
    bool was_touched = false;
    uint32_t tick_count = 0;

    printf("[GT911 TEST] Task started (poll=%lums, LCD draw=%s)\r\n",
           (unsigned long)TOUCH_TEST_POLL_INTERVAL_MS,
           g_lcd_draw_enabled ? "ON" : "OFF");

    while (1) {
        /* 轮询触摸屏 (检查中断标志, GPIO fallback 保底) */
        if (gt911_scan()) {
            /* ---- 有新触摸数据 ---- */
            uint8_t num = g_gt911_touch.point_num;

            if (num > 0) {
                /* 检测到触摸按下 */
                if (!was_touched) {
                    printf("[GT911 TEST] Touch DOWN: %d point(s)\r\n", num);
                    was_touched = true;
                }

                /* 打印每个触摸点坐标 */
                for (uint8_t i = 0; i < num; i++) {
                    gt911_touch_point_t *p = &g_gt911_touch.points[i];
                    printf("  TP%d: id=%d X=%5u Y=%5u\r\n",
                           i + 1, p->id, p->x, p->y);

                    /* LCD 绘制: 在触摸点位置画小圆 */
                    if (g_lcd_draw_enabled) {
                        rgblcd_fill_circle(p->x, p->y,
                                           TOUCH_DOT_RADIUS, TOUCH_DOT_COLOR);
                    }
                }

                /*
                 * 擦除不再存在的旧触摸点 (多点触摸时手指移开)
                 * 原理: 如果上一帧有 N 个点, 当前帧只有 M 个点 (M < N),
                 *       则擦除多余的旧点。
                 */
                if (g_lcd_draw_enabled && num < GT911_MAX_TOUCH_POINTS) {
                    for (uint8_t i = num; i < GT911_MAX_TOUCH_POINTS; i++) {
                        if (g_prev_points[i].valid) {
                            rgblcd_fill_circle(g_prev_points[i].x,
                                               g_prev_points[i].y,
                                               TOUCH_DOT_RADIUS + 2,
                                               TOUCH_DOT_COLOR_OLD);
                            g_prev_points[i].valid = false;
                        }
                    }
                }

                /* 保存当前触摸点位置, 供下一帧擦除 */
                if (g_lcd_draw_enabled) {
                    for (uint8_t i = 0; i < num; i++) {
                        g_prev_points[i].valid = true;
                        g_prev_points[i].x = g_gt911_touch.points[i].x;
                        g_prev_points[i].y = g_gt911_touch.points[i].y;
                    }
                }
            }
        } else {
            /* ---- 无触摸数据 ---- */
            if (was_touched) {
                printf("[GT911 TEST] Touch UP (released)\r\n");
                was_touched = false;

                /* 擦除所有旧触摸点 */
                if (g_lcd_draw_enabled) {
                    for (uint8_t i = 0; i < GT911_MAX_TOUCH_POINTS; i++) {
                        if (g_prev_points[i].valid) {
                            rgblcd_fill_circle(g_prev_points[i].x,
                                               g_prev_points[i].y,
                                               TOUCH_DOT_RADIUS + 2,
                                               TOUCH_DOT_COLOR_OLD);
                            g_prev_points[i].valid = false;
                        }
                    }
                }
            }
        }

        /* 每 100 次循环打印心跳 (1 秒) */
        tick_count++;
        if (tick_count % 100 == 0) {
            printf("[GT911 TEST] Heartbeat #%lu\r\n",
                   (unsigned long)(tick_count / 100));
        }

        vTaskDelay(pdMS_TO_TICKS(TOUCH_TEST_POLL_INTERVAL_MS));
    }
}

/* ======================================================================== */
/*  Public API                                                               */
/* ======================================================================== */

void gt911_test_start(bool enable_lcd_draw)
{
    g_lcd_draw_enabled = enable_lcd_draw;

    BaseType_t ret = xTaskCreate(gt911_test_task,
                                  "touch_test",
                                  TOUCH_TEST_TASK_STACK_SIZE,
                                  NULL,
                                  TOUCH_TEST_TASK_PRIORITY,
                                  NULL);
    if (ret != pdPASS) {
        printf("[GT911 TEST] Failed to create task!\r\n");
    } else {
        printf("[GT911 TEST] Task created\r\n");
    }
}
