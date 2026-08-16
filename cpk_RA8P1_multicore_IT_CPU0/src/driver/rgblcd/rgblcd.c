/**
 ****************************************************************************************************
 * @file        rgblcd.c
 * @brief       RGB LCD 驱动实现 (1024x600 RGB565, GLCDC, D/AVE 2D + CPU 回退)
 ****************************************************************************************************
 * 硬件平台: Titan-mini (RA8P1, Cortex-M85)
 * 显示控制器: GLCDC (g_display0)
 * 图形加速: D/AVE 2D (d2_handle0), 若初始化失败自动回退 CPU 绘图
 * 背光控制: GPT Timer 7 PWM (g_timer7, GTIOCB)
 * 像素格式: RGB565 (GLCDC Color Order = BGR, 硬件自动做 R/B 交换)
 * 字模格式: PC2LCD2002, 阴码+逐列式+顺向+C51格式 (MSB = 顶部)
 ****************************************************************************************************
 */

#include "rgblcd.h"
#include "hal_data.h"
#include "common_data.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "FreeRTOS.h"
#include "task.h"

/* ======================================================================== */
/*  内部宏                                                                    */
/* ======================================================================== */

/** D/AVE 2D 硬件加速开关: 0=CPU绘图, 1=D/AVE 2D加速 */
#define RGBLCD_USE_D2D      (0)
/*是否是CPU0控制RGBLCD — CPU0 需要管理 D-Cache */
#define CPU0_USE_RGBLCD     (1)

#if RGBLCD_USE_D2D
#include "dave_driver.h"
extern d2_device *d2_handle0;
#endif


/*DCache相关操作只有CPU0才需要管理*/
/* D-Cache 已开启, GLCDC 直接从 SDRAM 读取, 必须手动刷新缓存 */
#if CPU0_USE_RGBLCD
extern void SCB_CleanDCache_by_Addr (volatile void *addr, int32_t dsize);
#endif

#if CPU0_USE_RGBLCD
static void fb_cache_clean(volatile void *addr, uint32_t size)
{
    uint32_t start = (uint32_t)addr & ~31U;
    int32_t  len   = (int32_t)((((uint32_t)addr + size + 31) & ~31U) - start);
    SCB_CleanDCache_by_Addr((uint32_t *)start, len);
}
#endif
/**
 * @brief GLCDC 垂直同步回调 (每帧结束时由硬件中断触发)
 * @note  common_data.c 中注册了此回调, 必须提供实现
 */
volatile uint32_t g_frame_count = 0;
volatile uint32_t g_vsync_last_event = 0;
void DisplayVsyncCallback(display_callback_args_t *p_args)
{
    if (p_args) {
        g_vsync_last_event = p_args->event;
    }
    g_frame_count++;
}

/* 字体数据 (PC2LCD2002 取模: 阴码+逐列式+顺向+C51格式, MSB=顶部) */
#include "lcdfont.h"


#if RGBLCD_USE_D2D
/* D/AVE 2D 定点坐标转换 (整数 -> 1:11:4 定点) */
#define D2_FIX4(x)      ((d2_point)((x) << 4))

/* RGB565 -> ARGB8888 */
#define RGB565_TO_ARGB8888(c) \
    ((d2_color)(0xFF000000 | \
     (((uint32_t)(c) & 0xF800) << 8) | \
     (((uint32_t)(c) & 0x07E0) << 5) | \
     (((uint32_t)(c) & 0x001F) << 3)))

static d2_context  *gp_d2_ctx  = NULL;
static d2_renderbuffer *gp_d2_rb = NULL;

static void d2_render_begin(uint16_t color)
{
    d2_color argb = RGB565_TO_ARGB8888(color);
    d2_selectrenderbuffer(d2_handle0, gp_d2_rb);
    d2_startframe(d2_handle0);
    d2_selectcontext(d2_handle0, gp_d2_ctx);
    d2_cliprect(d2_handle0, 0, 0,
                (d2_border)(RGBLCD_WIDTH - 1),
                (d2_border)(RGBLCD_HEIGHT - 1));
    d2_setcolor(d2_handle0, 0, argb);
    d2_setalpha(d2_handle0, 255);
}

static void d2_render_end(void)
{
    d2_endframe(d2_handle0);
    d2_executerenderbuffer(d2_handle0, gp_d2_rb, d2_ef_default);
}
#endif /* RGBLCD_USE_D2D */

/* ======================================================================== */
/*  CPU 绘图回退函数                                                          */
/* ======================================================================== */

