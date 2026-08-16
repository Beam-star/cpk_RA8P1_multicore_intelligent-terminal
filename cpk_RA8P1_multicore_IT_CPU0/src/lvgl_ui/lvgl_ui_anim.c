/**
 ******************************************************************************
 * @file    lvgl_ui_anim.c
 * @brief   UI frame players — animated mascot + fingerprint loading spinner.
 *
 * Frames are preloaded from W25Q256 to fixed SDRAM addresses at boot
 * (lvgl_ui_anim_init), then the LVGL timer only swaps the image source
 * pointer.  Runtime cost per tick = one pointer switch + a 96×96 D/AVE 2D
 * re-render; no XIP read, no memcpy, no SDRAM write — so it can't starve the
 * LVGL task or trip the OSPI_B state (which w25q256_close() disables after
 * the AI model loader runs).
 *
 * Preload SDRAM (0x68C00000+) is CACHEABLE (outside the 0x68000000-0x687FFFFF
 * non-cacheable window), so it is D-cache cleaned once after loading — the
 * CPU never touches it again at runtime (D/AVE 2D reads it via AXI directly).
 ******************************************************************************
 */

#include "lvgl_ui_anim.h"
#include "driver/w25q256/w25q256.h"
#include <string.h>
#include <stdio.h>

extern void SCB_CleanDCache_by_Addr(volatile void *addr, int32_t dsize);

/* Fixed SDRAM addresses for the preloaded frame sets (after the Chinese font
 * at 0x68800000, safely inside the CPU0 15 MB region). */
#define MASCOT_FRAMES_SDRAM   0x68C00000UL
#define LOADING_FRAMES_SDRAM  0x68C80000UL

/* ---- Colours (match lvgl_ui_main.c theme) ---- */
#define C_POP_BG     0x081A2E
#define C_POP_BORDER 0x4FC3F7

/* ======================================================================== */
/*  Frame player (pointer-swap only)                                         */
/* ======================================================================== */

typedef struct {
    lv_image_dsc_t dsc;
    lv_obj_t      *img;
    lv_timer_t    *timer;
    uint8_t       *frames;      /* SDRAM base of preloaded frames */
    uint16_t       w, h;
    uint32_t       frame_size;
    int            total_frames;
    int            current;
} anim_player_t;

static void player_init(anim_player_t *p, uint8_t *frames,
                        uint16_t w, uint16_t h, int total)
{
    memset(p, 0, sizeof(*p));
    p->frames = frames;
    p->w = w;
    p->h = h;
    p->frame_size = (uint32_t)w * h * 2;
    p->total_frames = total;

    p->dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    p->dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    p->dsc.header.w      = w;
    p->dsc.header.h      = h;
    p->dsc.header.stride = w * 2;
    p->dsc.data_size     = p->frame_size;
}

static void player_show(anim_player_t *p, int idx)
{
    p->dsc.data = p->frames + (uint32_t)idx * p->frame_size;
    if (p->img) lv_image_set_src(p->img, &p->dsc);
}

static void player_timer_cb(lv_timer_t *t)
{
    anim_player_t *p = (anim_player_t *)lv_timer_get_user_data(t);
    if (!p || !p->img || p->total_frames <= 0) return;
    p->current = (p->current + 1) % p->total_frames;
    player_show(p, p->current);
}

static void player_start(anim_player_t *p, lv_obj_t *parent, uint32_t period_ms)
{
    if (p->total_frames <= 0 || !p->frames) return;
    p->img = lv_image_create(parent);
    lv_obj_set_size(p->img, p->w, p->h);
    player_show(p, 0);
    p->current = 0;
    p->timer = lv_timer_create(player_timer_cb, period_ms, p);
}

static void player_stop(anim_player_t *p)
{
    if (p->timer) { lv_timer_delete(p->timer); p->timer = NULL; }
    if (p->img)   { lv_obj_delete(p->img);       p->img = NULL; }
    p->current = 0;
}

/* ======================================================================== */
/*  Preload at boot                                                          */
/* ======================================================================== */

