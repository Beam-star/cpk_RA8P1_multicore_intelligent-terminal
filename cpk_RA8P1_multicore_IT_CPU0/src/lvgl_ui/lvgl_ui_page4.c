/**
 ******************************************************************************
 * @file    lvgl_ui_page4.c
 * @brief   UI 第四页 — 语音命令词说明 (CI1302 声控命令参考)
 *
 * 布局 (384×600):
 *   标题栏   y=0..47   标题「语音命令说明」(中文字库)
 *   正文     y=52..600 可滚动容器 + 自动换行 label (中文字库)
 *
 * 正文采用「可滚动容器 + 换行 label」而非 textarea，与 lvgl_ui_textview.c
 * 保持一致：只读 + 触摸滚动更可靠，无编辑光标/最大长度副作用。
 ******************************************************************************
 */

#include "lvgl_ui_page4.h"
#include "myChineseFont.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "lvgl.h"

#define P4_W        384
#define P4_H        600
#define P4_TITLE_H  48
#define X0          8

#define C_BG        0x0D1B2A   /* 背景渐变顶部（深蓝） */
#define C_BG_GRAD   0x1B0F2E   /* 背景渐变底部（深紫） */
#define C_TITLE_BG  0x0A1220
#define C_TEXT      0xFFFFFF
#define C_BORDER    0x1E3350

static lv_obj_t *g_page4 = NULL;

/* 语音命令词 → 功能 说明列表（UTF-8，中文字库渲染）。
 * 标点统一用英文半角（: , ( )），避免字库缺全角标点字形导致渲染乱码。 */
static const char *P4_TEXT =
    "唤醒词: 小萨小萨\n"
    "作用: 唤醒语音模块\n"
    "\n"
    "音量控制(模块内部自动处理):\n"
    "增大音量, 减小音量\n"
    "最大音量, 中等音量, 最小音量\n"
    "开启播报, 关闭播报\n"
    "\n"
    "开始录制, 开始录制会议\n"
    "作用: 开始录制音视频\n"
    "\n"
    "停止录制, 停止录制会议\n"
    "作用: 停止录制\n"
    "\n"
    "切换模式, 切换人脸与手掌识别模式\n"
    "作用: 切换人脸与手掌识别\n"
    "\n"
    "开启语音转文字, 开启语音转文字功能\n"
    "作用: 开启实时语音转文字\n"
    "\n"
    "关闭语音转文字, 关闭语音转文字功能\n"
    "作用: 关闭实时语音转文字\n"
    "\n"
    "开启对话功能, 开启人工智能对话\n"
    "作用: 开启智能对话\n"
    "\n"
    "关闭对话功能, 关闭人工智能对话\n"
    "作用: 关闭智能对话\n"
    "\n"
    "开启控制PPT, 开启控制PPT功能\n"
    "作用: 开启手掌翻页(需手掌模式)\n"
    "\n"
    "关闭控制PPT, 关闭控制PPT功能\n"
    "作用: 关闭手掌翻页\n"
    "\n"
    "放一首歌听听\n"
    "作用: 播放SD卡歌曲ForgetTime.wav\n"
    "\n"
    "人脸追踪(舵机, 默认追踪一号):\n"
    "开始追踪人脸, 开始追踪人脸功能\n"
    "追踪一号, 二号, 三号人脸\n"
    "取消追踪, 取消追踪人脸\n"
    "\n"
    "声源追踪(舵机, 左右各35度):\n"
    "开始声源追踪, 开始声源追踪功能\n"
    "\n"
    "指纹(录入/打卡):\n"
    "录入指纹\n"
    "作用: 录入一枚指纹(按压3次)\n"
    "\n"
    "指纹打卡\n"
    "作用: 指纹打卡签到(比对已录入指纹)\n";

void lvgl_ui_page4_init(void)
{
    g_page4 = lv_obj_create(NULL);
    lv_obj_set_size(g_page4, P4_W, P4_H);
    ui_apply_bg_gradient(g_page4);
    lv_obj_set_style_border_width(g_page4, 0, 0);

    /* ---- 标题栏 ---- */
    lv_obj_t *tbar = lv_obj_create(g_page4);
    lv_obj_set_size(tbar, P4_W, P4_TITLE_H);
    lv_obj_set_pos(tbar, 0, 0);
    lv_obj_set_style_bg_color(tbar, lv_color_hex(C_TITLE_BG), 0);
    lv_obj_set_style_border_width(tbar, 0, 0);
    lv_obj_set_style_radius(tbar, 0, 0);
    lv_obj_set_style_pad_all(tbar, 0, 0);
    lv_obj_clear_flag(tbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(tbar);
    lv_label_set_text(title, "Voice Commands");
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(title, &bangers_28, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, 0);

    /* ---- 可滚动正文 ---- */
    lv_obj_t *cont = lv_obj_create(g_page4);
    lv_obj_set_size(cont, P4_W - 2 * X0, P4_H - P4_TITLE_H - 8);
    lv_obj_set_pos(cont, X0, P4_TITLE_H + 4);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x050508), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cont, 1, 0);
    lv_obj_set_style_border_color(cont, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_pad_all(cont, 10, 0);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);

    lv_obj_t *lbl = lv_label_create(cont);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl, P4_TEXT);
    lv_obj_set_width(lbl, lv_pct(100));        /* 约束宽度以触发换行 */
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(lbl, &myChineseFont, 0);
}

lv_obj_t *lvgl_ui_page4_get_screen(void)
{
    return g_page4;
}