static void cpu_draw_line(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                          uint16_t color, uint16_t width)
{
    int16_t dx  = abs((int16_t)x1 - (int16_t)x0);
    int16_t dy  = abs((int16_t)y1 - (int16_t)y0);
    int16_t sx  = (x0 < x1) ? 1 : -1;
    int16_t sy  = (y0 < y1) ? 1 : -1;
    int16_t err = dx - dy;
    int16_t hw  = (int16_t)(width / 2);

    while (1) {
        for (int16_t wy = -hw; wy <= hw; wy++) {
            for (int16_t wx = -hw; wx <= hw; wx++) {
                rgblcd_draw_point((uint16_t)((int16_t)x0 + wx),
                                  (uint16_t)((int16_t)y0 + wy), color);
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int16_t e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 = (uint16_t)((int16_t)x0 + sx); }
        if (e2 <  dx) { err += dx; y0 = (uint16_t)((int16_t)y0 + sy); }
    }
}

static void cpu_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          uint16_t color, uint16_t width)
{
    if (width < 1) width = 1;
    rgblcd_fill_rect(x, y, w, width, color);
    rgblcd_fill_rect(x, (uint16_t)(y + h - width), w, width, color);
    rgblcd_fill_rect(x, (uint16_t)(y + width), width,
                     (uint16_t)(h - 2 * width), color);
    rgblcd_fill_rect((uint16_t)(x + w - width), (uint16_t)(y + width), width,
                     (uint16_t)(h - 2 * width), color);
}

static void cpu_draw_circle(uint16_t cx, uint16_t cy, uint16_t r,
                            uint16_t color, uint16_t width)
{
    int16_t x   = (int16_t)r;
    int16_t y   = 0;
    int16_t err = 0;
    int16_t hw  = (int16_t)(width / 2);

    while (x >= y) {
        for (int16_t i = -hw; i <= hw; i++) {
            rgblcd_draw_point((uint16_t)(cx + x), (uint16_t)(cy + y + i), color);
            rgblcd_draw_point((uint16_t)(cx + y + i), (uint16_t)(cy + x), color);
            rgblcd_draw_point((uint16_t)(cx - y - i), (uint16_t)(cy + x), color);
            rgblcd_draw_point((uint16_t)(cx - x), (uint16_t)(cy + y + i), color);
            rgblcd_draw_point((uint16_t)(cx - x), (uint16_t)(cy - y - i), color);
            rgblcd_draw_point((uint16_t)(cx - y - i), (uint16_t)(cy - x), color);
            rgblcd_draw_point((uint16_t)(cx + y + i), (uint16_t)(cy - x), color);
            rgblcd_draw_point((uint16_t)(cx + x), (uint16_t)(cy - y - i), color);
        }
        y++;
        if (err <= 0) { err += 2 * y + 1; }
        if (err >  0) { x--; err -= 2 * x + 1; }
    }
}

static void cpu_fill_circle(uint16_t cx, uint16_t cy, uint16_t r, uint16_t color)
{
    int16_t x   = (int16_t)r;
    int16_t y   = 0;
    int16_t err = 0;

    while (x >= y) {
        rgblcd_fill_rect((uint16_t)(cx - x), (uint16_t)(cy + y),
                         (uint16_t)(2 * x + 1), 1, color);
        rgblcd_fill_rect((uint16_t)(cx - x), (uint16_t)(cy - y),
                         (uint16_t)(2 * x + 1), 1, color);
        rgblcd_fill_rect((uint16_t)(cx - y), (uint16_t)(cy + x),
                         (uint16_t)(2 * y + 1), 1, color);
        rgblcd_fill_rect((uint16_t)(cx - y), (uint16_t)(cy - x),
                         (uint16_t)(2 * y + 1), 1, color);
        y++;
        if (err <= 0) { err += 2 * y + 1; }
        if (err >  0) { x--; err -= 2 * x + 1; }
    }
}

/* ======================================================================== */
/*  初始化/控制                                                               */
/* ======================================================================== */

