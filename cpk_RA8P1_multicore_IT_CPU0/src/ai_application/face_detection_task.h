/**
 ******************************************************************************
 * @file    face_detection_task.h
 * @brief   Face detection FreeRTOS task (model loading + inference loop)
 ******************************************************************************
 */
#ifndef FACE_DETECTION_TASK_H__
#define FACE_DETECTION_TASK_H__

#include <stdint.h>
#include "face_detection_config.h"

/* Detection result structure — in CAMERA coordinate space (640×480).
 * The post-processor maps AI (96×96) boxes back to camera pixels using the
 * active model's preprocessing mapping (centre-crop for face, letterbox for
 * hand) before storing here, so the camera task needs no mode awareness to
 * draw them.
 *
 * `id` is the stable person index assigned by the temporal tracker (0-based →
 * label "person{id+1}"), in first-appearance order.  Hand mode does not track,
 * so `id` is 0 there (the label is simply "hand"). */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    int16_t id;
} face_detect_result_t;

/* Global detection results (written by AI task, read by camera task) */
extern face_detect_result_t g_face_detection_results[AI_MAX_DETECTION_NUM];
extern volatile uint32_t    g_face_detection_count;
extern volatile float       g_face_detect_fps;     /* smoothed FPS, updated each inference */

/* Global input buffer for detection (shared between preprocess and inference) */
extern int8_t g_face_detect_input_buffer[AI_INPUT_IMAGE_SIZE];

/**
 * @brief  Start the detection task (face + hand models).
 *
 * Creates a FreeRTOS task that:
 *   1. Loads BOTH models (face + hand) from W25Q256 to SDRAM
 *   2. Opens Ethos-U NPU driver
 *   3. Loops: preprocess frame → NPU inference (active model) → post-process
 */
void face_detection_task_start(void);

/**
 * @brief  Signal the detection task that a new camera frame is available.
 *
 * Called from the camera capture task after VIN DMA completes.
 * Passes the frame buffer address.
 *
 * @param  frame_addr  SDRAM address of the captured RGB565 frame
 */
void face_detection_signal_new_frame(uint32_t frame_addr);

/**
 * @brief  Set the active detection model (face or hand).
 *
 * Called from the LVGL UI mode-toggle button.  Takes effect on the next
 * inference frame.
 *
 * @param  mode  DETECTION_MODE_FACE or DETECTION_MODE_HAND
 */
void face_detection_set_mode(detection_mode_t mode);

/**
 * @brief  Get the active detection model.
 *
 * Called from the camera task to label boxes (personN vs handN).
 */
detection_mode_t face_detection_get_mode(void);

/**
 * @brief  Set the inference interval (process 1 frame every N camera frames).
 *
 * Lower N = smoother live preview but more NPU/SDRAM traffic; higher N frees
 * SDRAM bandwidth for MJPEG encode/decode during recording & video playback.
 * Default 2.  Takes effect on the next camera frame; safe to call from any
 * task (single-word volatile write).
 *
 * @param  n  interval in frames (clamped to >= 1)
 */
void face_detection_set_infer_interval(uint32_t n);

#endif /* FACE_DETECTION_TASK_H__ */
