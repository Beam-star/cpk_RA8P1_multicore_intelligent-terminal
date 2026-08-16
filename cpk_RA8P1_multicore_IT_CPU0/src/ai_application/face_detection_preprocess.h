/**
 ******************************************************************************
 * @file    face_detection_preprocess.h
 * @brief   Image preprocessing for face detection (RGB565 → INT8 grayscale)
 ******************************************************************************
 */
#ifndef FACE_DETECTION_PREPROCESS_H__
#define FACE_DETECTION_PREPROCESS_H__

#include <stdint.h>

/**
 * @brief  Convert RGB565 image to INT8 grayscale for AI inference
 *
 * Processing steps:
 *   1. Crop center square from landscape image (e.g. 640x480 → 480x480)
 *   2. Nearest-neighbor downsample to output size (e.g. 480x480 → 192x192)
 *   3. Convert RGB565 to grayscale (approximate weighting)
 *   4. Convert to INT8 range (subtract 128)
 *
 * @param  p_input      Input RGB565 image buffer
 * @param  p_output     Output INT8 image buffer
 * @param  in_width     Input image width (pixels)
 * @param  in_height    Input image height (pixels)
 * @param  out_width    Output image width (pixels)
 * @param  out_height   Output image height (pixels)
 * @return 0 on success, -1 on error
 */
int face_detect_preprocess_rgb565_to_int8(const void *p_input, void *p_output,
                                           uint16_t in_width, uint16_t in_height,
                                           uint16_t out_width, uint16_t out_height);

/**
 * @brief  Hand-model preprocessing: RGB565 → INT8 grayscale (letterbox).
 *
 * Unlike the face model (centre-crop), the hand model uses aspect-ratio-
 * preserving letterbox resize with gray (114) padding, matching the training
 * pipeline.  Standard Rec.601 luminance grayscale.
 *
 * Processing:
 *   1. Letterbox-resize to fit (out_w × out_h) preserving aspect ratio
 *   2. Pad the letterbox borders with gray 114 (→ INT8 -14)
 *   3. RGB565 → Rec.601 luminance → INT8 (subtract 128)
 *
 * @param  p_input      Input RGB565 image buffer (in_width × in_height)
 * @param  p_output     Output INT8 buffer (out_width × out_height)
 * @return 0 on success, -1 on error
 */
int hand_detect_preprocess_rgb565_to_int8(const void *p_input, void *p_output,
                                          uint16_t in_width, uint16_t in_height,
                                          uint16_t out_width, uint16_t out_height);

#endif /* FACE_DETECTION_PREPROCESS_H__ */
