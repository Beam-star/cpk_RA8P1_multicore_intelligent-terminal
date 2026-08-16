/**
 ******************************************************************************
 * @file    face_detection_task.c
 * @brief   Face detection FreeRTOS task �?v2 model (96×96 anchor-free)
 *
 * Model: MobileNetV2 width 0.5 + SPPF + lightweight detect heads
 *        96×96×1 INT8 input, 134 KB weights, 144 KB arena, 14.58M MACs
 *
 * Data flow:
 *   Camera task �?face_detection_signal_new_frame() �?this task:
 *     preprocess (RGB565 640×480→INT8 96×96) �?NPU inference �?post-process
 ******************************************************************************
 */
#include "face_detection_build_mode.h"

#if !FACE_DETECT_FLASH_PROGRAMMER
#include "face_detection_task.h"
#include "face_detection_preprocess.h"
#include "face_detection_config.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "common_data.h"
#include "mipi_camera.h"
#include "w25q256.h"

#include <stdio.h>
#include <string.h>

/* ---- Ethos-U NPU driver ---- */
#include "rm_ethosu_api.h"

/* ---- Model API (C linkage) ---- */
#include "model/model.h"
#include "model/wrapper.h"
#include "model/sub_0000_invoke.h"

/* ---- C++ inference function ---- */
#ifdef __cplusplus
extern "C" {
#endif

extern int face_detect_run_inference(void);

/* D-cache management for Ethos-U (implemented in ethosu_dcache.c) */
extern void ethosu_invalidate_dcache(uint32_t *p, size_t bytes);
extern void ethosu_clean_dcache(uint32_t *p, size_t bytes);

#ifdef __cplusplus
}
#endif

/* ======================================================================== */
/*  Global variables                                                         */
/* ======================================================================== */

/* Detection results (written here, read by camera display task) */
face_detect_result_t g_face_detection_results[AI_MAX_DETECTION_NUM] = {0};
volatile uint32_t    g_face_detection_count = 0;
volatile float       g_face_detect_fps     = 0.0f;   /* smoothed FPS for OSD */

/* Preprocessed input buffer (shared with C++ inference code) */
int8_t g_face_detect_input_buffer[AI_INPUT_IMAGE_SIZE] BSP_ALIGN_VARIABLE(8);

/* Model data loaded from W25Q256 to SDRAM (face + hand) */
static uint8_t *g_face_model_data_sdram   = NULL;
static uint8_t *g_face_command_stream_sdram = NULL;
static uint8_t *g_hand_model_data_sdram   = NULL;
static uint8_t *g_hand_command_stream_sdram = NULL;

/* Active detection mode (switched from LVGL UI).  Read each inference frame
 * and each draw_face_boxes() call.  A one-frame overlap on switch is harmless
 * (results are simply drawn with the new mode's mapping for one frame). */
static volatile detection_mode_t g_detection_mode = DETECTION_MODE_FACE;

/* Frame synchronization */
static SemaphoreHandle_t g_frame_ready_sem = NULL;
static volatile uint32_t g_latest_frame_addr = 0;

/* NPU inference in progress flag (defined in model/sub_0000_invoke.c).
 * Camera/LCD task checks this before writing the framebuffer. */
extern volatile bool g_npu_inferencing;

/* Inference interval (process 1 frame every N).  Default 2 for a smooth live
 * preview (~24 fps).  Raised to 3 during recording / video playback so the NPU
 * issues fewer SDRAM accesses and leaves bandwidth for MJPEG encode/decode
 * (empirically: N=2 → ~24 fps preview but slow codec; N=3 → ~12 fps preview
 * but much faster record/playback).
 *
 * volatile: written from the RPMsg/UI context (recording & playback state
 * transitions), read by this task every frame.  Single-word 32-bit load is
 * atomic on M85, so no lock is needed — the new value simply applies on the
 * next frame. */
static volatile uint32_t g_infer_every_n_frames = INFER_EVERY_N_PREVIEW;

/* ======================================================================== */
/*  Model data override                                                      */
/*  The MERA-generated sub_0000_invoke.c uses sub_0000_model_data[]         */
/*  from sub_0000_model_data.c. We need to redirect it to SDRAM.            */
/*  We do this by modifying sub_0000_invoke.c to use a pointer instead.     */
/* ======================================================================== */

