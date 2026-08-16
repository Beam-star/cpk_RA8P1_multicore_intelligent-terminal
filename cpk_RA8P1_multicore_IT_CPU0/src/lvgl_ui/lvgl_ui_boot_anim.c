/*
 * lvgl_ui_boot_anim.c
 *
 * LVGL boot animation player �?loads raw RGB565 frames from W25Q256 QSPI Flash
 * via memory-mapped XIP reads (no filesystem).
 *
 * Two loading strategies (tried in order):
 *   1. Asset directory �?scans the flash asset directory for "boot_NNN" entries.
 *   2. Hardcoded fallback �?fixed-size frames at BOOT_ANIM_FLASH_OFFSET.
 *
 * Frame format: raw RGB565, BOOT_ANIM_WIDTH × BOOT_ANIM_HEIGHT × 2 bytes each.
 */

#include "lvgl_ui_boot_anim.h"
#include "../driver/w25q256/w25q256.h"
#include "../driver/w25q256/w25q256_partition.h"
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Frame info table (populated by boot_anim_scan_dir)                         */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t offset;    /* flash offset   */
    uint32_t size;      /* bytes          */
} frame_info_t;

static frame_info_t g_frame_table[BOOT_ANIM_MAX_FRAMES];
static flash_asset_dir_t g_anim_dir;  /* ~4KB �?must NOT be on stack */
static bool         g_use_dir = false;

/* -------------------------------------------------------------------------- */
/* Static state                                                               */
/* -------------------------------------------------------------------------- */

static uint8_t  g_frame_buf_a[BOOT_ANIM_MAX_FRAME_SIZE]
    BSP_ALIGN_VARIABLE(64)
    BSP_PLACE_IN_SECTION(BSP_UNINIT_SECTION_PREFIX ".sdram_noinit");

static uint8_t  g_frame_buf_b[BOOT_ANIM_MAX_FRAME_SIZE]
    BSP_ALIGN_VARIABLE(64)
    BSP_PLACE_IN_SECTION(BSP_UNINIT_SECTION_PREFIX ".sdram_noinit");

static uint8_t *g_active_buf   = g_frame_buf_a;
static uint8_t *g_idle_buf     = g_frame_buf_b;

static lv_obj_t      *g_anim_img   = NULL;
static lv_timer_t    *g_anim_timer = NULL;
static lv_image_dsc_t  g_frame_dsc;

static int            g_total_frames  = 0;
static int            g_current_frame = 0;
static uint32_t       g_frame_period_ms;
static boot_anim_done_cb_t g_on_done = NULL;

/* -------------------------------------------------------------------------- */
/* Asset directory scan                                                        */
/* -------------------------------------------------------------------------- */

int boot_anim_scan_dir(void)
{
    /* Read directory sector via memory-mapped XIP into static buffer */
    memcpy(&g_anim_dir, (const void *)(W25Q256_MEM_BASE + PART_ASSET_DIR_OFFSET),
           sizeof(g_anim_dir));

    if (g_anim_dir.magic != FLASH_ASSET_MAGIC || g_anim_dir.count == 0
        || g_anim_dir.count > FLASH_ASSET_MAX_ENTRIES) {
        return 0;  /* directory not valid */
    }

    /* Collect entries matching "boot_NNN" */
    int found = 0;
    for (uint32_t i = 0; i < g_anim_dir.count && found < BOOT_ANIM_MAX_FRAMES; i++) {
        flash_asset_entry_t *e = &g_anim_dir.entries[i];
        if (e->name[0] == '\0' || e->name[0] == (char)0xFF) continue;
        if (e->size == 0 || e->offset < PART_LVGL_DATA_OFFSET) continue;

        /* Match "boot_" prefix */
        if (strncmp(e->name, "boot_", 5) != 0) continue;

        g_frame_table[found].offset = e->offset;
        g_frame_table[found].size   = e->size;
        found++;
    }

    if (found > 0) {
        printf("[ANIM] Directory scan: %d frames\r\n", found);
    }
    return found;
}

/* -------------------------------------------------------------------------- */
/* Load one frame from W25Q256 via memory-mapped XIP read                      */
/* -------------------------------------------------------------------------- */

static bool load_frame(int frame_idx, uint8_t *dst)
{
    if (frame_idx < 0 || frame_idx >= g_total_frames) {
        return false;
    }

    uint32_t offset;
    uint32_t size;

    if (g_use_dir) {
        offset = g_frame_table[frame_idx].offset;
        size   = g_frame_table[frame_idx].size;
    } else {
        offset = BOOT_ANIM_FLASH_OFFSET
               + (uint32_t)frame_idx * BOOT_ANIM_FRAME_SIZE;
        size   = BOOT_ANIM_FRAME_SIZE;
    }

    if (offset < PART_LVGL_DATA_OFFSET
        || offset + size > W25Q256_CAPACITY) {
        printf("[ANIM] frame %d offset/size out of range\r\n", frame_idx);
        return false;
    }

    if (size > BOOT_ANIM_MAX_FRAME_SIZE) {
        printf("[ANIM] frame %d too large: %lu > %lu\r\n",
               frame_idx, (unsigned long)size,
               (unsigned long)BOOT_ANIM_MAX_FRAME_SIZE);
        return false;
    }

    memcpy(dst, (const void *)(W25Q256_MEM_BASE + offset), size);
    return true;
}

