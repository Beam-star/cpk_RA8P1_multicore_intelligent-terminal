/**
 ******************************************************************************
 * @file    ui_fonts.h
 * @brief   UI 艺术字库声明（Bangers 标题 / Righteous 功能按钮 / Cinzel 短标签）
 *
 * 字库由 https://lvgl.io/tools/fontconverter 从 TTF 生成，.c 文件放在 fonts/ 下：
 *   - bangers_28.c     Bangers-Regular.ttf        28px  页面标题
 *   - righteous_20.c   Righteous-Regular.ttf      20px  功能按钮
 *   - cinzel_20.c      CinzelDecorative-Bold.ttf  20px  短标签切换 (Mode/ASR/AI/PPT)
 *
 * 各 .c 文件末尾定义 `const lv_font_t <name>`，这里用 LV_FONT_DECLARE 声明引用。
 ******************************************************************************
 */
#ifndef UI_FONTS_H_
#define UI_FONTS_H_

#include "lvgl.h"

LV_FONT_DECLARE(bangers_28);
LV_FONT_DECLARE(righteous_20);
LV_FONT_DECLARE(cinzel_20);

#endif /* UI_FONTS_H_ */
