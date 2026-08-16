/**
 ****************************************************************************************************
 * @file        rgblcd.h
 * @brief       RGB LCD驱动头文件 (1024x600 RGB565, GLCDC, D/AVE 2D + CPU 回退)
 ****************************************************************************************************
 * 硬件依赖:
 *   - GLCDC 显示控制器 (g_display0)
 *   - D/AVE 2D 图形加速引擎 (d2_handle0, 可选, 由 RGBLCD_USE_D2D 宏控制)
 *   - GPT Timer 7 PWM 背光控制 (g_timer7, GTIOCB)
 *   - SDRAM 中的 framebuffer (fb_background)
 ****************************************************************************************************
 */

#ifndef RGBLCD_RGBLCD_H_
#define RGBLCD_RGBLCD_H_

#include "bsp_api.h"
#include <stdint.h>

FSP_HEADER

/* ======================================================================== */
/*  常量定义                                                                  */
/* ======================================================================== */

#define RGBLCD_WIDTH            (1024)      /* LCD 宽度(像素) */
#define RGBLCD_HEIGHT           (600)       /* LCD 高度(像素) */
#define RGBLCD_STRIDE_PIXELS    (1024)      /* framebuffer 行跨度(像素), 必须与GLCDC配置一致 */
#define RGBLCD_STRIDE_BYTES     (2048)      /* framebuffer 行跨度(字节), 1024*2=2048 */

/* 常用 RGB565 颜色 */
#define RGBLCD_COLOR_BLACK      (0x0000)
#define RGBLCD_COLOR_WHITE      (0xFFFF)
#define RGBLCD_COLOR_RED        (0xF800)
#define RGBLCD_COLOR_GREEN      (0x07E0)
#define RGBLCD_COLOR_BLUE       (0x001F)
#define RGBLCD_COLOR_YELLOW     (0xFFE0)
#define RGBLCD_COLOR_CYAN       (0x07FF)
#define RGBLCD_COLOR_MAGENTA    (0xF81F)

/* 字体大小 */
#define RGBLCD_FONT_16          (16)        /* 16x16 ASCII (实际宽度8) */
#define RGBLCD_FONT_24          (24)        /* 24x24 ASCII (实际宽度12) */
#define RGBLCD_FONT_32          (32)        /* 32x32 ASCII (实际宽度16) */

/* ======================================================================== */
/*  颜色转换宏                                                                */
/* ======================================================================== */

/** 将 24位 RGB 转换为 RGB565 */
#define RGB_TO_RGB565(r, g, b)  ((uint16_t)(((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | ((b) >> 3)))

/* ======================================================================== */
/*  初始化/控制 API                                                            */
/* ======================================================================== */

/**
 * @brief 初始化 RGB LCD (GLCDC + D/AVE 2D + 背光 PWM)
 *
 * @note  如果 LVGL FSP port 层 (rm_lvgl_port) 已接管 GLCDC,
 *        则改用 rgblcd_backlight_init() + RM_LVGL_PORT_Open()
 *
 * @return FSP_SUCCESS 成功, 其他失败
 */
fsp_err_t rgblcd_init(void);

/**
 * @brief 仅初始化背光 PWM (GPT Timer 7), 不初始化 GLCDC
 *
 * 在 LVGL FSP port 接管 GLCDC 的场景下使用。
 *
 * @return FSP_SUCCESS 成功, 其他失败
 */
fsp_err_t rgblcd_backlight_init(void);

/**
 * @brief 设置背光亮度
 * @param percent 亮度百分比 (0-100)
 */
void rgblcd_backlight_set(uint8_t percent);

/* ======================================================================== */
/*  基础像素操作 (直接操作 framebuffer, CPU)                                     */
/* ======================================================================== */

/**
 * @brief 全屏清屏
 * @param color RGB565 颜色值
 */
void rgblcd_clear(uint16_t color);