/* -------------------------------------------------------------------------- */
/* Update the LVGL image descriptor to point to new buffer                    */
/* -------------------------------------------------------------------------- */

static void update_image_src(uint8_t *buf)
{
    g_frame_dsc.data      = buf;
    g_frame_dsc.data_size = BOOT_ANIM_FRAME_SIZE;
    lv_image_set_src(g_anim_img, &g_frame_dsc);
}

/* -------------------------------------------------------------------------- */
/* LVGL timer callback �?load and display next frame                          */
/* -------------------------------------------------------------------------- */

static void anim_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (g_current_frame >= g_total_frames) {
        printf("[ANIM] Finished (%d frames played)\r\n", g_total_frames);
        boot_anim_stop();
        return;
    }

    if (!load_frame(g_current_frame, g_idle_buf)) {
        boot_anim_stop();
        return;
    }

    /* Swap buffers */
    uint8_t *tmp = g_active_buf;
    g_active_buf = g_idle_buf;
    g_idle_buf   = tmp;

    update_image_src(g_active_buf);
    g_current_frame++;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void boot_anim_start(lv_obj_t *parent, boot_anim_done_cb_t on_done)
{
    g_on_done = on_done;

    /* --- Strategy 1: Try asset directory --- */
    int dir_frames = boot_anim_scan_dir();
    if (dir_frames > 0) {
        g_use_dir       = true;
        g_total_frames  = dir_frames;
        g_current_frame = 0;
        g_frame_period_ms = 1000 / BOOT_ANIM_FPS;

        printf("[ANIM] Using asset directory: %d frames @ %lu fps\r\n",
               g_total_frames, (unsigned long)BOOT_ANIM_FPS);
    } else {
        /* --- Strategy 2: Hardcoded fallback --- */
        uint8_t test_header[8];
        memcpy(test_header,
               (const void *)(W25Q256_MEM_BASE + BOOT_ANIM_FLASH_OFFSET),
               sizeof(test_header));

        bool blank = true;
        for (int i = 0; i < (int)sizeof(test_header); i++) {
            if (test_header[i] != 0xFF && test_header[i] != 0x00) {
                blank = false;
                break;
            }
        }

        if (blank) {
            printf("[ANIM] No animation data �?skipping\r\n");
            if (on_done) on_done();
            return;
        }

        g_use_dir       = false;
        g_total_frames  = BOOT_ANIM_FRAME_COUNT;
        g_current_frame = 0;
        g_frame_period_ms = 1000 / BOOT_ANIM_FPS;

        printf("[ANIM] Using hardcoded fallback: offset 0x%lX, %d frames\r\n",
               (unsigned long)BOOT_ANIM_FLASH_OFFSET, g_total_frames);
    }

    /* Initialize the image descriptor template */
    memset(&g_frame_dsc, 0, sizeof(g_frame_dsc));
    g_frame_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    g_frame_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    g_frame_dsc.header.w      = BOOT_ANIM_WIDTH;
    g_frame_dsc.header.h      = BOOT_ANIM_HEIGHT;
    g_frame_dsc.header.stride = BOOT_ANIM_WIDTH * 2;
    g_frame_dsc.data_size     = BOOT_ANIM_FRAME_SIZE;

    /* Create the LVGL image object, centered on screen */
    g_anim_img = lv_image_create(parent);
    lv_obj_set_size(g_anim_img, BOOT_ANIM_WIDTH, BOOT_ANIM_HEIGHT);
    lv_obj_center(g_anim_img);

    /* Load and display the first frame */
    if (!load_frame(0, g_active_buf)) {
        printf("[ANIM] Failed to load first frame\r\n");
        boot_anim_stop();
        return;
    }
    update_image_src(g_active_buf);
    g_current_frame = 1;

    /* Start the playback timer */
    g_anim_timer = lv_timer_create(anim_timer_cb, g_frame_period_ms, NULL);

    printf("[ANIM] Started: %d frames @ %lu fps (period %lu ms)\r\n",
           g_total_frames,
           (unsigned long)BOOT_ANIM_FPS,
           (unsigned long)g_frame_period_ms);
}

void boot_anim_stop(void)
{
    if (g_anim_timer) {
        lv_timer_delete(g_anim_timer);
        g_anim_timer = NULL;
    }

    if (g_anim_img) {
        lv_obj_delete(g_anim_img);
        g_anim_img = NULL;
    }

    g_active_buf    = g_frame_buf_a;
    g_idle_buf      = g_frame_buf_b;
    g_use_dir       = false;
    g_total_frames  = 0;
    g_current_frame = 0;

    if (g_on_done) {
        boot_anim_done_cb_t cb = g_on_done;
        g_on_done = NULL;
        cb();
    }
}