const uint8_t *face_detect_get_model_data(void)
{
    return (g_detection_mode == DETECTION_MODE_HAND)
           ? g_hand_model_data_sdram : g_face_model_data_sdram;
}

size_t face_detect_get_model_data_size(void)
{
    return (g_detection_mode == DETECTION_MODE_HAND)
           ? (size_t)HAND_MODEL_DATA_SIZE : (size_t)FACE_MODEL_DATA_SIZE;
}

const uint8_t *face_detect_get_command_stream(void)
{
    return (g_detection_mode == DETECTION_MODE_HAND)
           ? g_hand_command_stream_sdram : g_face_command_stream_sdram;
}

size_t face_detect_get_command_stream_size(void)
{
    return (g_detection_mode == DETECTION_MODE_HAND)
           ? (size_t)HAND_MODEL_COMMAND_STREAM_SIZE
           : (size_t)FACE_MODEL_COMMAND_STREAM_SIZE;
}

detection_mode_t face_detection_get_mode(void)
{
    return (detection_mode_t)g_detection_mode;
}

void face_detection_set_mode(detection_mode_t mode)
{
    if (mode != DETECTION_MODE_FACE && mode != DETECTION_MODE_HAND) return;
    g_detection_mode = mode;
    printf("[DET] Mode -> %s\r\n",
           mode == DETECTION_MODE_HAND ? "HAND" : "FACE");
}

void face_detection_set_infer_interval(uint32_t n)
{
    if (n < 1) n = 1;
    g_infer_every_n_frames = n;
    printf("[DET] Infer interval -> every %lu frame(s)\r\n", (unsigned long)n);
}

/* ======================================================================== */
/*  Detection result access (called from C++ post-processing)                */
/* ======================================================================== */

void face_detect_update_result(uint16_t index, int16_t x, int16_t y, int16_t w, int16_t h, int16_t id)
{
    if (index < AI_MAX_DETECTION_NUM) {
        g_face_detection_results[index].x = x;
        g_face_detection_results[index].y = y;
        g_face_detection_results[index].w = w;
        g_face_detection_results[index].h = h;
        g_face_detection_results[index].id = id;
    }
}

/* ======================================================================== */
/*  Public API                                                               */
/* ======================================================================== */

void face_detection_signal_new_frame(uint32_t frame_addr)
{
    g_latest_frame_addr = frame_addr;
    if (g_frame_ready_sem) {
        xSemaphoreGive(g_frame_ready_sem);
    }
}

/* ======================================================================== */
/*  Model loading from W25Q256 to SDRAM                                      */
/* ======================================================================== */

/* Load one model's weights + command stream from W25Q256 into SDRAM,
 * then D-Cache clean so the NPU can see them via AXI.  Assumes the OSPI_B
 * controller is already open. */
static int load_one_model(uint32_t flash_off, uint32_t cms_flash_off,
                          uint8_t *sdram_addr, uint32_t data_size,
                          uint32_t cms_size, const char *name)
{
    uint8_t *cms_sdram = sdram_addr + data_size;

    w25q256_err_t werr = w25q256_read(flash_off, sdram_addr, data_size);
    if (werr != W25Q256_OK) {
        printf("[DET] %s weights read failed: %s\r\n", name, w25q256_err_str(werr));
        return -1;
    }

    werr = w25q256_read(cms_flash_off, cms_sdram, cms_size);
    if (werr != W25Q256_OK) {
        printf("[DET] %s command stream read failed: %s\r\n", name, w25q256_err_str(werr));
        return -1;
    }

    /* Command stream must begin with the "COP1" Ethos-U magic. */
    if (cms_sdram[0] != 0x43 || cms_sdram[1] != 0x4F
        || cms_sdram[2] != 0x50 || cms_sdram[3] != 0x31) {
        printf("[DET] %s command stream magic mismatch — flash not programmed?\r\n", name);
        return -1;
    }

#if (BSP_CFG_DCACHE_ENABLED == 1)
    SCB_CleanDCache_by_Addr((volatile void *)sdram_addr, (int32_t)data_size);
    SCB_CleanDCache_by_Addr((volatile void *)cms_sdram,  (int32_t)cms_size);
    __DSB();
#endif

    printf("[DET] %s loaded: weights %lu B @0x%08lX, cms %lu B @0x%08lX\r\n",
           name, (unsigned long)data_size, (unsigned long)sdram_addr,
           (unsigned long)cms_size, (unsigned long)cms_sdram);
    return 0;
}

