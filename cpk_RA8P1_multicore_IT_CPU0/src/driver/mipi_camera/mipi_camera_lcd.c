/**
 ******************************************************************************
 * @file    mipi_camera_lcd.c
 * @brief   Unified camera capture + LCD display task (CPU0) — double-buffer
 *
 * Data flow (single task, double-buffer, vsync-synchronized):
 *   IMX415 (RAW10) → MIPI CSI → VIN → SDRAM
 *   → wait VIN frame → D-Cache invalidate → signal face detection
 *   → wait vsync → copy camera fb → D-Cache clean
 *   → R_GLCDC_BufferChange (atomic swap at next vsync)
 *
 * Double-buffer design:
 *   Writes to the OFF-SCREEN buffer (fb[write_idx]) while GLCDC scans out
 *   the ON-SCREEN buffer (fb[1 - write_idx]).  R_GLCDC_BufferChange atomically
 *   swaps them at the next vsync — no tearing, no flash.
 *
 *   The vsync wait before writing ensures GLCDC has already switched away
 *   from the target buffer.  D-Cache clean on the off-screen buffer does not
 *   contend with GLCDC reads (different SDRAM address range).
 *
 * SDRAM bandwidth:
 *   GLCDC pixel clock divider configured in e2studio FSP configurator
 *   to keep total SDRAM traffic (GLCDC + VIN DMA + CPU) within budget.
 *
 * Display layout:
 *   Camera (480×640) → Rotate 90° CW → (640×480)
 *   LCD: 1024×600, left-aligned (x_off=0), bottom-aligned (y_off=120):
 *   top 640×120 strip holds a static logo ("toplogo" asset).
 ******************************************************************************
 */

#include "mipi_camera_lcd.h"
#include "mipi_camera.h"
#include "rgblcd.h"
#include "common_data.h"
#include "rm_lvgl_port.h"
#include "rpmsg_video.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>
#include "driver/w25q256/w25q256.h"
#include "driver/w25q256/w25q256_partition.h"

/* Face detection integration */
#include "ai_application/face_detection_build_mode.h"
#if !FACE_DETECT_FLASH_PROGRAMMER
#include "ai_application/face_detection_task.h"
/* NPU inference flag — set true during preprocessing + NPU inference + post-processing.
 * Defence-in-depth: camera task skips framebuffer write while this is true.
 * The PRIMARY fix for screen flicker is the NPU AXI limiter in face_detection_task.c. */
extern volatile bool g_npu_inferencing;
#endif

/* GLCDC vsync frame counter (defined in rgblcd.c, incremented by DisplayVsyncCallback) */
extern volatile uint32_t g_frame_count;

/* D-Cache operations */
extern void SCB_InvalidateDCache_by_Addr(volatile void *addr, int32_t dsize);
extern void SCB_CleanDCache_by_Addr(volatile void *addr, int32_t dsize);

/* Camera frame parameters */
#define CAM_W                   640
#define CAM_H                   480
#define CAM_BPP                 2
//#define CAM_FRAME_SIZE          (CAM_W * CAM_H * CAM_BPP)

/* Task configuration */
#define CAMLCD_TASK_STACK_SIZE  4096
#define CAMLCD_TASK_PRIORITY    4

/* LCD offsets: camera (640×480) on the 1024×600 display.
 *
 * In dual-layer mode (GLCDC_CFG_LAYER_2_ENABLE), the right side (640..1024,
 * 384 px wide) is occupied by the LVGL UI panel on GLCDC layer 2.  Camera
 * frames go to layer 1 at x=0, leaving the layer-2 panel opaque and visible.
 *
 * The camera is BOTTOM-aligned (LCD_Y_OFF=120), leaving a 640×120 strip at the
 * top of layer 1 for a static logo ("toplogo" asset, drawn once at boot).
 *
 * In legacy (single-layer) mode, set LCD_X_OFF=192 to keep the old centred
 * layout — the camera task straddles the centre and LVGL can't render because
 * RM_LVGL_PORT_Open() is never called.
 */
#if GLCDC_CFG_LAYER_2_ENABLE
  #define LCD_X_OFF   0
#else
  #define LCD_X_OFF   ((RGBLCD_WIDTH  - CAM_W) / 2)   /* 192 (centred) */
#endif
#define LCD_Y_OFF   (RGBLCD_HEIGHT - CAM_H)          /* 120 — bottom-aligned */