void lvgl_ui_anim_init(void)
{
    w25q256_read(MASCOT_ANIM_OFFSET, (uint8_t *)MASCOT_FRAMES_SDRAM, MASCOT_ANIM_TOTAL);
    SCB_CleanDCache_by_Addr((volatile void *)MASCOT_FRAMES_SDRAM, (int32_t)MASCOT_ANIM_TOTAL);

    w25q256_read(LOADING_ANIM_OFFSET, (uint8_t *)LOADING_FRAMES_SDRAM, LOADING_ANIM_TOTAL);
    SCB_CleanDCache_by_Addr((volatile void *)LOADING_FRAMES_SDRAM, (int32_t)LOADING_ANIM_TOTAL);

    printf("[ANIM] frames preloaded (mascot 0x%lX, loading 0x%lX)\r\n",
           (unsigned long)MASCOT_FRAMES_SDRAM, (unsigned long)LOADING_FRAMES_SDRAM);
}

/* ======================================================================== */
/*  Players                                                                  */
/* ======================================================================== */

static anim_player_t s_mascot;
static anim_player_t s_loading;
static bool          s_loading_shown = false;
static lv_obj_t     *s_loading_popup = NULL;

lv_obj_t *lvgl_ui_mascot_start(lv_obj_t *parent, lv_coord_t x, lv_coord_t y)
{
    player_init(&s_mascot, (uint8_t *)MASCOT_FRAMES_SDRAM,
                MASCOT_ANIM_W, MASCOT_ANIM_H, MASCOT_ANIM_FRAMES);
    /* 250 ms/frame ≈ 4 fps */
    player_start(&s_mascot, parent, 250);
    if (s_mascot.img) {
        lv_obj_set_pos(s_mascot.img, x, y);
    }
    return s_mascot.img;
}

void lvgl_ui_mascot_stop(void)
{
    player_stop(&s_mascot);
}

static void loading_close_cb(lv_event_t *e)
{
    lvgl_ui_loading_cancel_cb_t cb =
        (lvgl_ui_loading_cancel_cb_t)lv_event_get_user_data(e);
    lvgl_ui_loading_hide();
    if (cb) cb();
}

void lvgl_ui_loading_show(const char *text, lvgl_ui_loading_cancel_cb_t on_cancel)
{
    if (s_loading_shown) return;

    player_init(&s_loading, (uint8_t *)LOADING_FRAMES_SDRAM,
                LOADING_ANIM_W, LOADING_ANIM_H, LOADING_ANIM_FRAMES);

    lv_obj_t *popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(popup, 240, 220);
    lv_obj_align(popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(popup, lv_color_hex(C_POP_BG), 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(popup, 2, 0);
    lv_obj_set_style_border_color(popup, lv_color_hex(C_POP_BORDER), 0);
    lv_obj_set_style_radius(popup, 12, 0);

    player_start(&s_loading, popup, 100);
    if (s_loading.img) {
        lv_obj_align(s_loading.img, LV_ALIGN_TOP_MID, 0, 18);
    }

    lv_obj_t *lbl = lv_label_create(popup);
    lv_label_set_text(lbl, text ? text : "Please wait...");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -18);

    /* 右上角取消叉号（可选） */
    if (on_cancel) {
        lv_obj_t *btn = lv_button_create(popup);
        lv_obj_set_size(btn, 36, 36);
        lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -6, 6);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A1215), 0);
        lv_obj_set_style_radius(btn, 18, 0);
        lv_obj_t *x = lv_label_create(btn);
        lv_label_set_text(x, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(x, lv_color_hex(0xEF5350), 0);
        lv_obj_center(x);
        lv_obj_add_event_cb(btn, loading_close_cb, LV_EVENT_CLICKED, (void *)on_cancel);
    }

    s_loading_popup = popup;
    s_loading_shown = true;
}

void lvgl_ui_loading_hide(void)
{
    player_stop(&s_loading);
    if (s_loading_popup) {
        lv_obj_delete(s_loading_popup);
        s_loading_popup = NULL;
    }
    s_loading_shown = false;
}
