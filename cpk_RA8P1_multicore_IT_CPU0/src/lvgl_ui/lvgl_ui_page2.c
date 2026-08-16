/**
 ******************************************************************************
 * @file    lvgl_ui_page2.c
 * @brief   UI 第二页 — SD 卡音频/视频文件浏览 + 音频播放
 *
 * 列表通过 RPMsg 分两次请求(audio/video 各带 tag),CPU1 分包回复,
 * rpmsg_record_cpu0.c 累积完整后调 lvgl_ui_page2_on_list(tag, ...)。
 * 音频文件可点击播放;视频文件(占位符)灰显不可播。
 ******************************************************************************
 */

#include "lvgl_ui_main.h"
#include "lvgl_ui_page2.h"
#include "lvgl_ui_page3.h"
#include "lvgl_ui_page4.h"
#include "lvgl_ui_textview.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "lvgl.h"
#include "rpmsg_record_cpu0.h"
#include "rpmsg_record.h"
#include <stdio.h>
#include <string.h>

#define UI_W  384
#define UI_H  600
#define X0    8

/* 文件条目实际占用宽度 = 列表宽(UI_W-16) 再预留 12px 给右侧滚动条,
 * 避免滚动条与最右侧的删除按钮重叠 (重叠会导致拖动滚动条时误触按钮卡死)。 */
#define ROW_W (UI_W - 16 - 12)

#define C_BG       0x0D1B2A   /* 背景渐变顶部（深蓝） */
#define C_BG_GRAD  0x1B0F2E   /* 背景渐变底部（深紫） */
#define C_TITLE_BG 0x0A1220
#define C_BTN      0x1E3350
#define C_TEXT     0xFFFFFF
#define C_ACCENT   0x4FC3F7
#define C_DIM      0x555555      /* 视频条目边框 (不可播) */
#define C_DIMTEXT  0x999999      /* 视频条目文字           */

#define UI_FONT  (&lv_font_montserrat_16)

static char    g_audio_names[FILE_LIST_MAX][32];
static int     g_audio_count = 0;
static char    g_video_names[FILE_LIST_MAX][32];
static int     g_video_count = 0;
static char    g_notes_names[FILE_LIST_MAX][32];
static int     g_notes_count = 0;
static bool    g_list_received = false;

static lv_obj_t *g_page1 = NULL;
static lv_obj_t *g_page2 = NULL;
static lv_obj_t *g_list  = NULL;
static int       g_cur_page = 0;

/* 播放控制状态 */
static lv_obj_t *g_pause_label  = NULL;
static lv_obj_t *g_status_label = NULL;
static bool       g_is_playing  = false;
static bool       g_is_paused   = false;

/* 播放进度 */
static lv_obj_t *g_time_label   = NULL;
static lv_obj_t *g_progress_bar = NULL;
static struct { uint32_t elapsed_ms; uint32_t total_ms; } g_progress = {0, 0};

/* ---- 分页 (用左右翻页替代上下滚动, 从根上规避列表滚动卡死) ---- */
#define PAGE_ROWS 6                   /* 每页最多显示的文件条目数 */
static int       g_page       = 0;    /* 当前页码 (0-based) */
static int       g_page_count = 1;    /* 总页数 */
static lv_obj_t *g_page_label = NULL; /* "1/N" 页码指示 */

/* ======================================================================== */

void lvgl_ui_page2_on_list(uint32_t tag, const char names[][32], int count)
{
    char (*dst)[32];
    switch (tag) {
    case REC_LIST_TAG_VIDEO: dst = g_video_names; break;
    case REC_LIST_TAG_NOTES: dst = g_notes_names; break;
    default:                 dst = g_audio_names; break;
    }
    int  n = (count > FILE_LIST_MAX) ? FILE_LIST_MAX : count;

    for (int i = 0; i < n; i++) {
        strncpy(dst[i], names[i], 31);
        dst[i][31] = '\0';
    }
    if (tag == REC_LIST_TAG_VIDEO)      g_video_count = n;
    else if (tag == REC_LIST_TAG_NOTES) g_notes_count = n;
    else                                g_audio_count = n;
    g_list_received = true;
}

