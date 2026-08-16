/**
 ******************************************************************************
 * @file    lvgl_ui_textview.c
 * @brief   会议记录文本查看器 (LVGL screen)
 *
 * 屏幕布局 (384×600):
 *   标题栏  y=0..47   [返回按钮] + 文件名 (中文字库)
 *   正文    y=52..600 可滚动容器 + 自动换行 label (中文字库)
 *
 * 正文较长时用「可滚动容器 + 换行 label」而非 textarea: 只读 + 触摸滚动
 * 更可靠, 也不会引入 textarea 的编辑光标/最大长度等副作用。
 ******************************************************************************
 */

#include "lvgl_ui_textview.h"
#include "lvgl_ui_page2.h"
#include "myChineseFont.h"
#include "lvgl.h"
#include <string.h>

#define UI_W  384
#define UI_H  600
#define X0    8

#define C_BG       0x000000
#define C_TITLE_BG 0x0C0C18
#define C_TEXT     0xF0F0F0
#define C_DIM      0xB0BEC5
#define C_ACCENT   0x42A5F5

static lv_obj_t *g_screen = NULL;
static bool      g_open   = false;

/* 返回文件浏览页 (在点击事件回调中直接切屏是安全的 — 非 indev 读回调) */
static void back_cb(lv_event_t *e)
{
    (void)e;
    lvgl_ui_textview_close();
}

void lvgl_ui_textview_open(const char *filename, const char *text)
{
    /* 上一次遗留的查看器 screen 现在已不活跃, 安全删除再重建 */
    if (g_screen) {
        lv_obj_delete(g_screen);
        g_screen = NULL;
    }

    g_screen = lv_obj_create(NULL);
    lv_obj_set_size(g_screen, UI_W, UI_H);
    lv_obj_set_style_bg_color(g_screen, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(g_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_screen, 0, 0);

    /* ---- 标题栏 ---- */
    lv_obj_t *tbar = lv_obj_create(g_screen);
    lv_obj_set_size(tbar, UI_W, 48);
    lv_obj_set_pos(tbar, 0, 0);
    lv_obj_set_style_bg_color(tbar, lv_color_hex(C_TITLE_BG), 0);
    lv_obj_set_style_border_width(tbar, 0, 0);
    lv_obj_set_style_radius(tbar, 0, 0);
    lv_obj_set_style_pad_all(tbar, 0, 0);
    lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(tbar);
    lv_obj_set_size(back, 72, 36);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(back, 6, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(tbar);
    lv_label_set_text(title, filename ? filename : "");
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(title, &myChineseFont, 0);
    lv_obj_align(title, LV_ALIGN_RIGHT_MID, -8, 0);

    /* ---- 正文: 可滚动容器 + 自动换行 label ---- */
    lv_obj_t *cont = lv_obj_create(g_screen);
    lv_obj_set_size(cont, UI_W - 2 * X0, UI_H - 48 - 12);
    lv_obj_set_pos(cont, X0, 52);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x050508), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cont, 1, 0);
    lv_obj_set_style_border_color(cont, lv_color_hex(0x152642), 0);
    lv_obj_set_style_pad_all(cont, 10, 0);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);  /* 使能滚动 */
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);        /* 仅纵向滚动 */

    lv_obj_t *lbl = lv_label_create(cont);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl, text ? text : "(empty)");
    lv_obj_set_width(lbl, lv_pct(100));        /* 约束宽度以触发换行 */
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(lbl, &myChineseFont, 0);

    lv_screen_load(g_screen);
    g_open = true;
}

void lvgl_ui_textview_close(void)
{
    g_open = false;
    lv_obj_t *p2 = lvgl_ui_page2_get_screen();
    if (p2) lv_screen_load(p2);
}

bool lvgl_ui_textview_is_open(void)
{
    return g_open;
}