/* ---- Top-strip logo (layer-1 area above the camera) ---- */
#define TOP_LOGO_W        640
#define TOP_LOGO_H        120
#define TOP_LOGO_BYTES    (TOP_LOGO_W * TOP_LOGO_H * 2)   /* 153,600 */

/* Blit the static "toplogo" asset (640×120 RGB565, W25Q256 asset directory)
 * into the top strip of a layer-1 framebuffer. Called once per framebuffer at
 * boot — the camera task never writes above LCD_Y_OFF, so the logo persists. */
static void draw_top_logo(uint16_t *fb)
{
    static flash_asset_dir_t dir;   /* ~4 KB — must NOT be on the stack */

    memcpy(&dir, (const void *)(W25Q256_MEM_BASE + PART_ASSET_DIR_OFFSET),
           sizeof(dir));
    if (dir.magic != FLASH_ASSET_MAGIC || dir.count == 0
        || dir.count > FLASH_ASSET_MAX_ENTRIES) {
        return;
    }

    for (uint32_t i = 0; i < dir.count; i++) {
        flash_asset_entry_t *e = &dir.entries[i];
        if (e->name[0] == '\0' || e->name[0] == (char)0xFF) continue;
        if (strncmp(e->name, "toplogo", sizeof(e->name)) != 0) continue;
        if (e->size != TOP_LOGO_BYTES) {
            printf("[CAM LCD] toplogo size mismatch (%lu, expect %lu)\r\n",
                   (unsigned long)e->size, (unsigned long)TOP_LOGO_BYTES);
            return;
        }

        const uint16_t *src = (const uint16_t *)(W25Q256_MEM_BASE + e->offset);
        for (int y = 0; y < TOP_LOGO_H; y++) {
            memcpy(&fb[y * RGBLCD_WIDTH], &src[y * TOP_LOGO_W], TOP_LOGO_W * 2);
        }
        printf("[CAM LCD] toplogo shown @0x%06lX\r\n", (unsigned long)e->offset);
        return;
    }
    /* Not found: leave the strip black (framebuffer already cleared). */
}

/* Forward declarations */
static void mipi_camera_lcd_task(void *pvParameters);

/* 视频录制帧抓取开关 (由 rpmsg_record_cpu0.c 在 REC 开始/停止时设置) */
static volatile bool g_video_record_enabled = false;

/* 视频回放显示开关 (由 video_play_display 在播放开始/结束时设置) */
static volatile bool g_playback_active = false;

void mipi_camera_lcd_set_video_record(bool enabled)
{
    g_video_record_enabled = enabled;
}

void mipi_camera_lcd_set_playback(bool active)
{
    g_playback_active = active;
}

/* ======================================================================== */
/*  Bounding-box drawing helpers (CPU rendering — D/AVE 2D not available)   */
/* ======================================================================== */

/**
 * @brief  Draw a single-pixel-wide rectangle outline in RGB565.
 *
 * Clips to the framebuffer dimensions.
 */
static void draw_rect_rgb565(uint16_t *fb, int x, int y, int w, int h, uint16_t color)
{
    /* Clip */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)RGBLCD_WIDTH)  w = (int)RGBLCD_WIDTH  - x;
    if (y + h > (int)RGBLCD_HEIGHT) h = (int)RGBLCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    /* Top edge */
    for (int i = 0; i < w; i++) fb[y * RGBLCD_WIDTH + x + i] = color;
    /* Bottom edge */
    for (int i = 0; i < w; i++) fb[(y + h - 1) * RGBLCD_WIDTH + x + i] = color;
    /* Left edge */
    for (int i = 0; i < h; i++) fb[(y + i) * RGBLCD_WIDTH + x] = color;
    /* Right edge */
    for (int i = 0; i < h; i++) fb[(y + i) * RGBLCD_WIDTH + x + w - 1] = color;
}

/**
 * @brief  Draw all detection bounding boxes + labels on the framebuffer.
 *
 * Detection results are already in CAMERA coordinate space (640×480) — the
 * post-processor maps AI (96×96) back to camera pixels using the active
 * model's geometry (centre-crop for face, letterbox for hand).  This function
 * only applies the camera → LCD offset:
 *
 *   Camera 640×480 → LCD 1024×600  (LCD_X_OFF=0, LCD_Y_OFF=60, no flip —
 *   the sensor already does 180° in hardware)
 *
 * Labels cycle in face mode: person1, person2, … (or hand1, hand2, … in
 * hand mode), with box colours cycling red → green → blue.
 *
 * @param fb  Pointer to the framebuffer (off-screen, not being scanned out).
 */
