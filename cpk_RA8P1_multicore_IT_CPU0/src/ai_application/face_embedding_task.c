/**
 ******************************************************************************
 * @file    face_embedding_task.c
 * @brief   Face embedding model �?load from W25Q256 �?SDRAM, NPU inference.
 *
 * Uses MobileNetV2 0.35× compiled to 100 % NPU (Ethos-U55).
 * Input:  96×96×3 INT8 (pixel = (float_pixel - 127.5) / 127.5 * 127)
 * Output: 128-d FP32 L2-normalised embedding.
 ******************************************************************************
 */
#include "face_embedding_task.h"
#include "embed_model/sub_0001_invoke.h"
#include "embed_model/sub_0001_tensors.h"

#include "driver/w25q256/w25q256.h"
#include "driver/w25q256/w25q256_partition.h"

#include "common_data.h"
#include "rm_ethosu_api.h"
#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

/* ---- Set by embed_invoke.c ---- */
extern void embed_set_model_data(const uint8_t *data);
extern void embed_set_cms(const uint8_t *cms, size_t sz);

/* ---- D-Cache management ---- */
extern void SCB_CleanDCache_by_Addr(volatile void *addr, int32_t dsize);

/* ---- Output dequantization params (verified against FP32 model) ---- */
#define OUT_SCALE  0.0193069773f  /* 1/51.8 �?matched to MERA INT8 quantizer */
#define OUT_ZP     0

/* ---- Static state ---- */
static bool g_embed_ready = false;

/* ======================================================================== */
/*  Public API                                                               */
/* ======================================================================== */

bool face_embedding_init(void)
{
    printf("[EMBED] Loading embedding model from W25Q256...\r\n");

    w25q256_err_t werr = w25q256_open();
    if (werr != W25Q256_OK) {
        printf("[EMBED] W25Q256 open failed: %s\r\n", w25q256_err_str(werr));
        return false;
    }

    uint8_t *model_sdram = (uint8_t *)EMBED_MODEL_SDRAM;
    uint8_t *cms_sdram   = model_sdram + EMBED_MODEL_DATA_SIZE;

    /* ---- Read model weights ---- */
    printf("[EMBED] Reading %lu KB weights �?SDRAM 0x%08lX...\r\n",
           (unsigned long)(EMBED_MODEL_DATA_SIZE / 1024),
           (unsigned long)model_sdram);
    werr = w25q256_read(EMBED_MODEL_FLASH_OFFSET, model_sdram, EMBED_MODEL_DATA_SIZE);
    if (werr != W25Q256_OK) {
        printf("[EMBED] Model read failed: %s\r\n", w25q256_err_str(werr));
        return false;
    }

    /* ---- Read command stream ---- */
    printf("[EMBED] Reading %lu B CMS �?SDRAM 0x%08lX...\r\n",
           (unsigned long)EMBED_CMDSTREAM_SIZE, (unsigned long)cms_sdram);
    werr = w25q256_read(EMBED_CMDSTREAM_FLASH_OFFSET, cms_sdram, EMBED_CMDSTREAM_SIZE);
    if (werr != W25Q256_OK) {
        printf("[EMBED] CMS read failed: %s\r\n", w25q256_err_str(werr));
        return false;
    }

    /* Quick sanity check: CMS must start with "COP1" Ethos-U magic */
    if (cms_sdram[0] != 0x43 || cms_sdram[1] != 0x4F
        || cms_sdram[2] != 0x50 || cms_sdram[3] != 0x31) {
        printf("[EMBED] CMS magic mismatch �?flash not programmed?\r\n");
        return false;
    }

    /*
     * D-Cache: CLEAN (not invalidate!) so NPU can see the data in SDRAM.
     * w25q256_read copies via CPU store �?D-Cache dirty lines.  The NPU
     * reads SDRAM via AXI, bypassing the cache, so we must write back.
     */
#if (BSP_CFG_DCACHE_ENABLED == 1)
    SCB_CleanDCache_by_Addr((volatile void *)model_sdram, (int32_t)EMBED_MODEL_DATA_SIZE);
    SCB_CleanDCache_by_Addr((volatile void *)cms_sdram,   (int32_t)EMBED_CMDSTREAM_SIZE);
    __DSB();
#endif

    embed_set_model_data(model_sdram);
    embed_set_cms(cms_sdram, EMBED_CMDSTREAM_SIZE);

    printf("[EMBED] Model loaded OK\r\n");

    g_embed_ready = true;
    return true;
}

bool face_embedding_run(const int8_t *input_96x96x3, float embedding_out[EMBED_DIM])
{
    if (!g_embed_ready) {
        printf("[EMBED] Not initialised\r\n");
        return false;
    }

    /*
     * Guard against NPU race: face-detection task (prio 2) and this
     * code (LVGL task prio 3 / enrol worker prio 2) share the same
     * Ethos-U55.  If the detection inference is in-flight, bail out
     * immediately �?the user can retry.
     *
     * g_npu_inferencing is set true by the detection task before
     * ethosu_invoke_v3 and cleared after.  A short busy-wait (�? ms)
     * catches the common case where detection just finished.
     */
    extern volatile bool g_npu_inferencing;
    int waited = 0;
    while (g_npu_inferencing && waited < 5) {
        vTaskDelay(1);   /* yield to face-detection task */
        waited++;
    }
    if (g_npu_inferencing) {
        printf("[EMBED] NPU busy �?retry\r\n");
        return false;
    }

    /* 1. Copy input to NPU arena (offset 36864, size 27648) */
    uint8_t *arena = (uint8_t *)EMBED_ARENA_ADDR;
    memcpy(arena + 36864, input_96x96x3, 27648);

    /* 2. Run NPU inference (D-Cache coherency handled inside invoke) */
    int ret = sub_0001_invoke(true);
    if (ret != 0) {
        printf("[EMBED] NPU invoke failed: %d, arena=0x%08lX model=0x%08lX\r\n",
               ret, (unsigned long)EMBED_ARENA_ADDR,
               (unsigned long)EMBED_MODEL_SDRAM);
        return false;
    }

    /* 3. Read output (128 INT8 values at offset 1280) */
    const int8_t *out_int8 = (const int8_t *)(arena + 1280);

    /* 4. Dequantize: float_val = (int8_val - zp) * scale */
    float sum_sq = 0.0f;
    for (int i = 0; i < EMBED_DIM; i++) {
        float val = (float)(out_int8[i] - OUT_ZP) * OUT_SCALE;
        embedding_out[i] = val;
        sum_sq += val * val;
    }

    /* 5. L2-normalise �?cosine similarity = dot product */
    if (sum_sq > 0.0f) {
        float inv_norm = 1.0f / sqrtf(sum_sq);
        for (int i = 0; i < EMBED_DIM; i++) {
            embedding_out[i] *= inv_norm;
        }
    }

    return true;
}
