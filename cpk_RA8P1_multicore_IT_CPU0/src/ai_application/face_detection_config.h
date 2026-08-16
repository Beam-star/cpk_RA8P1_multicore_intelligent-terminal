/**
 ******************************************************************************
 * @file    face_detection_config.h
 * @brief   Face detection AI application configuration (v2 model)
 *
 * Model: custom 96×96 grayscale anchor-free face detector
 *        MobileNetV2 width 0.5 + SPPF + lightweight detect heads
 *        Compiled for Ethos-U55-256 NPU via RUHMI
 *
 * Input:  96×96×1 INT8 grayscale (9,216 bytes)
 * Output: Two INT8 tensors �?box distances [1,4,180] + scores [1,1,180]
 ******************************************************************************
 */
#ifndef FACE_DETECTION_CONFIG_H__
#define FACE_DETECTION_CONFIG_H__

#include <stdint.h>

/* ---- Detection Mode ----
 *
 * The NPU can run one of two 96×96×1 anchor-free detectors (identical
 * architecture, different weights + decode params):
 *   DETECTION_MODE_FACE  — face detector (person1/person2/... labels)
 *   DETECTION_MODE_HAND  — hand detector  (hand1/hand2/... labels)
 *
 * Selected from the LVGL UI mode-toggle button (replaces the old Enroll
 * button).  The detection task loads BOTH models to SDRAM at boot and
 * switches the active model on mode change. */
typedef enum {
    DETECTION_MODE_FACE = 0,
    DETECTION_MODE_HAND = 1,
} detection_mode_t;

/* ---- AI Model Input Parameters ---- */
#define AI_INPUT_IMAGE_WIDTH            (96)
#define AI_INPUT_IMAGE_HEIGHT           (96)
#define AI_INPUT_IMAGE_BYTE_PER_PIXEL   (1)     /* Grayscale */
#define AI_INPUT_IMAGE_SIZE             (AI_INPUT_IMAGE_WIDTH * AI_INPUT_IMAGE_HEIGHT * AI_INPUT_IMAGE_BYTE_PER_PIXEL)  /* 9216 */

/* ---- Detection Parameters ---- */
#define AI_MAX_DETECTION_NUM            (20)
#define AI_DETECTION_THRESHOLD          (0.45f)  /* Confidence threshold */
#define AI_NMS_THRESHOLD                (0.45f)  /* NMS IoU threshold   */
#define AI_NUM_CLASSES                  (1)      /* Face only           */

/* ---- Inference interval (process 1 frame every N camera frames) ----
 *
 * Lower N = smoother live preview but more NPU/SDRAM traffic; higher N frees
 * SDRAM bandwidth for MJPEG encode/decode.  face_detection_set_infer_interval()
 * switches between these two on recording/playback state transitions (see
 * rpmsg_record_cpu0.c).  Adjust here to tune the preview/record trade-off. */
#define INFER_EVERY_N_PREVIEW           (3U)     /* live preview (idle)       */
#define INFER_EVERY_N_RECORD            (3U)     /* recording / video playback */

/* ---- Output Tensor Dimensions ---- */
/* v2 model is anchor-free: outputs are box distances (ltrb) and scores
 * at 180 grid points (12×12 + 6×6 = 144 + 36 = 180). */
#define AI_OUTPUT_GRID_POINTS           (180)
#define AI_OUTPUT_BOX_CHANNELS          (4)      /* left, top, right, bottom */
#define AI_OUTPUT_SCORE_CHANNELS        (1)

/* ---- Output Tensor Quantization ----
 *
 * Extracted from deploy_v3/model/face_int8.tflite via ai_edge_litert:
 *   OUTPUT "PartitionedCall:0": [1,4,180] box logits,  scale=0.0654573  zp=-124
 *   OUTPUT "PartitionedCall:1": [1,1,180] score logits, scale=0.0010920  zp=127  */
#define AI_OUTPUT_BOX_SCALE             (0.0654572919011116f)
#define AI_OUTPUT_BOX_ZERO_POINT        (-124)
#define AI_OUTPUT_SCORE_SCALE           (0.0010920179774984717f)
#define AI_OUTPUT_SCORE_ZERO_POINT      (127)

/* ---- Hand model output tensor quantization ----
 *
 * Extracted from deploy_hand_v2/model/gesture_int8.tflite via ai_edge_litert:
 *   OUTPUT "PartitionedCall:0": [1,4,180] box logits,   scale=0.0251124  zp=-117
 *   OUTPUT "PartitionedCall:1": [1,1,180] score logits, scale=0.0101225  zp=-100 */