static void draw_face_boxes(uint16_t *fb)
{
#if !FACE_DETECT_FLASH_PROGRAMMER
    uint32_t count = g_face_detection_count;
    if (count == 0) return;
    if (count > AI_MAX_DETECTION_NUM) count = AI_MAX_DETECTION_NUM;

    static const uint16_t box_colors[] = {
        RGBLCD_COLOR_RED,    /* person1 / hand1 */
        RGBLCD_COLOR_GREEN,  /* person2 / hand2 */
        RGBLCD_COLOR_BLUE,   /* person3 / hand3 */
    };

    detection_mode_t mode = face_detection_get_mode();

    for (uint32_t i = 0; i < count; i++) {
        face_detect_result_t *r = &g_face_detection_results[i];
        if (r->w <= 0 || r->h <= 0) continue;

        /* Camera → LCD (centering offset, no flip since sensor does 180° in hardware) */
        int lcd_x = LCD_X_OFF + r->x;
        int lcd_y = LCD_Y_OFF + r->y;

        uint16_t color = box_colors[i % 3];
        draw_rect_rgb565(fb, lcd_x, lcd_y, r->w, r->h, color);

        /* Label above the box.  Face mode uses the tracker's stable id
         * ("person1", "person2", …); hand mode is always "hand". */
        char label[12];
        if (mode == DETECTION_MODE_HAND) {
            snprintf(label, sizeof(label), "hand");
        } else {
            snprintf(label, sizeof(label), "person%d", (int)(r->id + 1));
        }
        int label_y = lcd_y - 16;
        if (label_y < LCD_Y_OFF) label_y = LCD_Y_OFF;
        rgblcd_draw_string((uint16_t)lcd_x, (uint16_t)label_y, label,
                           color, 0, RGBLCD_FONT_16);
    }
#endif
}

/* ======================================================================== */
/*  视频录制帧抓取 — 把带框画面复制到共享 SDRAM 双缓冲, 供 CPU1 编码            */
/* ======================================================================== */

static void video_capture_frame(const uint16_t *fb)
{
    static uint32_t vid_idx = 0;
    video_shmem_t *sh = video_shmem();

    uint32_t idx = vid_idx ^ 1;   /* 切换双缓冲 */
    uint16_t *dst = (uint16_t *)((idx == 0) ? VIDEO_FRAME0_ADDR : VIDEO_FRAME1_ADDR);

    /* 从显示 framebuffer 的相机区域复制 640×480 (带人脸框) */
    for (int y = 0; y < CAM_H; y++) {
        const uint16_t *src = &fb[(LCD_Y_OFF + y) * RGBLCD_WIDTH + LCD_X_OFF];
        memcpy(&dst[y * CAM_W], src, CAM_W * 2);
    }

    /* 视频缓冲在非缓存 SDRAM 区 (MPU region 0x68000000-0x687FFFFF),
     * 无需 CleanDCache。仅用 __DSB 屏障保证帧数据 + write_idx 在
     * frame_id 递增前对 CPU1 可见。 */
    sh->write_idx = idx;
    __DSB();
    sh->frame_id  = sh->frame_id + 1;
    __DSB();

    vid_idx = idx;
}

/* ======================================================================== */
/*  LVGL Port VPOS callback — keeps camera vsync-poll alive                  */
/* ======================================================================== */

/**
 * @brief  User callback registered with RM_LVGL_PORT_Open().
 *
 * When RM_LVGL_PORT owns the GLCDC display (dual-layer mode, inheriting
 * layer 2), it sets itself as the primary display callback and feeds its
 * internal semaphore for LVGL sync.  This user callback is invoked on every
 * VPOS (line-detect) event, incrementing g_frame_count so the camera task
 * can continue polling it for its own vsync-wait.
 *
 * Must be called from ISR context (GLCDC line-detect ISR).
 */
void lvgl_port_vpos_cb(rm_lvgl_port_callback_args_t *p_args)
{
    (void)p_args;
    g_frame_count++;
}

/* ======================================================================== */

/**
 * @brief Start the unified camera capture + LCD display task
 *
 * @param use_test_pattern      true = OV5640 color bars, false = normal camera
 * @param enable_face_detection true = run face detection AI pipeline
 */
