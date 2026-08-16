/**
 ******************************************************************************
 * @file    lvgl_ui_page4.h
 * @brief   UI 第四页 — 语音命令词说明 (CI1302 声控命令参考)
 *
 * 页面内容：一个可滚动的中文列表，解释每个语音命令词及其对应的功能，
 * 供用户查阅「说什么词 → 触发什么功能」。
 ******************************************************************************
 */

#ifndef LVGL_UI_PAGE4_H_
#define LVGL_UI_PAGE4_H_

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建第四页（语音命令说明），新建一个独立 screen。 */
void lvgl_ui_page4_init(void);

/** 返回第四页 screen 对象（供翻页导航使用）。 */
lv_obj_t *lvgl_ui_page4_get_screen(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_UI_PAGE4_H_ */