static int load_models_from_flash(void)
{
    int ret = -1;

    printf("[DET] Loading face + hand models from W25Q256 to SDRAM...\r\n");

    w25q256_err_t werr = w25q256_open();
    if (werr != W25Q256_OK) {
        printf("[DET] ERROR: W25Q256 open failed: %s\r\n", w25q256_err_str(werr));
        return -1;
    }

    w25q256_jedec_id_t id;
    if (w25q256_read_jedec_id(&id) == W25Q256_OK) {
        printf("[DET] W25Q256 ID: %02X %02X %02X\r\n",
               id.manufacturer, id.memory_type, id.capacity);
    }

    /* Face model → 0x68540000; command stream follows weights in SDRAM. */
    g_face_model_data_sdram   = (uint8_t *)FACE_MODEL_SDRAM_ADDR;
    g_face_command_stream_sdram = g_face_model_data_sdram + FACE_MODEL_DATA_SIZE;
    if (load_one_model(FACE_MODEL_FLASH_OFFSET, FACE_MODEL_CMDSTREAM_FLASH_OFFSET,
                       g_face_model_data_sdram, FACE_MODEL_DATA_SIZE,
                       FACE_MODEL_COMMAND_STREAM_SIZE, "FACE") != 0) {
        goto cleanup;
    }

    /* Hand model → 0x68600000 (freed face-embedding region). */
    g_hand_model_data_sdram   = (uint8_t *)HAND_MODEL_SDRAM_ADDR;
    g_hand_command_stream_sdram = g_hand_model_data_sdram + HAND_MODEL_DATA_SIZE;
    if (load_one_model(HAND_MODEL_FLASH_OFFSET, HAND_MODEL_CMDSTREAM_FLASH_OFFSET,
                       g_hand_model_data_sdram, HAND_MODEL_DATA_SIZE,
                       HAND_MODEL_COMMAND_STREAM_SIZE, "HAND") != 0) {
        goto cleanup;
    }

    ret = 0;

cleanup:
    /*
     * Close OSPI_B to release the hardware.  Model data and command streams
     * are now in SDRAM; the NPU accesses them directly via AXI — the OSPI_B
     * peripheral is no longer needed until the next firmware update.
     */
    w25q256_close();
    return ret;
}

/* ======================================================================== */
/*  Main FreeRTOS task                                                       */
/* ======================================================================== */