fsp_err_t rgblcd_init(void)
{
    fsp_err_t err;

    /* 1. 清 framebuffer 为蓝色 */
    uint16_t *fb = (uint16_t *)fb_background[0];
    for (uint32_t i = 0; i < (uint32_t)RGBLCD_STRIDE_PIXELS * RGBLCD_HEIGHT; i++) {
        fb[i] = RGBLCD_COLOR_BLUE;
    }
#if CPU0_USE_RGBLCD
    fb_cache_clean(fb, (uint32_t)RGBLCD_STRIDE_BYTES * RGBLCD_HEIGHT);
#endif
    /* 2. 打开 GLCDC
     *
     * 重要: g_display0_cfg.p_callback 在 FSP 配置中硬编码为
     * _rm_lvgl_port_display_callback (common_data.c:235).
     * 该回调在 VSYNC ISR 中调用 xSemaphoreGiveFromISR(g_semaphore_vpos),
     * 而 g_semaphore_vpos 仅在 RM_LVGL_PORT_Open() 中被创建.
     *
     * Camera 模式下未调用 RM_LVGL_PORT_Open(), 所以必须用本地配置
     * 副本将回调解耦为 DisplayVsyncCallback(仅递增 g_frame_count).
     *
     * LVGL Demo 模式使用 RM_LVGL_PORT_Open() 打开 GLCDC,
     * 不受此影响 (g_semaphore_vpos 已正确初始化).
     */
    display_cfg_t local_cfg = *g_display0.p_cfg;
    local_cfg.p_callback = DisplayVsyncCallback;
    err = g_display0.p_api->open(g_display0.p_ctrl, &local_cfg);
    if (err != FSP_SUCCESS) return err;

    /* 3. 启动 GLCDC */
    err = g_display0.p_api->start(g_display0.p_ctrl);
    if (err != FSP_SUCCESS) return err;

    /* 4. 初始化背光 PWM (Timer 7) */
    err = g_timer7.p_api->open(g_timer7.p_ctrl, g_timer7.p_cfg);
    if (err != FSP_SUCCESS) return err;
    g_timer7.p_api->dutyCycleSet(g_timer7.p_ctrl, 0, GPT_IO_PIN_GTIOCB);
    R_GPT_OutputEnable(g_timer7.p_ctrl, GPT_IO_PIN_GTIOCB);
    g_timer7.p_api->start(g_timer7.p_ctrl);

    /* 5. 初始化 D/AVE 2D (由 RGBLCD_USE_D2D 宏控制) */
#if RGBLCD_USE_D2D
    printf("[RGBLCD] D/AVE 2D init...\r\n");
    d2_handle0 = d2_opendevice(0);
    if (d2_handle0 != NULL) {
        d2_inithw(d2_handle0, 0);
        d2_u32 hwrev = d2_getrevisionhw(d2_handle0);
        printf("[RGBLCD] D/AVE 2D hwrev=0x%08lx\r\n", (unsigned long)hwrev);
        if (hwrev != 0) {
            d2_framebuffer(d2_handle0, fb_background[0],
                           RGBLCD_STRIDE_BYTES, RGBLCD_WIDTH, RGBLCD_HEIGHT,
                           d2_mode_rgb565);
            gp_d2_ctx = d2_newcontext(d2_handle0);
            gp_d2_rb  = d2_newrenderbuffer(d2_handle0, 2048, 256);
            if (gp_d2_ctx && gp_d2_rb) {
                printf("[RGBLCD] D/AVE 2D ready\r\n");
            }
        }
    }
#else
    printf("[RGBLCD] D/AVE 2D disabled (RGBLCD_USE_D2D=0)\r\n");
#endif

    return FSP_SUCCESS;
}

void rgblcd_backlight_set(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t period = g_timer7_cfg.period_counts;
    uint32_t duty   = ((uint32_t)percent * period) / 100;
    g_timer7.p_api->dutyCycleSet(g_timer7.p_ctrl, duty, GPT_IO_PIN_GTIOCB);
}

fsp_err_t rgblcd_backlight_init(void)
{
    fsp_err_t err;

    /* Open PWM timer for backlight (GPT Timer 7) */
    err = g_timer7.p_api->open(g_timer7.p_ctrl, g_timer7.p_cfg);
    if (err != FSP_SUCCESS) return err;

    g_timer7.p_api->dutyCycleSet(g_timer7.p_ctrl, 0, GPT_IO_PIN_GTIOCB);
    R_GPT_OutputEnable(g_timer7.p_ctrl, GPT_IO_PIN_GTIOCB);
    g_timer7.p_api->start(g_timer7.p_ctrl);

    return FSP_SUCCESS;
}

/* ======================================================================== */
/*  基础像素操作 (CPU 直接写 framebuffer)                                       */
/* ======================================================================== */

void rgblcd_clear(uint16_t color)
{
    uint16_t *fb = (uint16_t *)fb_background[0];
    uint32_t total = (uint32_t)RGBLCD_STRIDE_PIXELS * RGBLCD_HEIGHT;
    for (uint32_t i = 0; i < total; i++) {
        fb[i] = color;
    }
#if CPU0_USE_RGBLCD
    fb_cache_clean(fb, (uint32_t)RGBLCD_STRIDE_BYTES * RGBLCD_HEIGHT);
#endif
}