/* ======================================================================== */

/* 前向声明: delete_video_cb 在定义之前会调用 */
static void page2_request_lists(void);
static void reset_progress(void);

static void update_pause_label(void)
{
    if (g_pause_label) {
        lv_label_set_text(g_pause_label, g_is_paused ? "Resume" : "Pause");
    }
    /* 播放状态提示: 空闲不显示, 播放中绿色 "Playing", 暂停橙色 "Paused" */
    if (g_status_label) {
        if (!g_is_playing) {
            lv_label_set_text(g_status_label, "");
        } else if (g_is_paused) {
            lv_label_set_text(g_status_label, "Paused");
            lv_obj_set_style_text_color(g_status_label, lv_color_hex(0xFFB300), 0);
        } else {
            lv_label_set_text(g_status_label, "Playing");
            lv_obj_set_style_text_color(g_status_label, lv_color_hex(0x4CAF50), 0);
        }
    }
}

/* 暂停 / 恢复 */
static void pause_resume_cb(lv_event_t *e)
{
    (void)e;
    if (!g_is_playing) return;
    if (g_is_paused) {
        g_is_paused = false;
        rpmsg_record_send_resume();
        lvgl_ui_log("Resumed");
    } else {
        g_is_paused = true;
        rpmsg_record_send_pause();
        lvgl_ui_log("Paused");
    }
    update_pause_label();
}

/* 取消播放 */
static void cancel_cb(lv_event_t *e)
{
    (void)e;
    if (!g_is_playing) return;
    rpmsg_record_send_stop_play();
    g_is_playing = false;
    g_is_paused  = false;
    update_pause_label();
    reset_progress();
    lvgl_ui_log("Stopped");
}

/* 通用弹窗提示 (阻断操作) */
static void show_block_popup(const char *msg)
{
    lv_obj_t *mb = lv_msgbox_create(lv_screen_active());
    lv_msgbox_add_title(mb, "Notice");
    lv_msgbox_add_text(mb, msg);
    lv_msgbox_add_close_button(mb);
    lvgl_ui_log(msg);
}

/* 由 REC_EVT_*_DONE 经 lv_async_call 调用, 复位播放状态 */
static void set_playing_async(void *p)
{
    g_is_playing = (bool)(uintptr_t)p;
    if (!g_is_playing) g_is_paused = false;
    update_pause_label();
    if (!g_is_playing) reset_progress();
}

void lvgl_ui_page2_set_playing(bool playing)
{
    lv_async_call(set_playing_async, (void *)(uintptr_t)playing);
}

bool lvgl_ui_page2_is_playing(void)
{
    return g_is_playing;
}

static void format_time(uint32_t ms, char *buf, size_t len)
{
    uint32_t sec = ms / 1000u;
    snprintf(buf, len, "%02lu:%02lu",
             (unsigned long)(sec / 60u), (unsigned long)(sec % 60u));
}

static void reset_progress(void)
{
    if (g_time_label) lv_label_set_text(g_time_label, "00:00 / 00:00");
    if (g_progress_bar) lv_bar_set_value(g_progress_bar, 0, LV_ANIM_OFF);
}

static void set_progress_async(void *p)
{
    (void)p;
    if (!g_time_label || !g_progress_bar) return;

    char t1[16], t2[16], buf[40];
    format_time(g_progress.elapsed_ms, t1, sizeof(t1));
    format_time(g_progress.total_ms, t2, sizeof(t2));
    snprintf(buf, sizeof(buf), "%s / %s", t1, t2);
    lv_label_set_text(g_time_label, buf);

    uint32_t total = g_progress.total_ms;
    int percent = (total > 0)
        ? (int)((uint64_t)g_progress.elapsed_ms * 100u / total) : 0;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    lv_bar_set_value(g_progress_bar, percent, LV_ANIM_OFF);
}