static void face_detection_task_entry(void *pvParameters)
{
    (void)pvParameters;

    printf("[FACE_DET] Task started\r\n");

    /* 1. Load both models (face + hand) from W25Q256 to SDRAM */
    int ret = load_models_from_flash();
    if (ret != 0) {
        printf("[DET] FATAL: Model load failed\r\n");
        vTaskDelete(NULL);
        return;
    }

    /* 2. Open Ethos-U NPU driver */
    fsp_err_t err = RM_ETHOSU_Open(&g_rm_ethosu0_ctrl, &g_rm_ethosu0_cfg);
    if (err != FSP_SUCCESS) {
        printf("[FACE_DET] FATAL: Ethos-U open failed: %d\r\n", (int)err);
        vTaskDelete(NULL);
        return;
    }
    printf("[FACE_DET] Ethos-U NPU opened\r\n");

    /*
     * Keep NPU powered to avoid per-inference power-gating overhead.
     * The driver's ethosu_release_power() is called after each inference;
     * keeping request_counter >= 1 prevents unnecessary power cycling. */
    {
        int pw_ret = ethosu_request_power(&g_ethosu0);
        printf("[FACE_DET] NPU keep-powered: %s\r\n",
               pw_ret == 0 ? "OK" : "FAIL");
    }

    /*
     * ══════════════════════════════════════════════════════════════�?     * NPU AXI limiter configuration �?PRIMARY fix for screen flicker.
     * ══════════════════════════════════════════════════════════════�?     *
     * SDRAM bandwidth budget (W9825G6KH-6, ~200 MB/s realistic):
     *   NPU inference:  ~216 MB/s (422 KB weights + 442 KB arena / 4 ms)
     *   GLCDC scanout:   ~60 MB/s (1024×600 RGB565 @ ~60 Hz)
     *   VIN camera DMA:  ~18 MB/s (640×480 RGB565 @ 30 fps)
     *   Total:          ~294 MB/s �?1.5× the physical limit
     *
     * Root cause: the 4 NPU AXI limiters were configured independently,
     * allowing the NPU to issue up to 4 × 2 = 8 concurrent read bursts.
     * This let the NPU monopolise the SDRAM bus, starving GLCDC reads
     * �?pixel FIFO underrun �?visible screen flicker.
     *
     * Fix: �?memtype = Normal Bufferable (required for SDRAM)
     *      �?max_outstanding reads/writes = 1 per limiter (minimum)
     *      �?REGIONCFG: all 6 address regions �?limiter 0
     *
     * With all regions serialised through ONE limiter, the NPU can only
     * issue 1 read OR 1 write at a time.  Peak NPU SDRAM bandwidth drops
     * from ~216 MB/s to ~108 MB/s.  Inference time increases from ~4 ms
     * to ~8 ms, but SDRAM total demand drops to ~186 MB/s �?within the
     * ~200 MB/s physical budget.  No more GLCDC FIFO underrun. */
    {
        volatile uint32_t *npu = (volatile uint32_t *)R_NPU_BASE;
        static const uint32_t lim[] = { 0x0040, 0x0044, 0x0048, 0x004C };
        for (int i = 0; i < 4; i++) {
            uint32_t v = npu[lim[i] / 4];
            v &= ~((0xFu << 3) | (0x3FFu << 7) | (0x3FFu << 17) | 0x7u);
            v |=  (3u    << 3);   /* memtype = Normal Bufferable   */
            v |=  (0u    << 7);   /* max_outstanding_reads  = 1   */
            v |=  (0u    << 17);  /* max_outstanding_writes = 1   */
            npu[lim[i] / 4] = v;
        }
        /* REGIONCFG: all 6 address regions �?limiter 0 */
        npu[0x003C / 4] = (0u << 0) | (0u << 2) | (0u << 4)
                        | (0u << 6) | (0u << 8) | (0u << 10);
    }
    printf("[FACE_DET] Model ptr: 0x%08lX, Arena ptr: 0x%08lX\r\n",
           (unsigned long)face_detect_get_model_data(),
           (unsigned long)sub_0000_arena);

    /* 3. Signal initialization complete */
    /* (Could use EventGroup to sync with other tasks if needed) */

    printf("[FACE_DET] Ready for inference\r\n");

    /* 4. Main inference loop.
     *
     * v2 model MACs: 14.58M (vs v1's 38.92M) �?~3× faster.
     * With AXI limiter: inference �?2�? ms (vs v1's ~8 ms). */
    uint32_t infer_count   = 0;
    uint32_t fps_infers    = 0;
    TickType_t fps_window_start = xTaskGetTickCount();
    /* v3 @ 30M MACs: NPU ~4 ms.  Every-frame inference causes
     * g_npu_inferencing to overlap VSYNC �?camera skips fb write �?black.
     * Infer every 2nd frame to leave every other VSYNC free. */
    while (1) {
        /* Wait for a new camera frame */
        if (xSemaphoreTake(g_frame_ready_sem, pdMS_TO_TICKS(500)) == pdTRUE) {
            infer_count++;
            if (infer_count % g_infer_every_n_frames != 0) continue;  /* Skip frames */
            uint32_t frame_addr = g_latest_frame_addr;
            if (frame_addr == 0) continue;

            /* 4a. D-Cache invalidation for the camera frame buffer is NOT
             * needed here �?the camera/LCD task already invalidates the
             * VIN buffer on every frame (mipi_camera_lcd.c line 291).
             * Doing it again evicts the camera task's working set from
             * cache, causing cold-cache reads �?vsync miss �?screen flicker.
             *
             * Face detection reads the buffer AFTER the camera task
             * invalidated it (at vsync wait time), so the data is fresh
             * from SDRAM and fills cache naturally.  This cache fill
             * actually HELPS the subsequent camera display step (cache hit
             * when the camera task rotates the frame to the framebuffer). */

            /* Signal camera/LCD task: face detection is about to access
             * SDRAM.  Set BEFORE preprocessing (which reads the VIN buffer
             * from SDRAM) so the camera task skips framebuffer writes
             * during the ENTIRE SDRAM access window, not just NPU invoke. */
            g_npu_inferencing = true;
            __DSB();

            /* 4b. Preprocess: RGB565 → INT8 grayscale (mode-dependent geometry) */
            if (face_detection_get_mode() == DETECTION_MODE_HAND) {
                hand_detect_preprocess_rgb565_to_int8(
                    (const void *)frame_addr,
                    g_face_detect_input_buffer,
                    CAMERA_INPUT_WIDTH, CAMERA_INPUT_HEIGHT,
                    AI_INPUT_IMAGE_WIDTH, AI_INPUT_IMAGE_HEIGHT);
            } else {
                face_detect_preprocess_rgb565_to_int8(
                    (const void *)frame_addr,
                    g_face_detect_input_buffer,
                    CAMERA_INPUT_WIDTH, CAMERA_INPUT_HEIGHT,
                    AI_INPUT_IMAGE_WIDTH, AI_INPUT_IMAGE_HEIGHT);
            }

            /* 4c. Run inference + post-processing (C++ function) */
            int ret = face_detect_run_inference();

            /* FPS tracking: 500ms sliding window, light EMA (α=0.5) */
            {
                fps_infers++;
                TickType_t now = xTaskGetTickCount();
                TickType_t elapsed = now - fps_window_start;
                if (elapsed >= pdMS_TO_TICKS(500)) {
                    float instant = (float)fps_infers * 1000.0f / (float)elapsed;
                    if (g_face_detect_fps <= 0.0f)
                        g_face_detect_fps = instant;
                    else
                        g_face_detect_fps = g_face_detect_fps * 0.5f + instant * 0.5f;
                    fps_infers = 0;
                    fps_window_start = now;
                }
            }

            /* Face detection SDRAM access complete */
            g_npu_inferencing = false;
            __DSB();

            /* 4c. Update detection count */
            uint32_t count = 0;
            for (uint32_t i = 0; i < AI_MAX_DETECTION_NUM; i++) {
                if (g_face_detection_results[i].w > 0 && g_face_detection_results[i].h > 0) {
                    count++;
                }
            }
            g_face_detection_count = count;

            /* Log every 30 frames */
            infer_count++;
            if (infer_count % 100 == 1) {
                printf("[FACE_DET] frame#%lu ret=%d count=%lu", (unsigned long)infer_count, ret, (unsigned long)count);
                if (count > 0) {
                    printf(" [%d,%d %dx%d]",
                        g_face_detection_results[0].x, g_face_detection_results[0].y,
                        g_face_detection_results[0].w, g_face_detection_results[0].h);
                }
                printf("\r\n");
            }
        }
    }
}