void rgblcd_draw_point(uint16_t x, uint16_t y, uint16_t color)
{
    if (x >= RGBLCD_WIDTH || y >= RGBLCD_HEIGHT) return;
    uint16_t *fb = (uint16_t *)fb_background[0];
    fb[y * RGBLCD_STRIDE_PIXELS + x] = color;
#if CPU0_USE_RGBLCD
    fb_cache_clean(&fb[y * RGBLCD_STRIDE_PIXELS + x], sizeof(uint16_t));
#endif
}

uint16_t rgblcd_read_point(uint16_t x, uint16_t y)
{
    if (x >= RGBLCD_WIDTH || y >= RGBLCD_HEIGHT) return 0;
    uint16_t *fb = (uint16_t *)fb_background[0];
    return fb[y * RGBLCD_STRIDE_PIXELS + x];
}

void rgblcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (x >= RGBLCD_WIDTH || y >= RGBLCD_HEIGHT) return;
    if (x + w > RGBLCD_WIDTH)  w = RGBLCD_WIDTH  - x;
    if (y + h > RGBLCD_HEIGHT) h = RGBLCD_HEIGHT - y;

    uint16_t *fb = (uint16_t *)fb_background[0];
    for (uint16_t row = 0; row < h; row++) {
        uint16_t *line = &fb[(y + row) * RGBLCD_STRIDE_PIXELS + x];
        for (uint16_t col = 0; col < w; col++) {
            line[col] = color;
        }
    }
#if CPU0_USE_RGBLCD
    fb_cache_clean(&fb[y * RGBLCD_STRIDE_PIXELS + x],
                   (uint32_t)h * RGBLCD_STRIDE_BYTES);
#endif
}

/* ======================================================================== */
/*  图形操作 (D/AVE 2D 优先, CPU 回退)                                         */
/* ======================================================================== */

void rgblcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                      uint16_t color, uint16_t width)
{
    if (width < 1) width = 1;
#if RGBLCD_USE_D2D
    d2_render_begin(color);
    d2_outlinewidth(d2_handle0, (d2_width)((uint32_t)width << 4));
    d2_renderline(d2_handle0,
                  D2_FIX4(x1), D2_FIX4(y1),
                  D2_FIX4(x2), D2_FIX4(y2),
                  (d2_width)((uint32_t)width << 4),
                  d2_le_exclude_none);
    d2_render_end();
#else
    cpu_draw_line(x1, y1, x2, y2, color, width);
#endif
}

void rgblcd_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      uint16_t color, uint16_t width)
{
    if (width < 1) width = 1;
#if RGBLCD_USE_D2D
    d2_render_begin(color);
    d2_outlinewidth(d2_handle0, (d2_width)((uint32_t)width << 4));
    d2_point x1 = D2_FIX4(x);
    d2_point y1 = D2_FIX4(y);
    d2_point x2 = D2_FIX4(x + w);
    d2_point y2 = D2_FIX4(y + h);
    d2_width lw  = (d2_width)((uint32_t)width << 4);
    d2_renderline(d2_handle0, x1, y1, x2, y1, lw, d2_le_exclude_none);
    d2_renderline(d2_handle0, x2, y1, x2, y2, lw, d2_le_exclude_none);
    d2_renderline(d2_handle0, x2, y2, x1, y2, lw, d2_le_exclude_none);
    d2_renderline(d2_handle0, x1, y2, x1, y1, lw, d2_le_exclude_none);
    d2_render_end();
#else
    cpu_draw_rect(x, y, w, h, color, width);
#endif
}

void rgblcd_fill_rect_accel(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            uint16_t color)
{
#if RGBLCD_USE_D2D
    d2_render_begin(color);
    d2_renderbox(d2_handle0,
                 D2_FIX4(x), D2_FIX4(y),
                 D2_FIX4(w), D2_FIX4(h));
    d2_render_end();
#else
    rgblcd_fill_rect(x, y, w, h, color);
#endif
}

void rgblcd_draw_circle(uint16_t cx, uint16_t cy, uint16_t r,
                        uint16_t color, uint16_t width)
{
    if (width < 1) width = 1;
#if RGBLCD_USE_D2D
    d2_render_begin(color);
    d2_outlinewidth(d2_handle0, (d2_width)((uint32_t)width << 4));
    d2_rendercircle(d2_handle0,
                    D2_FIX4(cx), D2_FIX4(cy),
                    D2_FIX4(r),
                    (d2_width)((uint32_t)width << 4));
    d2_render_end();
#else
    cpu_draw_circle(cx, cy, r, color, width);
#endif
}

