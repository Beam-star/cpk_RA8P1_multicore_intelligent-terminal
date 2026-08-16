/**
 ******************************************************************************
 * @file    face_embedding_task.h
 * @brief   Face embedding model �?NPU inference for 128-d feature vector.
 *
 * MobileNetV2 0.35× backbone + Conv2D 1×1 embedding head.
 * Input:  96×96×3 INT8 (RGB, [-1,1] normalised �?INT8)
 * Output: 128-d FP32 embedding (L2-normalised on CPU after NPU)
 *
 * The model weights (~728 KB) and command stream (~43 KB) are loaded from
 * W25Q256 to SDRAM at boot.  The NPU tensor arena (144 KB) is also in SDRAM.
 ******************************************************************************
 */

#ifndef FACE_EMBEDDING_TASK_H_
#define FACE_EMBEDDING_TASK_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** SDRAM addresses for the embedding model (CPU0 region, 0x68000000-0x68FFFFFF).
 *
 *  YOLO model:  arena 0x68500000 (442 KB) / weights 0x68570000 (433 KB)
 *  Embedding:   arena 0x68600000 (144 KB) / weights 0x68630000   �?256 KB gap for safety
 */
#define EMBED_ARENA_ADDR     0x68600000u
#define EMBED_ARENA_SIZE     147456u       /* kArenaSize_sub_0001 = 144 KB */
#define EMBED_MODEL_SDRAM    0x68630000u   /* model weights loaded from flash */

/* Model storage on W25Q256 */
#define EMBED_MODEL_FLASH_OFFSET      0x00B00000u   /* PART_EMBED_MODEL_OFFSET  */
#define EMBED_MODEL_DATA_SIZE         738672u       /* INT8 weights (from sub_0001_model_data.c) */
#define EMBED_CMDSTREAM_FLASH_OFFSET  0x00BC0000u   /* PART_EMBED_CMDSTREAM_OFFSET */
#define EMBED_CMDSTREAM_SIZE          7000u         /* command stream (from sub_0001_command_stream.c) */

/** 128-d float embedding (L2-normalised). */
#define EMBED_DIM  128

/**
 * @brief  Load model weights + command stream from W25Q256 to SDRAM,
 *         open the Ethos-U NPU, and configure the AXI limiter.
 * @return true on success.
 */
bool face_embedding_init(void);

/**
 * @brief  Run NPU inference on a preprocessed 96×96×3 INT8 input.
 *
 * @param input_96x96x3  Preprocessed image (96×96×3 INT8), flattened row-major.
 *                       Pixel range: INT8 [-128, 127] (maps to [-1.0, 1.0]).
 * @param embedding_out  [out] 128-d FP32 L2-normalised embedding vector.
 * @return true on success.
 */
bool face_embedding_run(const int8_t *input_96x96x3, float embedding_out[EMBED_DIM]);

#ifdef __cplusplus
}
#endif

#endif /* FACE_EMBEDDING_TASK_H_ */