void mipi_camera_lcd_start(bool use_test_pattern, bool enable_face_detection)
{
    uint32_t param = ((uint32_t)use_test_pattern & 1)
                   | (((uint32_t)enable_face_detection & 1) << 1);

    BaseType_t ret;
    ret = xTaskCreate(mipi_camera_lcd_task,
                      "cam_lcd",
                      CAMLCD_TASK_STACK_SIZE,
                      (void *)param,
                      CAMLCD_TASK_PRIORITY,
                      NULL);
    if (ret != pdPASS) {
        printf("[CAM LCD] Failed to create unified task\r\n");
    }
}

/**
 * @brief Unified camera capture + LCD display task — double-buffer.
 *
 * Flow per frame:
 *   1. Wait VIN DMA complete (semaphore from ISR)
 *   2. Invalidate D-Cache for VIN buffer
 *   3. Signal face detection (async, if enabled)
 *   4. Wait for vsync (GLCDC has switched to the other buffer)
 *   5. Rotate 90° CW + write to OFF-SCREEN framebuffer
 *   6. D-Cache clean camera area (off-screen buffer, no GLCDC contention)
 *   7. R_GLCDC_BufferChange (atomic swap at next vsync)
 *
 * @param pvParameters  Packed: bit0=use_test_pattern, bit1=enable_face_detection
 */