/**
 * @brief 画点
 * @param x     X 坐标 (0 ~ 1023)
 * @param y     Y 坐标 (0 ~ 599)
 * @param color RGB565 颜色值
 */
void rgblcd_draw_point(uint16_t x, uint16_t y, uint16_t color);

/**
 * @brief 读取点的颜色
 * @param x X 坐标
 * @param y Y 坐标
 * @return RGB565 颜色值
 */
uint16_t rgblcd_read_point(uint16_t x, uint16_t y);

/**
 * @brief 矩形填充 (CPU 操作)
 * @param x     起始 X
 * @param y     起始 Y
 * @param w     宽度
 * @param h     高度
 * @param color RGB565 颜色值
 */
void rgblcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/* ======================================================================== */
/*  图形操作 (D/AVE 2D 优先, CPU 回退)                                          */
/* ======================================================================== */

/**
 * @brief 画线 (D/AVE 2D 加速, 失败则 CPU Bresenham)
 * @param x1,y1 起点坐标
 * @param x2,y2 终点坐标
 * @param color  RGB565 颜色值
 * @param width  线宽(像素)
 */
void rgblcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                      uint16_t color, uint16_t width);

/**
 * @brief 画矩形边框 (D/AVE 2D 加速, 失败则 CPU)
 * @param x,y   左上角坐标
 * @param w,h   宽高
 * @param color  RGB565 颜色值
 * @param width  边框线宽
 */
void rgblcd_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      uint16_t color, uint16_t width);

/**
 * @brief 加速矩形填充 (D/AVE 2D 加速, 失败则 CPU)
 */
void rgblcd_fill_rect_accel(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            uint16_t color);

/**
 * @brief 画圆边框 (D/AVE 2D 加速, 失败则 CPU 中点圆)
 * @param cx,cy 圆心坐标
 * @param r     半径
 * @param color RGB565 颜色值
 * @param width 边框线宽
 */
void rgblcd_draw_circle(uint16_t cx, uint16_t cy, uint16_t r,
                        uint16_t color, uint16_t width);

/**
 * @brief 填充圆 (D/AVE 2D 加速, 失败则 CPU)
 */
void rgblcd_fill_circle(uint16_t cx, uint16_t cy, uint16_t r, uint16_t color);

/* ======================================================================== */
/*  文字绘制 (CPU 操作, 逐像素写入 framebuffer)                                  */
/* ======================================================================== */

/**
 * @brief 绘制单个 ASCII 字符
 * @param x,y       左上角坐标
 * @param ch        字符 (ASCII 32~126)
 * @param color     前景色 RGB565
 * @param bg_color  背景色 RGB565 (0 表示透明背景)
 * @param size      字体大小: RGBLCD_FONT_16/24/32
 */
void rgblcd_draw_char(uint16_t x, uint16_t y, char ch,
                      uint16_t color, uint16_t bg_color, uint8_t size);

/**
 * @brief 绘制 ASCII 字符串
 * @param x,y       左上角坐标
 * @param str       字符串
 * @param color     前景色
 * @param bg_color  背景色 (0=透明)
 * @param size      字体大小
 */
void rgblcd_draw_string(uint16_t x, uint16_t y, const char *str,
                        uint16_t color, uint16_t bg_color, uint8_t size);

/**
 * @brief 绘制中英文混合字符串 (GB2312 编码)
 * @details 英文字符使用指定 size 字体, 中文字符使用 24x24 字体
 * @param x,y       左上角坐标
 * @param str       GB2312 编码字符串
 * @param color     前景色
 * @param bg_color  背景色 (0=透明)
 * @param size      英文字体大小 (中文固定 24x24)
 */
void rgblcd_draw_string_cn(uint16_t x, uint16_t y, const char *str,
                           uint16_t color, uint16_t bg_color, uint8_t size);

FSP_FOOTER
#endif /* RGBLCD_RGBLCD_H_ */
