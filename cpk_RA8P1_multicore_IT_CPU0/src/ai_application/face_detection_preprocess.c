/**
 ******************************************************************************
 * @file    face_detection_preprocess.c
 * @brief   Image preprocessing for face detection (RGB565 → INT8 grayscale)
 *
 * v2 model input: 96×96×1 INT8 grayscale (was 192×192 in v1).
 *
 * Pipeline: 640×480 RGB565 → center crop 480×480 → nearest-neighbor 96×96
 *           → INT8 [-128, 127].
 ******************************************************************************
 */
#include "face_detection_build_mode.h"

#if !FACE_DETECT_FLASH_PROGRAMMER

#include "face_detection_preprocess.h"
#include <stddef.h>

int face_detect_preprocess_rgb565_to_int8(const void *p_input, void *p_output,
                                           uint16_t in_width, uint16_t in_height,
                                           uint16_t out_width, uint16_t out_height)
{
    if ((p_input == NULL) || (p_output == NULL)) {
        return -1;
    }

    /* Input must be landscape (width >= height) */
    if (in_width < in_height) {
        return -1;
    }

    const uint16_t *p_in  = (const uint16_t *)p_input;
    int8_t         *p_out = (int8_t *)p_output;

    /* Pixels to skip at the start of each line to crop to center square */
    const uint32_t crop_offset = (in_width - in_height) / 2;

    for (uint32_t y = 0; y < out_height; y++) {
        for (uint32_t x = 0; x < out_width; x++) {
            /* Number of pixels in the Y-offset */
            uint32_t y_offset = in_width * ((in_height * y) / out_height);

            /* Pointer to the pixel at the start of the target cropped line */
            const uint16_t *p_input_base = p_in + crop_offset + y_offset;

            /* Add X-offset for input pixels in cropped image */
            uint16_t input = *(p_input_base + ((in_height * x) / out_width));

            /* Approximate RGB565 to grayscale weighting: R*0.25 + G*0.5 + B*0.125 */
            /* RGB565: RRRRRGGGGGGBBBBB */
            uint8_t weighted_sum = (uint8_t)(((input >> 11) << 1) +
                                             ((input >> 4) & 0x7E) +
                                             (input & 0x1F));

            /* Convert to INT8 range [-128, 127] */
            *p_out++ = (int8_t)(weighted_sum - 0x80);
        }
    }

    return 0;
}

int hand_detect_preprocess_rgb565_to_int8(const void *p_input, void *p_output,
                                          uint16_t in_width, uint16_t in_height,
                                          uint16_t out_width, uint16_t out_height)
{
    if ((p_input == NULL) || (p_output == NULL)) {
        return -1;
    }

    const uint16_t *p_in  = (const uint16_t *)p_input;
    int8_t         *p_out = (int8_t *)p_output;

    /* Letterbox ratio: preserve aspect ratio, fit inside out_width×out_height */
    float ratio = (float)out_height / (float)in_height;
    float ratio_w = (float)out_width / (float)in_width;
    if (ratio_w < ratio) ratio = ratio_w;

    int new_w = (int)((float)in_width  * ratio + 0.5f);
    int new_h = (int)((float)in_height * ratio + 0.5f);
    if (new_w < 1) new_w = 1;
    if (new_h < 1) new_h = 1;

    const int offset_x = (out_width  - new_w) / 2;
    const int offset_y = (out_height - new_h) / 2;

    /* Gray 114 pad → INT8 = 114 - 128 = -14 (matches training letterbox fill). */
    const int8_t pad_val = (int8_t)(114 - 128);

    for (uint32_t y = 0; y < out_height; y++) {
        for (uint32_t x = 0; x < out_width; x++) {
            int8_t *dst = &p_out[y * out_width + x];

            /* Outside the letterbox content region → pad */
            if ((int)x < offset_x || (int)x >= offset_x + new_w ||
                (int)y < offset_y || (int)y >= offset_y + new_h) {
                *dst = pad_val;
                continue;
            }

            /* Nearest-neighbour source pixel (consistent with face model) */
            uint32_t src_x = (uint32_t)((int)x - offset_x) * in_width  / (uint32_t)new_w;
            uint32_t src_y = (uint32_t)((int)y - offset_y) * in_height / (uint32_t)new_h;
            if (src_x >= in_width)  src_x = in_width  - 1;
            if (src_y >= in_height) src_y = in_height - 1;

            uint16_t pix = p_in[src_y * in_width + src_x];

            /* Rec.601 luminance (matches cv2.cvtColor BGR2GRAY):
             *   Y = 0.299R + 0.587G + 0.114B ≈ (77R + 150G + 29B) >> 8 */
            uint8_t r8 = (uint8_t)(((pix >> 11) & 0x1F) << 3 | ((pix >> 11) & 0x1F) >> 2);
            uint8_t g8 = (uint8_t)(((pix >> 5)  & 0x3F) << 2 | ((pix >> 5)  & 0x3F) >> 4);
            uint8_t b8 = (uint8_t)(( pix        & 0x1F) << 3 | ( pix        & 0x1F) >> 2);
            uint8_t gray = (uint8_t)((77u * r8 + 150u * g8 + 29u * b8) >> 8);

            *dst = (int8_t)(gray - 128);
        }
    }

    return 0;
}

#endif /* !FACE_DETECT_FLASH_PROGRAMMER */