/* ======================================================================== */
/*  Task creation                                                            */
/* ======================================================================== */

void face_detection_task_start(void)
{
    /* Diagnostic: show available heap before allocation */
    printf("[FACE_DET] Free heap before alloc: %lu bytes\r\n",
           (unsigned long)xPortGetFreeHeapSize());

    /* Create frame-ready semaphore */
    g_frame_ready_sem = xSemaphoreCreateBinary();
    if (g_frame_ready_sem == NULL) {
        printf("[FACE_DET] ERROR: Failed to create semaphore\r\n");
        return;
    }

    printf("[FACE_DET] Free heap after semaphore: %lu bytes, creating task (stack=%lu)...\r\n",
           (unsigned long)xPortGetFreeHeapSize(),
           (unsigned long)FACE_DETECT_TASK_STACK_SIZE);

    /* Create the task */
    BaseType_t ret = xTaskCreate(
        face_detection_task_entry,
        "face_det",
        FACE_DETECT_TASK_STACK_SIZE,
        NULL,
        FACE_DETECT_TASK_PRIORITY,
        NULL);

    if (ret != pdPASS) {
        printf("[FACE_DET] ERROR: Failed to create task (err=%ld, free_heap=%lu)\r\n",
               (long)ret, (unsigned long)xPortGetFreeHeapSize());
    } else {
        printf("[FACE_DET] Task created OK\r\n");
    }
}

#endif /* !FACE_DETECT_FLASH_PROGRAMMER */
