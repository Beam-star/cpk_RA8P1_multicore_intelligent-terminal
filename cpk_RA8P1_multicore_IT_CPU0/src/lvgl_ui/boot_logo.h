/*
 * boot_logo.h
 *
 * Static boot logo for MODE_CAMERA. Replaces the old animated boot sequence.
 *
 * Shows a full-screen 1024x600 raw RGB565 image (a competition logo) on GLCDC
 * graphics layer 1 (fb_background[0]) â€?the same layer the camera later uses â€? * holds it for a fixed time, then returns so the camera task can take over.
 *
 * The image is stored in the W25Q256 asset directory under the name "logo"
 * (headerless RGB565, little-endian, exactly 1024*600*2 = 1,228,800 bytes).
 * If the asset is missing or the wrong size, a dark screen is shown instead and
 * boot still proceeds (never blocks startup on a missing asset).
 */

#ifndef LVGL_UI_BOOT_LOGO_H_
#define LVGL_UI_BOOT_LOGO_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Show the static boot logo on GLCDC layer 1, then hold.
 *
 * Preconditions: w25q256_open() has been called (XIP available) and the GLCDC
 * display is running (RM_LVGL_PORT_Open() done). Must be called from a FreeRTOS
 * task context (uses vTaskDelay for the hold). The camera task overwrites
 * layer 1 afterwards.
 *
 * @param hold_ms  How long to keep the logo on screen (milliseconds). 0 = no hold.
 */
void boot_logo_show_layer1(uint32_t hold_ms);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_UI_BOOT_LOGO_H_ */