static void mipi_camera_lcd_task(void *pvParameters)
{
    uint32_t param = (uint32_t)pvParameters;
    bool use_test_pattern    = (bool)(param & 1);
    bool enable_face_detection = (bool)((param >> 1) & 1);

    uint32_t frame_count = 0;
    uint32_t err_count   = 0;
    uint32_t vsync_snapshot;

    printf("[CAM LCD] === Task started (prio=%lu) ===\r\n",
           (unsigned long)uxTaskPriorityGet(NULL));

    /*
     * Double-buffer ping-pong.
     * Start with write_idx = 1: GLCDC is configured to read fb_background[0]
     * (see common_data.c).  Writing to fb[1] first avoids the active scan-out
     * buffer.  After the first BufferChange(fb[1]), GLCDC switches to fb[1]
     * at the next vsync, and we write to fb[0] — and so on.
     */
    uint8_t write_idx = 1;

    /* ---- Phase 1: Clear BOTH framebuffers to black ---- */
    printf("[CAM LCD] Phase 1: clearing framebuffers...\r\n");
    uint16_t *fb0 = (uint16_t *)fb_background[0];
    uint16_t *fb1 = (uint16_t *)fb_background[1];
    for (uint32_t i = 0; i < (uint32_t)RGBLCD_WIDTH * RGBLCD_HEIGHT; i++) {
        fb0[i] = 0x0000;
        fb1[i] = 0x0000;
    }
    SCB_CleanDCache_by_Addr((volatile void *)fb0,
                            RGBLCD_WIDTH * RGBLCD_HEIGHT * 2);
    SCB_CleanDCache_by_Addr((volatile void *)fb1,
                            RGBLCD_WIDTH * RGBLCD_HEIGHT * 2);

    /* ---- Phase 1.5: Draw persistent top-strip logo (above the camera) ---- */
    draw_top_logo(fb0);
    draw_top_logo(fb1);
    SCB_CleanDCache_by_Addr((volatile void *)fb0, TOP_LOGO_H * RGBLCD_STRIDE_BYTES);
    SCB_CleanDCache_by_Addr((volatile void *)fb1, TOP_LOGO_H * RGBLCD_STRIDE_BYTES);
    printf("[CAM LCD] Phase 1: done\r\n");

    /* ---- Phase 2: Wait for first vsync (ensure GLCDC is running) ---- */
    printf("[CAM LCD] Phase 2: waiting for vsync (g_frame_count=%lu)...\r\n",
           (unsigned long)g_frame_count);
    vsync_snapshot = g_frame_count;
    {
        uint32_t spin_ms = 0;
        while (g_frame_count == vsync_snapshot) {
            vTaskDelay(1);
            spin_ms++;
            if (spin_ms > 2000) {   /* >2 s = stuck — something is wrong */
                printf("[CAM LCD] *** STUCK at vsync wait! g_frame_count=%lu ***\r\n",
                       (unsigned long)g_frame_count);
                spin_ms = 0; /* keep printing every 2s */
            }
        }
    }
    printf("[CAM LCD] Phase 2: vsync OK (g_frame_count=%lu, waited ~%lums)\r\n",
           (unsigned long)g_frame_count, 0UL);

    /* ---- Phase 3: Initialize camera hardware ---- */
    printf("[CAM LCD] Phase 3: camera init (test_pattern=%d)...\r\n",
           use_test_pattern);
    fsp_err_t err = mipi_camera_init(use_test_pattern);
    if (err != FSP_SUCCESS) {
        printf("[CAM LCD] Camera init failed: %d\r\n", (int)err);
        vTaskDelete(NULL);
        return;
    }

    /* Print VIN buffer addresses for diagnostics */
    printf("[CAM LCD] VIN buf1=0x%08lX buf2=0x%08lX buf3=0x%08lX\r\n",
           (unsigned long)vin_image_buffer_1,
           (unsigned long)vin_image_buffer_2,
           (unsigned long)vin_image_buffer_3);
    printf("[CAM LCD] FB    buf0=0x%08lX buf1=0x%08lX\r\n",
           (unsigned long)fb_background[0],
           (unsigned long)fb_background[1]);

    /* ---- Phase 4: Start face detection task (optional) ---- */
#if !FACE_DETECT_FLASH_PROGRAMMER
    if (enable_face_detection) {
        printf("[CAM LCD] Phase 4a: creating face detection task...\r\n");
        face_detection_task_start();
        printf("[CAM LCD] Phase 4a: face detection task started\r\n");
    }
#endif

    /* ---- Phase 5: Start VIN continuous capture ---- */
    printf("[CAM LCD] Phase 5: starting VIN capture...\r\n");
    mipi_camera_capture_start();
    printf("[CAM LCD] Phase 5: VIN capture started, entering loop\r\n");

    /* ---- Phase 6: Main capture + display loop ----
     *
     * DOUBLE-BUFFER design with R_GLCDC_BufferChange:
     *
     * Two framebuffers: fb_background[0] (initially on-screen) and
     * fb_background[1] (off-screen).  The task writes new camera frames
     * to the OFF-SCREEN buffer while GLCDC scans out the ON-SCREEN one.
     * After the write + D-Cache clean, R_GLCDC_BufferChange atomically
     * swaps them at the next vsync.
     *
     * This avoids the single-buffer "brightness-flash" artefact caused
     * by AEC-adjusted frames being partially visible during scan-out.
     * D-Cache clean on the off-screen buffer also does not contend with
     * GLCDC reads on the on-screen buffer (different SDRAM address range).
     *
     * Diagnostics (printed every 100 frames):
     *   skip_npu  — frames skipped because NPU was busy
     *   skip_vsync — vsync-waits that took > 2 ticks (late)
     */
    uint32_t diag_skip_npu   = 0;
    uint32_t diag_skip_vsync = 0;
    while (1) {
        /* 视频回放期间暂停写 layer1: 由 video_play_display 显示任务接管 */
        if (g_playback_active) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* 6a. Wait for VIN DMA to complete a frame */
        if (!mipi_camera_wait_frame(500)) {
            err_count++;
            if (err_count <= 3) {
                printf("[CAM LCD] Frame timeout #%lu\r\n", (unsigned long)err_count);
            }
            continue;
        }

        uint8_t *vin_frame = mipi_camera_get_frame();

        /*
         * 6b. D-Cache Invalidate for VIN buffer.
         * VIN DMA writes directly to SDRAM, bypassing CPU D-Cache.
         * Must invalidate before CPU reads to avoid stale cached data.
         */
        SCB_InvalidateDCache_by_Addr((volatile void *)vin_frame, CAM_FRAME_SIZE);
        __DSB();

        /* Diagnostic: first 3 frames */
        if (frame_count < 3) {
            printf("[CAM LCD] frame#%lu buf=0x%08lX [0..3]=%02X %02X %02X %02X\r\n",
                   (unsigned long)frame_count, (unsigned long)vin_frame,
                   vin_frame[0], vin_frame[1], vin_frame[2], vin_frame[3]);
        }

        /*
         * 6c. Signal face detection (async, lower priority).
         *     The face_detection task runs on the same core but at lower
         *     priority.  It preprocesses the frame, runs NPU inference,
         *     and updates g_face_detection_results[] / g_face_detection_count.
         */
#if !FACE_DETECT_FLASH_PROGRAMMER
        if (enable_face_detection) {
            face_detection_signal_new_frame((uint32_t)vin_frame);
        }
#endif

        /*
         * 6d. Wait for vsync, then write to the OFF-SCREEN buffer.
         *
         * After vsync, GLCDC is scanning out the ON-SCREEN buffer
         * (fb_background[1 - write_idx]).  We write to the other one
         * (fb_background[write_idx]), which is NOT being read by GLCDC.
         *
         */
        vsync_snapshot = g_frame_count;
        {
            uint32_t vsync_spin = 0;
            while (g_frame_count == vsync_snapshot) {
                vTaskDelay(1);
                vsync_spin++;
            }
            if (vsync_spin > 2) {
                diag_skip_vsync++;  /* late — missed ≥1 vsync */
            }
        }

        /*
         * 6d2. Defence-in-depth: skip framebuffer write if NPU is busy.
         *
         * The PRIMARY fix for flicker is the AXI limiter configuration
         * in face_detection_task.c.  This flag provides an additional
         * safety net — if NPU and camera framebuffer write were to
         * overlap, SDRAM bandwidth could still be exceeded. */
#if !FACE_DETECT_FLASH_PROGRAMMER
        if (enable_face_detection && g_npu_inferencing) {
            diag_skip_npu++;
            frame_count++;
            continue;
        }
#endif

        /*
         * 6e. Write to OFF-SCREEN framebuffer.
         *
         * Vertical flip: camera sensor rows are reversed relative to
         * the display.  Word-copy (uint32_t) halves the loop count vs
         * pixel-by-pixel uint16_t, reducing SDRAM transaction overhead.
         */
        uint16_t *fb = (uint16_t *)fb_background[write_idx];
        const uint16_t *src16 = (const uint16_t *)vin_frame;

        for (int ly = 0; ly < CAM_H; ly++) {
            uint32_t *dst_line = (uint32_t *)&fb[(LCD_Y_OFF + ly) * RGBLCD_WIDTH + LCD_X_OFF];
            const uint32_t *src_line = (const uint32_t *)&src16[ly * CAM_W];
            for (int lx = 0; lx < CAM_W / 2; lx++) {
                dst_line[lx] = src_line[lx];
            }
        }

        /*
         * 6e2. Draw face detection bounding boxes (CPU rendering).
         *      The face_detection task (lower priority) updates
         *      g_face_detection_results[] asynchronously.
         *      If no face is detected this frame, the old boxes from
         *      the last successful detection are still displayed.
         */
        draw_face_boxes(fb);

        /*
         * 6e2.5 视频录制: 每 3 帧抓一帧带框画面 (~10fps) 到共享 SDRAM。
         * CPU1 编码速度更慢时会自动丢帧, 只编码最新帧。
         */
        if (g_video_record_enabled && (frame_count % 3 == 0)) {
            video_capture_frame(fb);
        }

        /*
         * 6e3. Draw face detection FPS in the upper-right corner.
         *      Uses the existing rgblcd font renderer with FONT_16 (8×16 px).
         *      Positioned inside the camera frame area (right-aligned). */
        {
            char fps_str[16];
            snprintf(fps_str, sizeof(fps_str), "FPS:%4.1f",
                     (double)g_face_detect_fps);
            /* Right-align: FONT_16 = 8px/char, "FPS:23.4" = 9 chars = 72px */
            uint16_t fps_x = LCD_X_OFF + CAM_W - 8 * 9 - 4;
            uint16_t fps_y = LCD_Y_OFF + 4;
            rgblcd_draw_string(fps_x, fps_y, fps_str,
                               RGBLCD_COLOR_GREEN, 0, RGBLCD_FONT_16);
        }

        /*
         * 6f. D-Cache Clean for the off-screen framebuffer (camera area).
         * Must clean before GLCDC reads the new pixel data from SDRAM.
         * Since we're cleaning the off-screen buffer (not the active one),
         * this does not cause GLCDC underrun on the scan-out buffer.
         */
        SCB_CleanDCache_by_Addr(
            (volatile void *)((uint32_t)fb + LCD_Y_OFF * RGBLCD_STRIDE_BYTES),
            CAM_H * RGBLCD_STRIDE_BYTES);
        __DSB();

        /*
         * 6g. Schedule atomic buffer switch at next vsync.
         * R_GLCDC_BufferChange writes the new base address to a shadow
         * register; GLCDC applies it at the next vsync without any
         * output-disable or blanking period.
         */
        R_GLCDC_BufferChange(&g_display0_ctrl, fb, DISPLAY_FRAME_LAYER_1);
        __DSB();

        /* Toggle for next frame */
        write_idx = 1 - write_idx;

        frame_count++;
        if (frame_count <= 10 || frame_count % 100 == 0) {
            printf("[CAM LCD] frame#%lu → fb%d  | skip_npu=%lu late_vsync=%lu\r\n",
                   (unsigned long)frame_count, 1 - write_idx,
                   (unsigned long)diag_skip_npu, (unsigned long)diag_skip_vsync);
        }
    }
}
