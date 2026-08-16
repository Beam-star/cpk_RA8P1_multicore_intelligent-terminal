/**
 ******************************************************************************
 * @file    lvgl_ui_assets.h
 * @brief   UI assets loader.
 *
 * 静态小人（mascot）已替换为 lvgl_ui_anim.c 的动图，本模块退化为 no-op，
 * 仅保留 lvgl_ui_assets_load() 以维持 lvgl_ui_init() 的调用接口不变。
 ******************************************************************************
 */
#ifndef LVGL_UI_ASSETS_H_
#define LVGL_UI_ASSETS_H_

#ifdef __cplusplus
extern "C" {
#endif

/** 保留兼容的空实现：不再加载任何静态图片。 */
void lvgl_ui_assets_load(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_UI_ASSETS_H_ */