void lvgl_ui_page2_set_progress(uint32_t elapsed_ms, uint32_t total_ms)
{
    g_progress.elapsed_ms = elapsed_ms;
    g_progress.total_ms = total_ms;
    lv_async_call(set_progress_async, NULL);
}

bool lvgl_ui_page2_play_audio_path(const char *path)
{
    if (!path) return false;
    if (g_is_playing) {
        show_block_popup("Please stop the current playback first");
        return false;
    }
    if (lvgl_ui_is_recording()) {
        show_block_popup("Recording in progress, cannot play");
        return false;
    }
    lvgl_ui_log("Playing...");
    g_is_playing = true;
    g_is_paused  = false;
    update_pause_label();
    reset_progress();
    rpmsg_record_send_play(path);
    return true;
}

static void play_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    if (!name) return;
    char path[64];
    snprintf(path, sizeof(path), "/meeting/audio/%s", name);
    lvgl_ui_page2_play_audio_path(path);
}

static void video_play_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    if (!name) return;
    if (g_is_playing) { show_block_popup("Please stop the current playback first"); return; }
    if (lvgl_ui_is_recording()) { show_block_popup("Recording in progress, cannot play"); return; }
    char path[64];
    snprintf(path, sizeof(path), "/meeting/video/%s", name);
    lvgl_ui_log("Playing video...");
    g_is_playing = true;
    g_is_paused  = false;
    update_pause_label();
    reset_progress();
    rpmsg_record_send_play_video(path);
}

/* 删除视频 (CPU1 连带删除配对的同名音频) */
static void delete_video_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    if (!name) return;
    char path[64];
    snprintf(path, sizeof(path), "/meeting/video/%s", name);
    lvgl_ui_log("Deleting video...");
    rpmsg_record_send_delete(path);
    page2_request_lists();
}

/* 请求三个目录的文件列表 (CPU1 串行回复,不会交错) */
static void page2_request_lists(void)
{
    rpmsg_record_send_list("/meeting/audio", REC_LIST_TAG_AUDIO);
    rpmsg_record_send_list("/meeting/video", REC_LIST_TAG_VIDEO);
    rpmsg_record_send_list("/meeting/notes", REC_LIST_TAG_NOTES);
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    page2_request_lists();
}

/* 删除录音 (CPU1 连带删除配对占位视频), 然后刷新列表。
 * DELETE 与 LIST 由 CPU1 同一任务顺序处理, 列表回来时删除已完成。 */
static void delete_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    if (!name) return;
    char path[64];
    snprintf(path, sizeof(path), "/meeting/audio/%s", name);
    lvgl_ui_log("Deleting...");
    rpmsg_record_send_delete(path);
    page2_request_lists();
}

/* 点击会议记录 txt → CPU1 读取内容 → 文本查看器显示 */
static void view_notes_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    if (!name) return;
    lvgl_ui_log("Opening notes...");
    rpmsg_record_send_notes_read(name);
}

/* 删除会议记录 txt（/meeting/notes/ 无音频/视频配对, 直接删除） */
static void delete_notes_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    if (!name) return;
    char path[64];
    snprintf(path, sizeof(path), "/meeting/notes/%s", name);
    lvgl_ui_log("Deleting notes...");
    rpmsg_record_send_delete(path);
    page2_request_lists();
}

/* ---- 渲染辅助 ---- */

