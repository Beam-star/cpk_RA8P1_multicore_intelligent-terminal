/*
 * boot_logo.c
 *
 * Static boot logo player (see boot_logo.h). Reads a full-screen RGB565 image
 * named "logo" from the W25Q256 asset directory via memory-mapped XIP and blits
 * it into the layer-1 framebuffer (fb_background[0]).
 */

#include "boot_logo.h"
#include "../driver/w25q256/w25q256.h"
#include "../driver/w25q256/w25q256_partition.h"
#include "rgblcd.h"          /* RGBLCD_WIDTH/HEIGHT/STRIDE_BYTES */
#include "common_data.h"     /* fb_background[], g_display0_ctrl, R_GLCDC_BufferChange */
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

/* D-Cache clean (implemented in CMSIS/BSP; declared like mipi_camera_lcd.c) */
extern void SCB_CleanDCache_by_Addr(volatile void *addr, int32_t dsize);

/* Full-screen framebuffer size in bytes: 1024 x 600 x 2 = 1,228,800 */
#define BOOT_LOGO_FB_BYTES  ((uint32_t)RGBLCD_STRIDE_BYTES * RGBLCD_HEIGHT)

/* -------------------------------------------------------------------------- */
/* Look up a named asset in the flash asset directory (XIP read).             */
/* -------------------------------------------------------------------------- */

static bool boot_logo_find_asset(const char *name, uint32_t *offset, uint32_t *size)
{
    /* ~4 KB �?static, must NOT be on the stack */
    static flash_asset_dir_t dir;

    memcpy(&dir, (const void *)(W25Q256_MEM_BASE + PART_ASSET_DIR_OFFSET), sizeof(dir));

    if (dir.magic != FLASH_ASSET_MAGIC || dir.count == 0
        || dir.count > FLASH_ASSET_MAX_ENTRIES) {
        return false;
    }

    for (uint32_t i = 0; i < dir.count; i++) {
        flash_asset_entry_t *e = &dir.entries[i];
        if (e->name[0] == '\0' || e->name[0] == (char)0xFF) continue;
        if (strncmp(e->name, name, sizeof(e->name)) == 0) {
            *offset = e->offset;
            *size   = e->size;
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void boot_logo_show_layer1(uint32_t hold_ms)
{
    uint16_t *fb = (uint16_t *)fb_background[0];
    uint32_t off = 0, sz = 0;
    bool found = boot_logo_find_asset("logo", &off, &sz);

    if (found && sz == BOOT_LOGO_FB_BYTES
        && off >= PART_LVGL_DATA_OFFSET && (off + sz) <= W25Q256_CAPACITY) {
        /* Full-screen 1024x600 RGB565 logo �?blit straight into layer-1 fb */
        memcpy(fb, (const void *)(W25Q256_MEM_BASE + off), sz);
        printf("[BOOT] Logo shown (asset @0x%06lX, %lu bytes)\r\n",
               (unsigned long)off, (unsigned long)sz);
    } else {
        /* Missing/invalid asset: dark screen, still proceed with boot */
        for (uint32_t i = 0; i < (uint32_t)RGBLCD_WIDTH * RGBLCD_HEIGHT; i++) {
            fb[i] = RGBLCD_COLOR_BLACK;
        }
        if (found) {
            printf("[BOOT] Logo size mismatch (%lu, expect %lu) �?dark screen\r\n",
                   (unsigned long)sz, (unsigned long)BOOT_LOGO_FB_BYTES);
        } else {
            printf("[BOOT] No 'logo' asset in flash �?dark screen\r\n");
        }
    }

    /* Push the framebuffer to GLCDC layer 1 (camera layer) */
    SCB_CleanDCache_by_Addr((volatile void *)fb, BOOT_LOGO_FB_BYTES);
    __DSB();
    R_GLCDC_BufferChange(&g_display0_ctrl, fb, DISPLAY_FRAME_LAYER_1);
    __DSB();

#if GLCDC_CFG_LAYER_2_ENABLE
    /*
     * Layer 2 (384×600 panel at x=640) covers the right portion of layer 1.
     * Copy the right 384×600 of the logo into fb_foreground[0] so the full
     * 1024×600 logo is visible across both layers.
     *
     * Logo layout in flash (1024×600 RGB565, stride = 2048 bytes/row):
     *   Bytes 0..1279        �?x=0..639   (left side,  layer 1 only)
     *   Bytes 1280..2047     �?x=640..1023 (right side, covered by layer 2)
     *
     * fb_foreground stride = 768 bytes/row (384 px × 2 Bpp).
     * Since 768 = 384×2, one row of the right logo portion fits in one
     * foreground row �?copy row-by-row.
     *
     * Only fill when the logo was found (same condition as layer 1 above);
     * otherwise leave fb_foreground as black (already cleared by the
     * layer-1 fallback memset �?D-Cache clean chain on fb_background
     * does NOT cover fb_foreground, so we zero it explicitly here).
     */
    {
        extern uint8_t fb_foreground[2][DISPLAY_BUFFER_STRIDE_BYTES_INPUT1 * DISPLAY_VSIZE_INPUT1];
        const uint32_t fg_stride = (uint32_t)DISPLAY_BUFFER_STRIDE_BYTES_INPUT1;
        uint32_t fg_bytes = fg_stride * (uint32_t)DISPLAY_VSIZE_INPUT1;

        if (found && sz == BOOT_LOGO_FB_BYTES
            && off >= PART_LVGL_DATA_OFFSET && (off + sz) <= W25Q256_CAPACITY) {
            const uint32_t logo_stride = (uint32_t)RGBLCD_STRIDE_BYTES;  /* 2048 */
            const uint32_t logo_x_off  = 640 * 2;  /* byte offset to x=640 */

            for (int y = 0; y < (int)DISPLAY_VSIZE_INPUT1; y++) {
                const uint8_t *src = (const uint8_t *)(W25Q256_MEM_BASE + off)
                                   + y * logo_stride + logo_x_off;
                uint8_t *dst = &fb_foreground[0][y * fg_stride];
                memcpy(dst, src, fg_stride);  /* 384 px × 2 = 768 bytes */
            }
        } else {
            /* Logo not found �?fill with black to avoid SDRAM garbage */
            memset(fb_foreground[0], 0x00, fg_bytes);
        }

        /* Also fill buffer 1 (RM_LVGL_PORT may use double buffering) */
        memcpy(fb_foreground[1], fb_foreground[0], fg_bytes);

        /* D-Cache clean both foreground buffers */
        SCB_CleanDCache_by_Addr((volatile void *)fb_foreground[0], (int32_t)fg_bytes);
        SCB_CleanDCache_by_Addr((volatile void *)fb_foreground[1], (int32_t)fg_bytes);
        __DSB();

        /* Schedule buffer switch for layer 2 at next vsync */
        R_GLCDC_BufferChange(&g_display0_ctrl, fb_foreground[0],
                             DISPLAY_FRAME_LAYER_2);
        __DSB();

        printf("[BOOT] Layer-2 right panel: %lux%u (stride=%lu)\r\n",
               (unsigned)DISPLAY_HSIZE_INPUT1, (unsigned)DISPLAY_VSIZE_INPUT1,
               (unsigned long)fg_stride);
    }
#endif /* GLCDC_CFG_LAYER_2_ENABLE */

    if (hold_ms) {
        vTaskDelay(pdMS_TO_TICKS(hold_ms));
    }
}
