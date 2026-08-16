/**
 ******************************************************************************
 * @file    lvgl_ui_assets.c
 * @brief   UI assets loader — no-op（静态小人已被 lvgl_ui_anim.c 动图替换）。
 ******************************************************************************
 */

#include "lvgl_ui_assets.h"

void lvgl_ui_assets_load(void)
{
    /* 静态 mascot 已移除（原 300×398 RGB565 + 238 KB SDRAM 缓冲不再占用）。 */
}