static void add_section_header(const char *text)
{
    lv_obj_t *lbl = lv_label_create(g_list);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_font(lbl, &righteous_20, 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
}

static void add_empty_hint(void)
{
    lv_obj_t *lbl = lv_label_create(g_list);
    lv_label_set_text(lbl, "(no files)");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
}

/* 文件条目 = 播放按钮 + 右侧删除按钮 (同一行); 音频/视频共用 */
static void add_file_row(const char *name, lv_event_cb_t play_cb_,
                         lv_event_cb_t del_cb_, void *user_data)
{
    lv_obj_t *row = lv_obj_create(g_list);
    lv_obj_set_size(row, ROW_W, 46);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* 播放按钮 (占据左侧大部分宽度) */
    lv_obj_t *btn = lv_button_create(row);
    lv_obj_set_size(btn, ROW_W - 52, 42);
    lv_obj_set_pos(btn, 0, 2);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_BTN), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(btn, 8, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(lbl, UI_FONT, 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, play_cb_, LV_EVENT_SHORT_CLICKED, user_data);

    /* 删除按钮 (红色垃圾桶图标) */
    lv_obj_t *del = lv_button_create(row);
    lv_obj_set_size(del, 44, 42);
    lv_obj_set_pos(del, ROW_W - 44, 2);
    lv_obj_set_style_bg_color(del, lv_color_hex(0x3A1515), 0);
    lv_obj_set_style_border_width(del, 2, 0);
    lv_obj_set_style_border_color(del, lv_color_hex(0xE05555), 0);
    lv_obj_set_style_radius(del, 8, 0);

    lv_obj_t *dl = lv_label_create(del);
    lv_label_set_text(dl, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(dl, lv_color_hex(0xE05555), 0);
    lv_obj_center(dl);
    lv_obj_add_event_cb(del, del_cb_, LV_EVENT_SHORT_CLICKED, user_data);
}

/* 会议记录条目 = 查看按钮 + 右侧删除按钮 (同一行) */
static void add_notes_row(const char *name, lv_event_cb_t view_cb,
                          lv_event_cb_t del_cb, void *user_data)
{
    lv_obj_t *row = lv_obj_create(g_list);
    lv_obj_set_size(row, ROW_W, 46);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* 查看按钮 (占据左侧大部分宽度, 绿色边框) */
    lv_obj_t *btn = lv_button_create(row);
    lv_obj_set_size(btn, ROW_W - 52, 42);
    lv_obj_set_pos(btn, 0, 2);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_BTN), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x43A047), 0);  /* green */
    lv_obj_set_style_radius(btn, 8, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(lbl, UI_FONT, 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, view_cb, LV_EVENT_SHORT_CLICKED, user_data);

    /* 删除按钮 (红色垃圾桶图标) */
    lv_obj_t *del = lv_button_create(row);
    lv_obj_set_size(del, 44, 42);
    lv_obj_set_pos(del, ROW_W - 44, 2);
    lv_obj_set_style_bg_color(del, lv_color_hex(0x3A1515), 0);
    lv_obj_set_style_border_width(del, 2, 0);
    lv_obj_set_style_border_color(del, lv_color_hex(0xE05555), 0);
    lv_obj_set_style_radius(del, 8, 0);

    lv_obj_t *dl = lv_label_create(del);
    lv_label_set_text(dl, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(dl, lv_color_hex(0xE05555), 0);
    lv_obj_center(dl);
    lv_obj_add_event_cb(del, del_cb, LV_EVENT_SHORT_CLICKED, user_data);
}

/* 更新页码指示 "1/N" */
static void update_page_ctrl(void)
{
    if (g_page_label) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d/%d", g_page + 1, g_page_count);
        lv_label_set_text(g_page_label, buf);
    }
}

static void page_prev_cb(lv_event_t *e)
{
    (void)e;
    if (g_page > 0) {
        g_page--;
        lvgl_ui_page2_refresh();
    }
}

static void page_next_cb(lv_event_t *e)
{
    (void)e;
    if (g_page < g_page_count - 1) {
        g_page++;
        lvgl_ui_page2_refresh();
    }
}

