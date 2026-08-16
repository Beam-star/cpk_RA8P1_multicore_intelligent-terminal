/**
 ****************************************************************************************************
 * @file        rgblcd_test.c
 * @brief       RGB LCD 驱动测试程序
 ****************************************************************************************************
 */

#include "rgblcd.h"
#include "hal_data.h"
#include <stdio.h>
#include "rpmsg_core.h"
#include "rpmsg_log.h"
#include "FreeRTOS.h"
#include "task.h"

#define TEST_DELAY_MS   2000

static void test_delay(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void test_solid_colors(void)
{
	printf("[TEST] Solid colors...\r\n");

    rgblcd_clear(RGBLCD_COLOR_RED);
    rgblcd_draw_string(10, 10, "RED", RGBLCD_COLOR_WHITE, 0, RGBLCD_FONT_32);
    printf("  RED ok\r\n");
    test_delay(TEST_DELAY_MS);

    rgblcd_clear(RGBLCD_COLOR_GREEN);
    rgblcd_draw_string(10, 10, "GREEN", RGBLCD_COLOR_BLACK, 0, RGBLCD_FONT_32);
    printf("  GREEN ok\r\n");
    test_delay(TEST_DELAY_MS);

    rgblcd_clear(RGBLCD_COLOR_BLUE);
    rgblcd_draw_string(10, 10, "BLUE", RGBLCD_COLOR_WHITE, 0, RGBLCD_FONT_32);
    printf("  BLUE ok\r\n");
    test_delay(TEST_DELAY_MS);

    rgblcd_clear(RGBLCD_COLOR_WHITE);
    rgblcd_draw_string(10, 10, "WHITE", RGBLCD_COLOR_BLACK, 0, RGBLCD_FONT_32);
    printf("  WHITE ok\r\n");
    test_delay(TEST_DELAY_MS);

    rgblcd_clear(RGBLCD_COLOR_BLACK);
    rgblcd_draw_string(10, 10, "BLACK", RGBLCD_COLOR_WHITE, 0, RGBLCD_FONT_32);
    printf("  BLACK ok\r\n");
    test_delay(TEST_DELAY_MS);

    printf("[TEST] Solid colors done\r\n");
}

static void test_shapes(void)
{
    printf("[TEST] Shapes...\r\n");
    rgblcd_clear(RGBLCD_COLOR_BLACK);
    printf("  clear ok\r\n");

    /* CPU 填充矩形 */
    rgblcd_fill_rect(100, 100, 200, 150, RGBLCD_COLOR_RED);
    printf("  fill_rect RED ok\r\n");
    rgblcd_fill_rect(350, 100, 200, 150, RGBLCD_COLOR_GREEN);
    printf("  fill_rect GREEN ok\r\n");
    rgblcd_fill_rect(600, 100, 200, 150, RGBLCD_COLOR_BLUE);
    printf("  fill_rect BLUE ok\r\n");

    /* 边框 */
    rgblcd_draw_rect(50, 50, 924, 500, RGBLCD_COLOR_WHITE, 2);
    printf("  draw_rect ok\r\n");

    /* 十字线 */
    rgblcd_draw_line(512, 50, 512, 550, RGBLCD_COLOR_YELLOW, 1);
    printf("  draw_line V ok\r\n");
    rgblcd_draw_line(50, 300, 974, 300, RGBLCD_COLOR_CYAN, 1);
    printf("  draw_line H ok\r\n");

    /* 圆 */
    rgblcd_draw_circle(256, 420, 80, RGBLCD_COLOR_GREEN, 2);
    printf("  draw_circle ok\r\n");
    rgblcd_fill_circle(512, 420, 60, RGBLCD_COLOR_MAGENTA);
    printf("  fill_circle 1 ok\r\n");
    rgblcd_fill_circle(768, 420, 60, RGBLCD_COLOR_YELLOW);
    printf("  fill_circle 2 ok\r\n");

    printf("[TEST] Shapes done\r\n");
    test_delay(TEST_DELAY_MS * 2);
}

static void test_text(void)
{
    printf("[TEST] Text...\r\n");
    rgblcd_clear(RGBLCD_COLOR_BLACK);

    rgblcd_draw_string(10,  10, "Font16: Hello RA8P1!", RGBLCD_COLOR_CYAN,   0, RGBLCD_FONT_16);
    rgblcd_draw_string(10,  80, "Font24: Hello RA8P1!", RGBLCD_COLOR_YELLOW, 0, RGBLCD_FONT_24);
    rgblcd_draw_string(10, 130, "Font32: Hello RA8P1!", RGBLCD_COLOR_RED,    0, RGBLCD_FONT_32);

    rgblcd_draw_string(10, 200, "White on Blue", RGBLCD_COLOR_WHITE, RGBLCD_COLOR_BLUE, RGBLCD_FONT_24);
    rgblcd_draw_string(10, 240, "Red on Yellow", RGBLCD_COLOR_RED,   RGBLCD_COLOR_YELLOW, RGBLCD_FONT_24);

    rgblcd_draw_string(10, 300, "0123456789 !@#$%^&*()", RGBLCD_COLOR_WHITE, 0, RGBLCD_FONT_16);
    rgblcd_draw_string(10, 330, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", RGBLCD_COLOR_GREEN, 0, RGBLCD_FONT_16);
    rgblcd_draw_string(10, 360, "abcdefghijklmnopqrstuvwxyz", RGBLCD_COLOR_CYAN,  0, RGBLCD_FONT_16);

    printf("[TEST] Text done\r\n");
    test_delay(TEST_DELAY_MS * 2);
}

static void test_backlight(void)
{
    printf("[TEST] Backlight...\r\n");
    rgblcd_clear(RGBLCD_COLOR_WHITE);
    rgblcd_draw_string(100, 250, "Backlight Test", RGBLCD_COLOR_BLACK, 0, RGBLCD_FONT_32);
    rgblcd_draw_string(100, 300, "Brightness changing...", RGBLCD_COLOR_BLACK, 0, RGBLCD_FONT_16);

    for (uint8_t i = 0; i <= 100; i += 2) {
        rgblcd_backlight_set(i);
        test_delay(30);
    }
    test_delay(500);

    for (int16_t i = 100; i >= 0; i -= 2) {
        rgblcd_backlight_set((uint8_t)i);
        test_delay(30);
    }
    test_delay(500);

    rgblcd_backlight_set(80);
    printf("[TEST] Backlight done\r\n");
    test_delay(TEST_DELAY_MS);
}

/* ======================================================================== */
/*  测试入口                                                                  */
/* ======================================================================== */

void rgblcd_test_run(void)
{
    printf("[RGBLCD] Initializing...\r\n");
    fsp_err_t err = rgblcd_init();
    if (err != FSP_SUCCESS) {
        printf("[RGBLCD] Init failed: %ld\r\n", (long)err);
        return;
    }
    printf("[RGBLCD] Init OK\r\n");

    rgblcd_backlight_set(80);

    test_solid_colors();
    test_shapes();
    test_text();
    test_backlight();

    rgblcd_clear(RGBLCD_COLOR_BLACK);
    rgblcd_draw_string(10, 10, "All tests passed!", RGBLCD_COLOR_GREEN, 0, RGBLCD_FONT_32);
    rgblcd_draw_string(10, 60, "RGBLCD driver OK", RGBLCD_COLOR_WHITE, 0, RGBLCD_FONT_24);
    rgblcd_backlight_set(80);

    printf("[RGBLCD] All tests passed!\r\n");
}
