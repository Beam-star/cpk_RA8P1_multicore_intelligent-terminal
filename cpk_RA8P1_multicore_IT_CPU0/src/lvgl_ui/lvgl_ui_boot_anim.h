/*
 * lvgl_ui_boot_anim.h
 *
 * LVGL boot animation player.
 *
 * Loads pre-programmed RGB565 frames from W25Q256 QSPI Flash.
 * Two loading strategies (tried in order):
 *   1. Asset directory scan â€?reads the flash asset directory at
 *      PART_ASSET_DIR_OFFSET, finds entries named "boot_000".."boot_NNN",
 *      and plays variable-size frames at their stored offsets.
 *   2. Hardcoded fallback â€?if the directory is empty or invalid, reads
 *      fixed-size frames from BOOT_ANIM_FLASH_OFFSET (legacy mode).
 *
 * Frame format (both strategies):
 *   Raw RGB565 pixel data, one contiguous region per frame.
 *
 * Memory:
 *   Requires two frame buffers in SDRAM (double-buffered for tear-free
 *   playback).  Each buffer is BOOT_ANIM_MAX_FRAME_SIZE bytes.
 */

#ifndef LVGL_UI_BOOT_ANIM_H_
#define LVGL_UI_BOOT_ANIM_H_

#include "lvgl.h"

/* -------------------------------------------------------------------------- */
/* Configuration (edit these to match your animation)                         */
/* -------------------------------------------------------------------------- */

#define BOOT_ANIM_WIDTH         480         /* Frame width  (pixels)            */
#define BOOT_ANIM_HEIGHT        270         /* Frame height (pixels)            */
#define BOOT_ANIM_FPS           4           /* Target frame rate                */
#define BOOT_ANIM_FRAME_SIZE    ((uint32_t)(BOOT_ANIM_WIDTH) * (BOOT_ANIM_HEIGHT) * 2)

/* ---- Hardcoded fallback (used when asset directory is empty) ---- */
#define BOOT_ANIM_FRAME_COUNT   14          /* Number of frames (legacy)        */
#define BOOT_ANIM_FLASH_OFFSET  0x0006A000UL /* Legacy offset (after AI model) */

/* ---- Asset directory mode ---- */
#define BOOT_ANIM_MAX_FRAMES    128         /* Max frames from asset directory  */
#define BOOT_ANIM_MAX_FRAME_SIZE ((uint32_t)BOOT_ANIM_WIDTH * BOOT_ANIM_HEIGHT * 2)
                                            /* Worst-case uncompressed frame    */

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief  Type of the callback invoked when the animation finishes.
 */
typedef void (*boot_anim_done_cb_t)(void);

/**
 * @brief  Start the boot animation on the active screen.
 *
 * Loads frames from W25Q256 at ::BOOT_ANIM_FLASH_OFFSET, creates a full-screen
 * LVGL image object, and starts a timer that loads and displays each
 * frame at ::BOOT_ANIM_FPS.
 *
 * @param parent   LVGL parent object (typically lv_scr_act()).
 * @param on_done  Called after the last frame or if no valid data is found.
 *                 If NULL the animation runs once silently.
 */
void boot_anim_start(lv_obj_t *parent, boot_anim_done_cb_t on_done);

/**
 * @brief  Immediately stop the animation and free resources.
 */
void boot_anim_stop(void);

/**
 * @brief  Scan the flash asset directory for boot animation frames.
 *
 * Reads the asset directory from PART_ASSET_DIR_OFFSET via memory-mapped XIP,
 * counts entries named "boot_%03d", and returns the number of frames found.
 * If the directory is invalid or empty, returns 0 (caller should fall back
 * to hardcoded BOOT_ANIM_FRAME_COUNT / BOOT_ANIM_FLASH_OFFSET).
 *
 * @return Number of boot animation frames found (0 = directory empty/invalid).
 */
int boot_anim_scan_dir(void);

#endif /* LVGL_UI_BOOT_ANIM_H_ */