void lvgl_ui_page2_refresh(void)
{
    if (!g_list || !g_list_received) return;
    lv_obj_clean(g_list);

    /* 把 Audio/Video 两区连同各自节标题拍平成一条逻辑条目流,
     * 每页显示 PAGE_ROWS 条 (节标题占 1 条), 通过左右翻页浏览。 */
    int total = 0;
    if (g_audio_count > 0) total += 1 + g_audio_count;
    if (g_video_count > 0) total += 1 + g_video_count;
    if (g_notes_count > 0) total += 1 + g_notes_count;

    if (total == 0) {
        add_empty_hint();
        g_page       = 0;
        g_page_count = 1;
        update_page_ctrl();
        return;
    }

    g_page_count = (total + PAGE_ROWS - 1) / PAGE_ROWS;
    if (g_page < 0)             g_page = 0;
    if (g_page >= g_page_count) g_page = g_page_count - 1;

    int start = g_page * PAGE_ROWS;
    int end   = start + PAGE_ROWS;
    int idx   = 0;

    if (g_audio_count > 0) {
        if (idx >= start && idx < end) add_section_header("Audio");
        idx++;
        for (int i = 0; i < g_audio_count; i++) {
            if (idx >= start && idx < end) {
                add_file_row(g_audio_names[i], play_cb, delete_cb,
                             (void *)g_audio_names[i]);
            }
            idx++;
        }
    }

    if (g_video_count > 0) {
        if (idx >= start && idx < end) add_section_header("Video");
        idx++;
        for (int i = 0; i < g_video_count; i++) {
            if (idx >= start && idx < end) {
                add_file_row(g_video_names[i], video_play_cb, delete_video_cb,
                             (void *)g_video_names[i]);
            }
            idx++;
        }
    }

    if (g_notes_count > 0) {
        if (idx >= start && idx < end) add_section_header("Notes");
        idx++;
        for (int i = 0; i < g_notes_count; i++) {
            if (idx >= start && idx < end) {
                add_notes_row(g_notes_names[i], view_notes_cb, delete_notes_cb,
                              (void *)g_notes_names[i]);
            }
            idx++;
        }
    }

    update_page_ctrl();
}

/* ======================================================================== */

