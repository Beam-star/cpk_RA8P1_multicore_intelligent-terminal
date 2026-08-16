/**
 ******************************************************************************
 * @file    lvgl_ui_main.h
 * @brief   LVGL UI — face check-in / recording panel (public API)
 *
 * All functions are thread-safe.  Call from any FreeRTOS task.
 ******************************************************************************
 */

#ifndef LVGL_UI_MAIN_H_
#define LVGL_UI_MAIN_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One-time initialisation (after RM_LVGL_PORT_Open + GT911 init). */
void lvgl_ui_init(void);

/**
 * @brief  Append a line to the on-screen log window.
 * @param msg  Null-terminated string (will be copied immediately).
 */
void lvgl_ui_log(const char *msg);

/**
 * @brief  Update the recording timer display.
 * @param seconds  Elapsed recording time in seconds.
 */
void lvgl_ui_set_rec_time(uint32_t seconds);

/**
 * @brief  Set the bottom status bar text.
 */
void lvgl_ui_set_status(const char *msg);

/**
 * @brief  Start / stop the local recording timer.
 *
 * Called from RPMsg status task to sync timer state with CPU1.
 */
void lvgl_ui_recording_started(void);
void lvgl_ui_recording_stopped(void);

/** 是否正在录制 (供 page2 播放前检查互斥) */
bool lvgl_ui_is_recording(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_UI_MAIN_H_ */
