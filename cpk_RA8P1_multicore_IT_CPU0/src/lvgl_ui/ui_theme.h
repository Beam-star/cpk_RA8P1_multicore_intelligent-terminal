/**
 ******************************************************************************
 * @file    ui_theme.h
 * @brief   统一的 UI 主题工具（背景渐变等）
 *
 * 所有页面共用同一套「深蓝 → 青」双色竖向渐变背景，保证视觉一致。
 * 如需调整渐变色，只改这里即可。
 ******************************************************************************
 */
#ifndef UI_THEME_H_
#define UI_THEME_H_

#include "lvgl.h"

/* 双色竖向渐变：顶部深蓝 → 底部深蓝 */
static inline void ui_apply_bg_gradient(lv_obj_t *o)
{
    static lv_grad_dsc_t grad;
    static bool inited = false;
    if (!inited) {
        static lv_color_t colors[2];
        colors[0] = lv_color_hex(0x0A2545);   /* 顶部：深蓝 */
        colors[1] = lv_color_hex(0x0E5A78);   /* 底部：青   0x0E5A78*/
        lv_grad_vertical_init(&grad);
        lv_grad_init_stops(&grad, colors, NULL, NULL, 2);
        inited = true;
    }
    lv_obj_set_style_bg_grad(o, &grad, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

#endif /* UI_THEME_H_ */