void lvgl_ui_page2_init(lv_obj_t *page1)
{
    g_page1 = page1;
    g_cur_page = 0;

    /* 第二页 = 新 screen */
    g_page2 = lv_obj_create(NULL);
    lv_obj_set_size(g_page2, UI_W, UI_H);
    ui_apply_bg_gradient(g_page2);
    lv_obj_set_style_border_width(g_page2, 0, 0);

    /* 标题栏 (56px 高, 无内边距, 不可滚动 —— 避免 Refresh 按钮被裁剪) */
    lv_obj_t *tbar = lv_obj_create(g_page2);
    lv_obj_set_size(tbar, UI_W, 56);
    lv_obj_set_pos(tbar, 0, 0);
    lv_obj_set_style_bg_color(tbar, lv_color_hex(C_TITLE_BG), 0);
    lv_obj_set_style_border_width(tbar, 0, 0);
    lv_obj_set_style_radius(tbar, 0, 0);
    lv_obj_set_style_pad_all(tbar, 0, 0);
    lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(tbar);
    lv_label_set_text(t, "Recordings");
    lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(t, &bangers_28, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 12, 0);

    /* 刷新按钮 (右侧垂直居中) */
    lv_obj_t *rf = lv_button_create(tbar);
    lv_obj_set_size(rf, 88, 40);
    lv_obj_align(rf, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(rf, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(rf, 6, 0);
    lv_obj_t *rl = lv_label_create(rf);
    lv_label_set_text(rl, "Refresh");
    lv_obj_set_style_text_font(rl, &righteous_20, 0);
    lv_obj_center(rl);
    lv_obj_add_event_cb(rf, refresh_cb, LV_EVENT_CLICKED, NULL);

    /* 文件列表 (非滚动容器 + 纵向 flex, 用左右翻页替代上下滚动) */
    g_list = lv_obj_create(g_page2);
    lv_obj_set_size(g_list, UI_W - 16, UI_H - 68 - 52 - 48 - 44);
    lv_obj_set_pos(g_list, X0, 62);
    lv_obj_set_style_bg_color(g_list, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(g_list, 0, 0);
    lv_obj_set_style_pad_all(g_list, 0, 0);
    lv_obj_clear_flag(g_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);

    /* 分页控制栏: 左右箭头 + 页码。独立于 g_list (是 g_page2 的子对象),
     * 不会被 lv_obj_clean(g_list) 清除。 */
    {
        lv_obj_t *pctrl = lv_obj_create(g_page2);
        lv_obj_set_size(pctrl, UI_W, 44);
        lv_obj_set_pos(pctrl, 0, 62 + (UI_H - 68 - 52 - 48 - 44));
        lv_obj_set_style_bg_color(pctrl, lv_color_hex(C_BG), 0);
        lv_obj_set_style_border_width(pctrl, 0, 0);
        lv_obj_set_style_pad_all(pctrl, 0, 0);
        lv_obj_clear_flag(pctrl, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(pctrl, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(pctrl, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *prev = lv_button_create(pctrl);
        lv_obj_set_size(prev, 44, 36);
        lv_obj_set_style_bg_color(prev, lv_color_hex(C_BTN), 0);
        lv_obj_set_style_radius(prev, 6, 0);
        lv_obj_t *pl = lv_label_create(prev);
        lv_label_set_text(pl, LV_SYMBOL_LEFT);
        lv_obj_center(pl);
        lv_obj_add_event_cb(prev, page_prev_cb, LV_EVENT_CLICKED, NULL);

        g_page_label = lv_label_create(pctrl);
        lv_label_set_text(g_page_label, "1/1");
        lv_obj_set_style_text_color(g_page_label, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_text_font(g_page_label, UI_FONT, 0);
        lv_obj_set_style_margin_left(g_page_label, 14, 0);
        lv_obj_set_style_margin_right(g_page_label, 14, 0);

        lv_obj_t *next = lv_button_create(pctrl);
        lv_obj_set_size(next, 44, 36);
        lv_obj_set_style_bg_color(next, lv_color_hex(C_BTN), 0);
        lv_obj_set_style_radius(next, 6, 0);
        lv_obj_t *nl = lv_label_create(next);
        lv_label_set_text(nl, LV_SYMBOL_RIGHT);
        lv_obj_center(nl);
        lv_obj_add_event_cb(next, page_next_cb, LV_EVENT_CLICKED, NULL);
    }

    /* 播放进度行 (时间 + 进度条) */
    lv_obj_t *prow = lv_obj_create(g_page2);
    lv_obj_set_size(prow, UI_W, 44);
    lv_obj_set_pos(prow, 0, UI_H - 52 - 48);
    lv_obj_set_style_bg_color(prow, lv_color_hex(C_TITLE_BG), 0);
    lv_obj_set_style_border_width(prow, 0, 0);
    lv_obj_set_style_radius(prow, 0, 0);
    lv_obj_set_style_pad_all(prow, 0, 0);
    lv_obj_clear_flag(prow, LV_OBJ_FLAG_SCROLLABLE);

    g_time_label = lv_label_create(prow);
    lv_label_set_text(g_time_label, "00:00 / 00:00");
    lv_obj_set_style_text_color(g_time_label, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(g_time_label, UI_FONT, 0);
    lv_obj_align(g_time_label, LV_ALIGN_LEFT_MID, 8, 0);

    g_progress_bar = lv_bar_create(prow);
    lv_obj_set_size(g_progress_bar, UI_W - 16 - 120, 10);
    lv_obj_align(g_progress_bar, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_bar_set_range(g_progress_bar, 0, 100);
    lv_bar_set_value(g_progress_bar, 0, LV_ANIM_OFF);

    /* 播放控制栏 (底部): 暂停/恢复 + 取消 */
    lv_obj_t *cbar = lv_obj_create(g_page2);
    lv_obj_set_size(cbar, UI_W, 48);
    lv_obj_set_pos(cbar, 0, UI_H - 52);
    lv_obj_set_style_bg_color(cbar, lv_color_hex(C_TITLE_BG), 0);
    lv_obj_set_style_border_width(cbar, 0, 0);
    lv_obj_set_style_radius(cbar, 0, 0);
    lv_obj_set_style_pad_all(cbar, 0, 0);
    lv_obj_clear_flag(cbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *pb = lv_button_create(cbar);
    lv_obj_set_size(pb, 130, 40);
    lv_obj_align(pb, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(pb, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(pb, 6, 0);
    g_pause_label = lv_label_create(pb);
    lv_label_set_text(g_pause_label, "Pause");
    lv_obj_set_style_text_font(g_pause_label, &righteous_20, 0);
    lv_obj_center(g_pause_label);
    lv_obj_add_event_cb(pb, pause_resume_cb, LV_EVENT_SHORT_CLICKED, NULL);

    lv_obj_t *cb = lv_button_create(cbar);
    lv_obj_set_size(cb, 130, 40);
    lv_obj_align(cb, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(cb, lv_color_hex(0x3A1515), 0);
    lv_obj_set_style_border_width(cb, 2, 0);
    lv_obj_set_style_border_color(cb, lv_color_hex(0xE05555), 0);
    lv_obj_set_style_radius(cb, 6, 0);
    lv_obj_t *cl = lv_label_create(cb);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(0xE05555), 0);
    lv_obj_set_style_text_font(cl, &righteous_20, 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(cb, cancel_cb, LV_EVENT_SHORT_CLICKED, NULL);

    /* 播放状态提示 (居中, 播放中显示 "Playing", 暂停显示 "Paused") */
    g_status_label = lv_label_create(cbar);
    lv_label_set_text(g_status_label, "");
    lv_obj_set_style_text_color(g_status_label, lv_color_hex(0x4CAF50), 0);
    lv_obj_set_style_text_font(g_status_label, UI_FONT, 0);
    lv_obj_center(g_status_label);

    /* 初始化加载列表 */
    page2_request_lists();
}

void lvgl_ui_handle_gesture(bool swipe_left)
{
    /* 文本查看器打开时屏蔽翻页手势, 只允许返回按钮关闭 */
    if (lvgl_ui_textview_is_open()) return;

    /* 翻页动画：左滑=下一页(新页从右滑入)、右滑=上一页(新页从左滑入)。
     * MOVE_LEFT/MOVE_RIGHT 会让新旧两页一起平移，类似手机系统翻页。
     * 页序：0 控制台(会议/外设子页由标题栏左右键切换) → 1 文件浏览 → 2 声源雷达 → 3 语音命令说明 */
    const lv_screen_load_anim_t anim = swipe_left
        ? LV_SCR_LOAD_ANIM_MOVE_LEFT
        : LV_SCR_LOAD_ANIM_MOVE_RIGHT;

    if (swipe_left) {
        if (g_cur_page == 0) {
            lv_screen_load_anim(g_page2, anim, 250, 0, false);
            g_cur_page = 1;
            page2_request_lists();      /* 每次滑入都刷新文件列表 */
        } else if (g_cur_page == 1) {
            lv_obj_t *p3 = lvgl_ui_page3_get_screen();
            if (p3) {
                lv_screen_load_anim(p3, anim, 250, 0, false);
                g_cur_page = 2;
            }
        } else if (g_cur_page == 2) {
            lv_obj_t *p4 = lvgl_ui_page4_get_screen();
            if (p4) {
                lv_screen_load_anim(p4, anim, 250, 0, false);
                g_cur_page = 3;
            }
        }
    } else {
        if (g_cur_page == 1) {
            lv_screen_load_anim(g_page1, anim, 250, 0, false);
            g_cur_page = 0;
        } else if (g_cur_page == 2) {
            lv_screen_load_anim(g_page2, anim, 250, 0, false);
            g_cur_page = 1;
        } else if (g_cur_page == 3) {
            lv_obj_t *p3 = lvgl_ui_page3_get_screen();
            if (p3) {
                lv_screen_load_anim(p3, anim, 250, 0, false);
                g_cur_page = 2;
            }
        }
    }
}

lv_obj_t *lvgl_ui_page2_get_screen(void)
{
    return g_page2;
}