#define HAND_OUTPUT_BOX_SCALE           (0.02511235699057579f)
#define HAND_OUTPUT_BOX_ZERO_POINT      (-117)
#define HAND_OUTPUT_SCORE_SCALE         (0.010122505016624928f)
#define HAND_OUTPUT_SCORE_ZERO_POINT    (-100)

/* ---- Model Storage (W25Q256 Flash �?SDRAM) ---- */
#define FACE_MODEL_FLASH_OFFSET         (0x00000000)
#define FACE_MODEL_DATA_SIZE            (250416)
#define FACE_MODEL_COMMAND_STREAM_SIZE  (11980)
/* Command stream must start on a 256-byte page boundary, otherwise
 * W25Q256 Page Program wraps at page end, corrupting the write. */
#define FACE_MODEL_CMDSTREAM_FLASH_OFFSET  (((FACE_MODEL_DATA_SIZE) + 255) & ~255)  /* 0x0003D300 */
#define FACE_MODEL_FLASH_TOTAL_SIZE     (FACE_MODEL_CMDSTREAM_FLASH_OFFSET + FACE_MODEL_COMMAND_STREAM_SIZE)

/* SDRAM layout (CPU0 16MB: 0x68000000�?x68FFFFFF):
 *
 *   0x68000000 �?0x681C207F  VIN DMA buffers (3 × 614400 B)
 *   0x681C2080 �?0x6841A07F  fb_background[0..1] (2 × 1024×600×2)
 *
 *   0x68500000 �?0x68535FFF  NPU Tensor Arena (216 KB)              �?v3 arena
 *   0x68536000 �?0x6853FFFF  (gap)
 *   0x68540000 �?0x6857D22F  Model weights  (250,416 B)             �?v3 weights
 *   0x6857D230 �?0x685800F3  Command stream  ( 11,980 B)            �?v3 cmd stream
 *
 *   0x68600000 �?0x68623FFF  Embed NPU Arena (144 KB)
 *   0x68630000 �?0x686E47FF  Embed model weights (~721 KB) */

/* v3 model in SDRAM */
#define FACE_MODEL_ARENA_ADDR           (0x68500000)   /* 216 KB arena      */
#define FACE_MODEL_SDRAM_ADDR           (0x68540000)   /* 245 KB weights    */

/* ---- Hand model storage (W25Q256 Flash → SDRAM) ----
 *
 * The hand model reuses the SAME NPU arena as the face model (0x68500000,
 * 216 KB) — both are 96×96×1 with identical tensor layout, and only one
 * runs at a time.  Its weights are placed in the freed face-embedding
 * region (0x68600000). */
#define HAND_MODEL_ARENA_ADDR           (FACE_MODEL_ARENA_ADDR)   /* shared arena */
#define HAND_MODEL_SDRAM_ADDR           (0x68600000)   /* 229,568 B hand weights */

#define HAND_MODEL_FLASH_OFFSET         (0x00B00000)
#define HAND_MODEL_DATA_SIZE            (229568)
#define HAND_MODEL_COMMAND_STREAM_SIZE  (11988)
/* Command stream at a clean 256 KB boundary after the weights partition. */
#define HAND_MODEL_CMDSTREAM_FLASH_OFFSET  (0x00B40000)

/* ---- Hand model letterbox preprocessing (640×480 → 96×96) ----
 *
 * The hand model was trained with aspect-ratio-preserving letterbox resize
 * (NOT the face model's centre-crop).  For a 640×480 camera frame:
 *   ratio    = min(96/480, 96/640) = 96/640 = 0.15
 *   new_w    = round(640 × 0.15)   = 96
 *   new_h    = round(480 × 0.15)   = 72
 *   offset_x = (96 - 96) / 2       = 0
 *   offset_y = (96 - 72) / 2       = 12
 * Camera-space mapping:  cam = (ai - offset) / ratio  */
#define HAND_LETTERBOX_RATIO            (0.15f)
#define HAND_LETTERBOX_OFFSET_X         (0)
#define HAND_LETTERBOX_OFFSET_Y         (12)

/* ---- Camera Input ---- */
/* Source camera image: 640×480 RGB565 (from OV5640 via VIN DMA) */
#define CAMERA_INPUT_WIDTH              (640)
#define CAMERA_INPUT_HEIGHT             (480)

/* ---- Task Configuration ---- */
#define FACE_DETECT_TASK_STACK_SIZE     (4096)  /* raised from 2048: v3 NMS needs ~4.3KB for candidates */
#define FACE_DETECT_TASK_PRIORITY       (2)

#endif /* FACE_DETECTION_CONFIG_H__ */