void rgblcd_fill_circle(uint16_t cx, uint16_t cy, uint16_t r, uint16_t color)
{
#if RGBLCD_USE_D2D
    d2_render_begin(color);
    d2_rendercircle(d2_handle0,
                    D2_FIX4(cx), D2_FIX4(cy),
                    D2_FIX4(r),
                    D2_FIX4(r));
    d2_render_end();
#else
    cpu_fill_circle(cx, cy, r, color);
#endif
}

/* ======================================================================== */
/*  文字绘制 (始终 CPU)                                                        */
/* ======================================================================== */

static void draw_char_internal(uint16_t x, uint16_t y, uint16_t color,
                               uint16_t bg_color, uint8_t char_width,
                               uint8_t char_height, const unsigned char *font_data)
{
    uint8_t bytes_per_col = (char_height + 7) / 8;
    uint16_t csize = (uint16_t)bytes_per_col * char_width;
    uint16_t *fb = (uint16_t *)fb_background[0];
    uint16_t y0 = y;

    for (uint16_t t = 0; t < csize; t++) {
        uint8_t temp = font_data[t];
        for (uint8_t t1 = 0; t1 < 8; t1++) {
            if ((temp & 0x80) != 0) {
                fb[y * RGBLCD_STRIDE_PIXELS + x] = color;
            } else if (bg_color != 0) {
                fb[y * RGBLCD_STRIDE_PIXELS + x] = bg_color;
            }
            temp <<= 1;
            y++;
            if ((y - y0) == char_height) {
                y = y0;
                x++;
            }
        }
    }
#if CPU0_USE_RGBLCD
    fb_cache_clean(&fb[y0 * RGBLCD_STRIDE_PIXELS],
                   (uint32_t)char_height * RGBLCD_STRIDE_BYTES);
#endif
}

void rgblcd_draw_char(uint16_t x, uint16_t y, char ch,
                      uint16_t color, uint16_t bg_color, uint8_t size)
{
    if (ch < 32 || ch > 126) return;

    uint8_t idx = ch - 32;
    const unsigned char *font_data = NULL;
    uint8_t char_width  = 0;
    uint8_t char_height = 0;

    switch (size) {
        case RGBLCD_FONT_16:
            font_data = asc2_1608[idx]; char_width =  8; char_height = 16; break;
        case RGBLCD_FONT_24:
            font_data = asc2_2412[idx]; char_width = 12; char_height = 24; break;
        case RGBLCD_FONT_32:
            font_data = asc2_3216[idx]; char_width = 16; char_height = 32; break;
        default:
            return;
    }

    draw_char_internal(x, y, color, bg_color, char_width, char_height, font_data);
}

void rgblcd_draw_string(uint16_t x, uint16_t y, const char *str,
                        uint16_t color, uint16_t bg_color, uint8_t size)
{
    if (str == NULL) return;

    uint8_t char_width;
    switch (size) {
        case RGBLCD_FONT_16: char_width =  8; break;
        case RGBLCD_FONT_24: char_width = 12; break;
        case RGBLCD_FONT_32: char_width = 16; break;
        default: return;
    }

    while (*str) {
        if (*str == '\n') { x = 0; y += size; str++; continue; }
        if (x + char_width > RGBLCD_WIDTH) { x = 0; y += size; }
        if (y + size > RGBLCD_HEIGHT) break;
        rgblcd_draw_char(x, y, *str, color, bg_color, size);
        x += char_width;
        str++;
    }
}

void rgblcd_draw_string_cn(uint16_t x, uint16_t y, const char *str,
                           uint16_t color, uint16_t bg_color, uint8_t size)
{
    if (str == NULL) return;

    uint8_t ascii_width;
    switch (size) {
        case RGBLCD_FONT_16: ascii_width =  8; break;
        case RGBLCD_FONT_24: ascii_width = 12; break;
        case RGBLCD_FONT_32: ascii_width = 16; break;
        default: return;
    }

    while (*str) {
        if (*str == '\n') { x = 0; y += size; str++; continue; }

        if ((uint8_t)*str < 0x80) {
            if (x + ascii_width > RGBLCD_WIDTH) { x = 0; y += size; }
            if (y + size > RGBLCD_HEIGHT) break;
            rgblcd_draw_char(x, y, *str, color, bg_color, size);
            x += ascii_width;
            str++;
        } else {
            if (x + 24 > RGBLCD_WIDTH) { x = 0; y += 24; }
            if (y + 24 > RGBLCD_HEIGHT) break;
            rgblcd_fill_rect(x, y, 24, 24, bg_color ? bg_color : RGBLCD_COLOR_BLACK);
            rgblcd_draw_rect(x + 2, y + 2, 20, 20, color, 1);
            x += 24;
            str += 2;
        }
    }
}
