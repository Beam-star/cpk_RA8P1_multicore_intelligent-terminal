/**
 ******************************************************************************
 * @file    lvgl_ui_anim.h
 * @brief   UI frame players — animated mascot + fingerprint loading spinner.
 *
 * All frames are PRELOADED to SDRAM at boot (lvgl_ui_anim_init, called while
 * OSPI_B is still open).  At runtime the timer only switches the image source
 * pointer — no XIP read, no memcpy — so it survives `w25q256_close()` (which
 * the AI model loader calls and which disables the XIP memory map).
 ******************************************************************************
 */
#ifndef LVGL_UI_ANIM_H_
#define LVGL_UI_ANIM_H_

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- W25Q256 offsets ---- */
#define MASCOT_ANIM_OFFSET       0x00210000UL   /* 14 × 128×128×2 */
#define LOADING_ANIM_OFFSET      0x00290000UL   /* 20 × 96×96×2   */

/* ---- Mascot (page-1 idle loop) ---- */
#define MASCOT_ANIM_W            128
#define MASCOT_ANIM_H            128
#define MASCOT_ANIM_FRAMES       14
#define MASCOT_ANIM_FRAME_SIZE   (MASCOT_ANIM_W * MASCOT_ANIM_H * 2)
#define MASCOT_ANIM_TOTAL        (MASCOT_ANIM_FRAME_SIZE * MASCOT_ANIM_FRAMES)

/* ---- Loading spinner (fingerprint enroll/identify) ---- */
#define LOADING_ANIM_W           96
#define LOADING_ANIM_H           96
#define LOADING_ANIM_FRAMES      20
#define LOADING_ANIM_FRAME_SIZE  (LOADING_ANIM_W * LOADING_ANIM_H * 2)
#define LOADING_ANIM_TOTAL       (LOADING_ANIM_FRAME_SIZE * LOADING_ANIM_FRAMES)

/** Preload both frame sets from W25Q256 to SDRAM. Call once at boot while
 *  w25q256_open() is still active (before the AI model loader closes it). */
void lvgl_ui_anim_init(void);

/** Create + position + start the looping animated mascot on @p parent. */
lv_obj_t *lvgl_ui_mascot_start(lv_obj_t *parent, lv_coord_t x, lv_coord_t y);

/** Stop the mascot animation and free its image. */
void lvgl_ui_mascot_stop(void);

/** 取消回调（如指纹取消），点右上角 ✕ 时触发。 */
typedef void (*lvgl_ui_loading_cancel_cb_t)(void);

/** Show a centred modal spinner + text (top layer, persists across pages).
 *  @p on_cancel 非 NULL 时，在右上角加一个 ✕ 取消按钮。 */
void lvgl_ui_loading_show(const char *text, lvgl_ui_loading_cancel_cb_t on_cancel);

/** Hide the loading spinner (no-op if not shown). */
void lvgl_ui_loading_hide(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_UI_ANIM_H_ */
