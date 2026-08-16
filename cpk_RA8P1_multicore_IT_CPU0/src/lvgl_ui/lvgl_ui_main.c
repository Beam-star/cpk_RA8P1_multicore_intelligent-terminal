/**
 ******************************************************************************
 * @file    lvgl_ui_main.c
 * @brief   Meeting Terminal UI �?icons, mascot, clean log (LVGL 9.3)
 *
 * Layout (384×600 on GLCDC layer 2 @ x=640):
 *   Title bar           y=0..43    (44px)  logo + "Meeting Terminal"
 *   Row 1 buttons       y=51..114  (64px)  [Enroll] [Check-in]
 *   Row 2 buttons       y=120..183 (64px)  [Start Rec] [Stop Rec]
 *   Clear DB            y=189..226 (38px)  full-width, subtle red
 *   Rec timer           y=232..252 (20px)  �?MM:SS
 *   Log + Mascot        y=260..556 (296px) split left/right
 *   Status bar           y=568..592 (24px)  �?status text
 ******************************************************************************
 */

#include "lvgl_ui_main.h"
#include "lvgl_ui_assets.h"
#include "lvgl_ui_anim.h"
#include "lvgl_ui_page2.h"
#include "lvgl_ui_page3.h"
#include "lvgl_ui_page4.h"
#include "lvgl_ui_ppt.h"
#include "lvgl_ui_tracking.h"
#include "myChineseFont.h"
#include "driver/esp32/esp32_uart.h"
#include "driver/ci1302/ci1302_uart.h"
#include "driver/zw111/zw111_fingerprint.h"
#include "driver/zw111/fp_name_db.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "lvgl.h"
#include "rm_lvgl_port.h"
#include "gt911.h"
#include "rpmsg_record_cpu0.h"
#include "ai_application/face_detection_task.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

/* ======================================================================== */
/*  Config                                                                    */
/* ======================================================================== */

#define UI_FONT_SM   (&lv_font_montserrat_16)
#define UI_FONT_MD   (&lv_font_montserrat_18)
#define UI_FONT_LG   (&lv_font_montserrat_20)

#define UI_W  384
#define UI_H  600

/* ---- Geometry ---- */
#define X0       8
#define BTN_W    180
#define BTN_H    48
#define BTN_GAP  8

/* ---- Colours (dark multi-stop gradient premium theme) ---- */
#define C_BG           0x0A2545         /* 弹窗等纯色背景（取渐变顶部色） */
#define C_TITLE_BAR    0x081A2E         /* 标题栏：更深的蓝黑            */
#define C_BTN          0x1E3350         /* 按钮：深蓝灰                  */
#define C_BTN_ALT      0x1E4050         /* 按钮(备用)：深青              */
#define C_BTN_PRESS    0x3A5A80         /* 按下：亮蓝                    */
#define C_ACCENT       0x4FC3F7         /* 亮蓝                          */
#define C_GREEN        0x66BB6A         /* 亮绿                          */
#define C_RED          0xEF5350         /* 亮红                          */
#define C_ORANGE       0xFFB74D         /* 亮橙                          */
#define C_TEXT         0xFFFFFF         /* 纯白（最醒目）                */
#define C_TEXT_DIM     0xBDC9D4         /* 灰白                          */
#define C_LOG_BG       0x081A2E         /* 日志背景：深蓝黑              */

/* ======================================================================== */
/*  Static widgets                                                           */
/* ======================================================================== */

static lv_obj_t *g_log_area    = NULL;
static lv_obj_t *g_rec_timer   = NULL;
static lv_obj_t *g_status_bar  = NULL;

static volatile bool     g_recording      = false;

/* ---- Mode toggle (face ↔ hand detection) ---- */
static lv_obj_t *g_mode_label = NULL;
static lv_obj_t *g_ppt_label  = NULL;   /* PPT 模式开关标签 */

/* ---- 追踪开关（人脸 / 声源） ---- */
static lv_obj_t *g_face_track_label  = NULL;
static lv_obj_t *g_sound_track_label = NULL;

/* ---- ESP32 语音转文字 / AI 对话 字幕 ---- */
static lv_obj_t *g_subtitle  = NULL;
static lv_obj_t *g_stt_label = NULL;
static lv_obj_t *g_ai_label  = NULL;
static bool       g_stt_on    = false;
static bool       g_ai_on     = false;

/* ---- 指纹录入 / 打卡 ---- */
static lv_obj_t *g_fp_status_label  = NULL;   /* 打卡状态提示行 */

static bool  g_fp_checked_in    = false;      /* 已打卡（对应一次会议记录） */
static char  g_fp_participant[32];            /* 打卡后输入的人员名字 */
static bool  g_notes_opened     = false;      /* 会议记录 txt 是否已打开 */
static bool  g_notes_name_written = false;    /* 首行「参会人员有:」是否已写入 */

/* 打卡完成回调（worker 任务上下文）→ 暂存 → defer 到 LVGL 任务 */
static zw111_op_t  g_fp_op;
static int         g_fp_status;
static uint16_t    g_fp_page;
static uint16_t    g_fp_score;
static uint16_t    g_fp_pending_id = 0;   /* 录入成功后待绑名字的指纹 ID */

/* 本次上电周期内已打卡的名字（去重：同一个人可能录入多枚指纹） */
#define FP_MAX_PARTICIPANTS  32
static char g_checked_names[FP_MAX_PARTICIPANTS][FP_NAME_LEN];
static int  g_checked_count = 0;

/* 名字键盘弹窗 */
static lv_obj_t *g_fp_popup    = NULL;
static lv_obj_t *g_fp_name_ta  = NULL;

/* 清空指纹库确认弹窗 */
static lv_obj_t *g_fp_clear_popup = NULL;

static void btn_mode_toggle_cb(lv_event_t *e);
static void fp_done_cb(zw111_op_t op, const zw111_result_t *result);
static void fp_done_async(void *p);

/* ======================================================================== */
/*  Helpers                                                                   */
/* ======================================================================== */

static void set_radius(lv_obj_t *o, int r)
{
    lv_obj_set_style_radius(o, r, 0);
}

static void btn_base(lv_obj_t *b, uint32_t bg, uint32_t border)
{
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_BTN_PRESS), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(border), 0);
    set_radius(b, 14);
    lv_obj_set_style_pad_all(b, 4, 0);
}

/* ======================================================================== */
/*  Button factory — plain text button (no icon)                             */
/* ======================================================================== */

static lv_obj_t *make_text_btn(lv_obj_t *p, const char *text, uint32_t bg,
                               uint32_t border, lv_coord_t x, lv_coord_t y,
                               lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_size(b, BTN_W, BTN_H);
    lv_obj_set_pos(b, x, y);
    btn_base(b, bg, border);

    lv_obj_t *lbl = lv_label_create(b);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(lbl, &righteous_20, 0);
    lv_obj_center(lbl);

    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

/* ======================================================================== */
/*  Touch + gesture                                                           */
/* ======================================================================== */

static int32_t  g_touch_down_x = 0;
static int32_t  g_touch_down_y = 0;
static bool     g_touch_down   = false;
static bool     g_swipe_latch  = false;   /* latched after a swipe until finger lift */
#define SWIPE_THRESH 60   /* 滑动 �?0px 才算手势 */

/* Defer the page-switch gesture to the next lv_timer_handler pass.
 * Calling lv_screen_load() synchronously from inside the indev read callback
 * interrupts LVGL's in-flight input/scroll processing and can cause a
 * use-after-free HardFault when the file list is being scrolled. */
static void gesture_async(void *p)
{
    lvgl_ui_handle_gesture((bool)(uintptr_t)p);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    gt911_scan();
    if (g_gt911_touch.point_num > 0 && g_gt911_touch.points[0].pressed) {
        int32_t x = (int32_t)g_gt911_touch.points[0].x - 640;
        int32_t y = (int32_t)g_gt911_touch.points[0].y;

        /* After a swipe, suppress presses until the finger lifts, so the
         * still-down finger doesn't re-trigger a tap on the new screen. */
        if (g_swipe_latch) {
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }

        if (!g_touch_down) {
            g_touch_down   = true;
            g_touch_down_x = x;
            g_touch_down_y = y;
        } else {
            /* 检测水平滑动。要求水平位移明显大于垂直位移 (adx > 2*ady),
             * 否则垂直滚动列表时手指轻微横向漂移会被误判为翻页手势,
             * 在滚动中途触发换屏导致卡死跑飞。 */
            int32_t dx = x - g_touch_down_x;
            int32_t dy = y - g_touch_down_y;
            int32_t adx = dx < 0 ? -dx : dx;
            int32_t ady = dy < 0 ? -dy : dy;
            if (adx > ady * 2 && dx < -SWIPE_THRESH) {
                lv_async_call(gesture_async, (void *)(uintptr_t)true);
                g_swipe_latch = true;   /* 左滑 */
                g_touch_down = false;
                data->state  = LV_INDEV_STATE_RELEASED;
                return;
            } else if (adx > ady * 2 && dx > SWIPE_THRESH) {
                lv_async_call(gesture_async, (void *)(uintptr_t)false);
                g_swipe_latch = true;  /* 右滑 */
                g_touch_down = false;
                data->state  = LV_INDEV_STATE_RELEASED;
                return;
            }
        }
        data->state   = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
    } else {
        g_touch_down  = false;
        g_swipe_latch = false;
        data->state   = LV_INDEV_STATE_RELEASED;
    }
}

/* ======================================================================== */
/*  Async UI callbacks                                                        */
/* ======================================================================== */

static void async_log(void *p)
{
    if (!g_log_area) return;
    lv_textarea_add_text(g_log_area, (const char *)p);
    lv_textarea_add_text(g_log_area, "\n");
    lv_textarea_set_cursor_pos(g_log_area, LV_TEXTAREA_CURSOR_LAST);
}

static void async_rec_time(void *p)
{
    uint32_t sec = (uint32_t)(uintptr_t)p;
    if (g_rec_timer) {
        char b[8];
        sprintf(b, "%02lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
        lv_label_set_text(g_rec_timer, b);
    }
}

static void async_status(void *p)
{
    if (g_status_bar) lv_label_set_text(g_status_bar, (const char *)p);
}

/* ======================================================================== */
/*  Button callbacks                                                          */
/* ======================================================================== */

static void mode_toggle_action(void)
{
    detection_mode_t m = face_detection_get_mode();
    detection_mode_t next = (m == DETECTION_MODE_HAND) ? DETECTION_MODE_FACE
                                                       : DETECTION_MODE_HAND;
    face_detection_set_mode(next);

    if (g_mode_label) {
        lv_label_set_text(g_mode_label,
                          (next == DETECTION_MODE_HAND) ? "Mode: Hand" : "Mode: Face");
    }

    /* 切回 Face 模式时自动关闭 PPT（PPT 只在 Hand 模式下有效）。
     * 退出 0x09 仍由手势任务在激活态下降沿发送，这里只同步开关与标签。 */
    if (next == DETECTION_MODE_FACE && lvgl_ui_ppt_is_on()) {
        lvgl_ui_ppt_set_on(false);
        if (g_ppt_label) lv_label_set_text(g_ppt_label, "PPT: Off");
        lvgl_ui_log("PPT mode off (left Hand mode)");
    }

    /* 切到 Hand 模式时自动关闭人脸追踪（人脸追踪只在 Face 模式下有效）。 */
    if (next == DETECTION_MODE_HAND && lvgl_ui_tracking_face_is_on()) {
        lvgl_ui_tracking_face_set_on(false);
        if (g_face_track_label) lv_label_set_text(g_face_track_label, "Face Track: Off");
        lvgl_ui_log("Face track off (left Face mode)");
    }

    lvgl_ui_log((next == DETECTION_MODE_HAND) ? "Hand mode" : "Face mode");
}

static void btn_mode_toggle_cb(lv_event_t *e)
{
    (void)e;
    mode_toggle_action();
    ci1302_uart_send(CI1302_CMD_MODE_SWITCH_V2);   /* 被动播报「模式已切换」 */
}

/* 弹窗提示 (录制被播放阻断) */
static void show_block_popup(const char *msg)
{
    lv_obj_t *mb = lv_msgbox_create(lv_screen_active());
    lv_msgbox_add_title(mb, "Notice");
    lv_msgbox_add_text(mb, msg);
    lv_msgbox_add_close_button(mb);
    lvgl_ui_log(msg);
}

static bool rec_start_action(void)
{
    /* 防抖/防重�? 已在录制中则忽略 (重复 START 会让 CPU1 �?ERROR,
     * UI 误判为录制停�? */
    if (g_recording) {
        lvgl_ui_log("Already recording");
        return false;
    }
    if (lvgl_ui_page2_is_playing()) {
        show_block_popup("Playback in progress, please stop it first");
        return false;
    }
    rpmsg_record_send_start(70, 5, 0);
    g_recording    = true;
    lvgl_ui_set_rec_time(0);
    lvgl_ui_log("Rec started");
    return true;
}

static bool rec_stop_action(void)
{
    rpmsg_record_send_stop();
    g_recording = false;
    lvgl_ui_log("Rec stopped");
    return true;
}

/* ---- ESP32 语音转文字 / AI 对话 ----
 * 注意：动作函数本身不发送 CI1302 被动播报。被动播报(FF 组)只在「UI 按钮」
 * 触发时由按钮回调补发；声控命令走主动播报(CI1302 自己说)，这里只执行动作。 */

static bool stt_set(bool on)
{
    if (on == g_stt_on) {
        lvgl_ui_log(on ? "ASR already on" : "ASR already off");
        return false;
    }
    /* 互斥：AI 开启时不能开启 ASR */
    if (on && g_ai_on) {
        show_block_popup("Please stop AI first");
        return false;
    }
    /* 互斥：PPT 开启时不能开启 ASR（ESP32 协议 0x01/0x03/0x06 互斥） */
    if (on && lvgl_ui_ppt_is_on()) {
        show_block_popup("Please stop PPT first");
        return false;
    }
    g_stt_on = on;
    if (on) {
        esp32_uart_send_cmd(ESP32_CMD_STT_ON);
        /* 打卡后才保存会议记录；首行「参会人员有:」在收到第一条字幕时才写，
         * 这样没有 ESP32 中文内容时 txt 保持为空，关闭时由 CPU1 丢弃不保存。 */
        if (g_fp_checked_in) {
            rpmsg_record_send_notes_open();
            g_notes_opened = true;
            g_notes_name_written = false;
            g_fp_checked_in = false;   /* 本次打卡对应一次会议记录，用掉 */
            if (g_fp_status_label) lv_label_set_text(g_fp_status_label, "Check-in: none");
            lvgl_ui_log("ASR on (will save)");
        } else {
            lvgl_ui_log("ASR on (no check-in, not saving)");
        }
        if (g_stt_label) lv_label_set_text(g_stt_label, "ASR: On");
    } else {
        esp32_uart_send_cmd(ESP32_CMD_STT_OFF);
        if (g_notes_opened) {
            rpmsg_record_send_notes_close();  /* 停止 ASR 并保存 txt 到 SD 卡（空文件会被 CPU1 丢弃） */
            g_notes_opened = false;
            g_notes_name_written = false;
            g_checked_count = 0;   /* 会议记录关闭：清空已打卡去重列表 */
        }
        if (g_stt_label) lv_label_set_text(g_stt_label, "ASR: Off");
        if (g_subtitle)  lv_label_set_text(g_subtitle, "");
    }
    return true;
}

static bool ai_set(bool on)
{
    if (on == g_ai_on) {
        lvgl_ui_log(on ? "AI already on" : "AI already off");
        return false;
    }
    /* 互斥：ASR 开启时不能开启 AI */
    if (on && g_stt_on) {
        show_block_popup("Please stop ASR first");
        return false;
    }
    /* 互斥：PPT 开启时不能开启 AI */
    if (on && lvgl_ui_ppt_is_on()) {
        show_block_popup("Please stop PPT first");
        return false;
    }
    g_ai_on = on;
    if (on) {
        esp32_uart_send_cmd(ESP32_CMD_AI_ON);
        if (g_ai_label) lv_label_set_text(g_ai_label, "AI: On");
    } else {
        esp32_uart_send_cmd(ESP32_CMD_AI_OFF);
        if (g_ai_label) lv_label_set_text(g_ai_label, "AI: Off");
        if (g_subtitle) lv_label_set_text(g_subtitle, "");
    }
    return true;
}

/* ---- PPT 翻页模式开关 ---- */

static bool ppt_set(bool on)
{
    if (on == lvgl_ui_ppt_is_on()) {
        lvgl_ui_log(on ? "PPT already on" : "PPT already off");
        return false;
    }

    if (!on) {
        /* 关闭 PPT：无需额外检查 */
        lvgl_ui_ppt_set_on(false);
        if (g_ppt_label) lv_label_set_text(g_ppt_label, "PPT: Off");
        lvgl_ui_log("PPT mode off");
        return true;
    }

    /* 开启 PPT：必须先切到 Hand 检测模式 */
    if (face_detection_get_mode() != DETECTION_MODE_HAND) {
        show_block_popup("Please switch to Hand mode first");
        return false;
    }
    /* 互斥：PPT 与 ASR/AI 主模式互斥（ESP32 协议 0x06 与 0x01/0x03 互斥） */
    if (g_stt_on || g_ai_on) {
        show_block_popup("Please stop ASR/AI first");
        return false;
    }

    lvgl_ui_ppt_set_on(true);
    if (g_ppt_label) lv_label_set_text(g_ppt_label, "PPT: On");
    /* 进入/退出 0x06/0x09 由手势任务在激活态边沿发送 */
    lvgl_ui_log("PPT mode on");
    return true;
}

/* ---- 人脸追踪 / 声源追踪 开关 ----
 * 二者互斥，只能开启一个；人脸追踪仅 Face 模式可用，声源追踪两种模式均可用。 */

static bool face_track_set(bool on)
{
    if (on == lvgl_ui_tracking_face_is_on()) {
        lvgl_ui_log(on ? "Face track already on" : "Face track already off");
        return false;
    }

    if (!on) {
        lvgl_ui_tracking_face_set_on(false);
        if (g_face_track_label) lv_label_set_text(g_face_track_label, "Face Track: Off");
        lvgl_ui_log("Face track off");
        return true;
    }

    /* 开启：必须先切到 Face 检测模式 */
    if (face_detection_get_mode() != DETECTION_MODE_FACE) {
        show_block_popup("Please switch to Face mode first");
        return false;
    }
    /* 互斥：与声源追踪 */
    if (lvgl_ui_tracking_sound_is_on()) {
        show_block_popup("Please stop sound tracking first");
        return false;
    }

    lvgl_ui_tracking_face_set_on(true);
    if (g_face_track_label) {
        lv_label_set_text(g_face_track_label, "Face Track: On");
    }
    lvgl_ui_log("Face track on (person1)");
    return true;
}

static bool sound_track_set(bool on)
{
    if (on == lvgl_ui_tracking_sound_is_on()) {
        lvgl_ui_log(on ? "Sound track already on" : "Sound track already off");
        return false;
    }

    if (!on) {
        lvgl_ui_tracking_sound_set_on(false);
        if (g_sound_track_label) lv_label_set_text(g_sound_track_label, "Sound Track: Off");
        lvgl_ui_log("Sound track off");
        return true;
    }

    /* 互斥：与人脸追踪 */
    if (lvgl_ui_tracking_face_is_on()) {
        show_block_popup("Please stop face tracking first");
        return false;
    }

    lvgl_ui_tracking_sound_set_on(true);
    if (g_sound_track_label) {
        lv_label_set_text(g_sound_track_label, "Sound Track: On");
    }
    lvgl_ui_log("Sound track on");
    return true;
}

/* 选择追踪人脸（语音命令）：先设目标编号，再确保追踪开启 */
static void face_track_select(uint8_t person_idx)
{
    lvgl_ui_tracking_set_target_person(person_idx);

    if (!lvgl_ui_tracking_face_is_on()) {
        face_track_set(true);
    } else {
        if (g_face_track_label) {
            char b[24];
            sprintf(b, "Face Track: p%d", (int)person_idx + 1);
            lv_label_set_text(g_face_track_label, b);
        }
        /* lvgl_ui_log 是异步的（只存指针不拷贝），用静态字符串避免栈变量悬空 */
        static const char *s_logs[3] = {
            "Tracking person1", "Tracking person2", "Tracking person3"
        };
        lvgl_ui_log(s_logs[person_idx]);
    }
}

/* 取消所有追踪（人脸 + 声源都关）。供「取消追踪」声控指令使用。 */
static void tracking_cancel_all(void)
{
    bool changed = false;

    if (lvgl_ui_tracking_face_is_on()) {
        lvgl_ui_tracking_face_set_on(false);
        if (g_face_track_label) lv_label_set_text(g_face_track_label, "Face Track: Off");
        changed = true;
    }
    if (lvgl_ui_tracking_sound_is_on()) {
        lvgl_ui_tracking_sound_set_on(false);
        if (g_sound_track_label) lv_label_set_text(g_sound_track_label, "Sound Track: Off");
        changed = true;
    }

    lvgl_ui_log(changed ? "Tracking off" : "Tracking already off");
}

/* ---- 指纹录入 / 打卡 ----
 * 录入/打卡是阻塞数秒的操作，由 zw111 驱动 worker 任务执行；这里只投递请求，
 * 结果经 fp_done_cb（worker 上下文）→ lv_async_call → fp_done_async（LVGL 任务）。 */

static void fp_log_fail(const char *what, int status)
{
    static char buf[64];
    if (status == ZW111_ERR_TRANSPORT) {
        sprintf(buf, "%s: no response", what);
    } else {
        sprintf(buf, "%s (%s)", what, zw111_confirm_str(status));
    }
    lvgl_ui_log(buf);
}

/* loading 弹窗右上角 ✕：取消正在进行的录入/打卡 */
static void fp_cancel_cb(void)
{
    zw111_fingerprint_cancel();
    lvgl_ui_log("Cancelled");
}

static bool fp_enroll_action(void)
{
    if (zw111_fingerprint_is_busy()) {
        lvgl_ui_log("Fingerprint busy");
        return false;
    }
    zw111_fingerprint_enroll();
    lvgl_ui_loading_show("Enrolling...", fp_cancel_cb);
    lvgl_ui_log("Enrolling fingerprint...");
    return true;
}

static bool fp_checkin_action(void)
{
    if (zw111_fingerprint_is_busy()) {
        lvgl_ui_log("Fingerprint busy");
        return false;
    }
    zw111_fingerprint_identify();
    lvgl_ui_loading_show("Checking in...", fp_cancel_cb);
    lvgl_ui_log("Checking in...");
    return true;
}

static bool fp_clear_action(void)
{
    if (zw111_fingerprint_is_busy()) {
        lvgl_ui_log("Fingerprint busy");
        return false;
    }
    zw111_fingerprint_clear();
    lvgl_ui_log("Clearing fingerprints...");
    return true;
}

/* 录入成功 → 名字键盘弹窗确认：把名字按录入 ID 入队，由 fp_name worker 写 flash */
static void fp_name_ready_cb(lv_event_t *e)
{
    (void)e;
    if (g_fp_name_ta) {
        const char *name = lv_textarea_get_text(g_fp_name_ta);
        if (name && name[0] != '\0') {
            static char buf[64];
            sprintf(buf, "Enrolled ID %u: %s", (unsigned)g_fp_pending_id, name);
            lvgl_ui_log(buf);
            fp_name_db_save(g_fp_pending_id, name);   /* 同步更新 RAM */
            zw111_fingerprint_flush_names();           /* 交给 zw111_fp worker 写 flash */
        } else {
            lvgl_ui_log("Name empty (fingerprint unnamed)");
        }
    }
    if (g_fp_popup) { lv_obj_delete(g_fp_popup); g_fp_popup = NULL; }
    g_fp_name_ta = NULL;
}

static void fp_name_cancel_cb(lv_event_t *e)
{
    (void)e;
    lvgl_ui_log("Name skipped (fingerprint unnamed)");
    if (g_fp_popup) { lv_obj_delete(g_fp_popup); g_fp_popup = NULL; }
    g_fp_name_ta = NULL;
}

static void fp_show_name_popup(void)
{
    lv_obj_t *popup = lv_obj_create(lv_screen_active());
    lv_obj_set_size(popup, 360, 440);
    lv_obj_align(popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(popup, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(popup, 2, 0);
    lv_obj_set_style_border_color(popup, lv_color_hex(C_ACCENT), 0);
    set_radius(popup, 8);

    lv_obj_t *title = lv_label_create(popup);
    lv_label_set_text(title, "Name this fingerprint");
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(title, UI_FONT_MD, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *ta = lv_textarea_create(popup);
    lv_obj_set_size(ta, 320, 44);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 36);
    lv_textarea_set_placeholder_text(ta, "Name (English)");
    lv_textarea_set_max_length(ta, 31);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_style_text_font(ta, UI_FONT_MD, 0);

    lv_obj_t *kb = lv_keyboard_create(popup);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_set_width(kb, 356);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -6);

    g_fp_popup   = popup;
    g_fp_name_ta = ta;

    lv_obj_add_event_cb(ta, fp_name_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(ta, fp_name_cancel_cb, LV_EVENT_CANCEL, NULL);
}

/* 清空指纹库确认弹窗（确认 / 取消） */
static void fp_clear_confirm_cb(lv_event_t *e)
{
    (void)e;
    if (g_fp_clear_popup) { lv_obj_delete(g_fp_clear_popup); g_fp_clear_popup = NULL; }
    fp_clear_action();
}

static void fp_clear_cancel_cb(lv_event_t *e)
{
    (void)e;
    if (g_fp_clear_popup) { lv_obj_delete(g_fp_clear_popup); g_fp_clear_popup = NULL; }
}

static void fp_show_clear_popup(void)
{
    lv_obj_t *popup = lv_obj_create(lv_screen_active());
    lv_obj_set_size(popup, 320, 170);
    lv_obj_align(popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(popup, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(popup, 2, 0);
    lv_obj_set_style_border_color(popup, lv_color_hex(C_RED), 0);
    set_radius(popup, 8);

    lv_obj_t *title = lv_label_create(popup);
    lv_label_set_text(title, "Clear All Fingerprints?");
    lv_obj_set_style_text_color(title, lv_color_hex(C_RED), 0);
    lv_obj_set_style_text_font(title, UI_FONT_MD, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *text = lv_label_create(popup);
    lv_label_set_text(text, "Delete all saved fingerprints?");
    lv_obj_set_style_text_color(text, lv_color_hex(C_TEXT_DIM), 0);
    lv_obj_set_style_text_font(text, UI_FONT_SM, 0);
    lv_obj_align(text, LV_ALIGN_TOP_MID, 0, 52);

    lv_obj_t *cancel_btn = lv_button_create(popup);
    lv_obj_set_size(cancel_btn, 120, 40);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 20, -16);
    btn_base(cancel_btn, C_BTN, C_ACCENT);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(cancel_lbl, UI_FONT_SM, 0);
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel_btn, fp_clear_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clear_btn = lv_button_create(popup);
    lv_obj_set_size(clear_btn, 120, 40);
    lv_obj_align(clear_btn, LV_ALIGN_BOTTOM_RIGHT, -20, -16);
    btn_base(clear_btn, 0x2A1215, C_RED);
    lv_obj_t *clear_lbl = lv_label_create(clear_btn);
    lv_label_set_text(clear_lbl, "Clear");
    lv_obj_set_style_text_color(clear_lbl, lv_color_hex(C_RED), 0);
    lv_obj_set_style_text_font(clear_lbl, UI_FONT_SM, 0);
    lv_obj_center(clear_lbl);
    lv_obj_add_event_cb(clear_btn, fp_clear_confirm_cb, LV_EVENT_CLICKED, NULL);

    g_fp_clear_popup = popup;
}

/* 去重：名字是否在本上电周期内已经打过卡 */
static bool fp_name_already_checked(const char *name)
{
    for (int i = 0; i < g_checked_count; i++) {
        if (strcmp(g_checked_names[i], name) == 0) return true;
    }
    return false;
}

static void fp_name_mark_checked(const char *name)
{
    if (g_checked_count < FP_MAX_PARTICIPANTS) {
        strncpy(g_checked_names[g_checked_count], name, FP_NAME_LEN - 1);
        g_checked_names[g_checked_count][FP_NAME_LEN - 1] = '\0';
        g_checked_count++;
    }
}

/* worker 任务上下文 → 暂存结果并 defer 到 LVGL 任务 */
static void fp_done_cb(zw111_op_t op, const zw111_result_t *result)
{
    g_fp_op     = op;
    g_fp_status = result->status;
    g_fp_page   = result->page_id;
    g_fp_score  = result->score;
    lv_async_call(fp_done_async, NULL);
}

static void fp_done_async(void *p)
{
    (void)p;
    lvgl_ui_loading_hide();   /* 无论哪种操作完成，先收起 loading 动画 */
    if (g_fp_op == ZW111_OP_ENROLL) {
        if (g_fp_status == 0) {
            /* 录入成功：弹名字键盘，把名字与录入 ID 绑定保存到 W25Q256 */
            g_fp_pending_id = g_fp_page;
            lvgl_ui_log("Fingerprint enrolled, enter name");
            ci1302_uart_send(CI1302_CMD_FINGERPRINT_ENROLL_OK);   /* 播报「录入成功」 */
            fp_show_name_popup();
        } else {
            fp_log_fail("Enroll failed", g_fp_status);            /* 失败只打日志，不播报 */
        }
    } else if (g_fp_op == ZW111_OP_CLEAR) {
        if (g_fp_status == 0) {
            /* 指纹库已清空，清 RAM + 交给 zw111_fp worker 擦名字分区 */
            fp_name_db_clear();
            zw111_fingerprint_flush_names();
            lvgl_ui_log("Fingerprints cleared");                  /* 清库无语音播报，仅日志 */
        } else {
            fp_log_fail("Clear failed", g_fp_status);
        }
    } else { /* IDENTIFY */
        if (g_fp_status == 0) {
            /* 打卡成功：按 page_id 从名字库查出名字，不再弹键盘 */
            const char *name = fp_name_db_get(g_fp_page);
            if (name[0] != '\0') {
                /* 去重：同一个人可能录入多枚指纹，同名只打卡一次 */
                if (fp_name_already_checked(name)) {
                    lvgl_ui_log("Already checked in");
                    /* 不置 g_fp_checked_in，不播报「打卡成功」 */
                } else {
                    strncpy(g_fp_participant, name, sizeof(g_fp_participant) - 1);
                    g_fp_participant[sizeof(g_fp_participant) - 1] = '\0';
                    g_fp_checked_in = true;
                    fp_name_mark_checked(name);
                    if (g_fp_status_label) {
                        char buf[48];
                        sprintf(buf, "Check-in: %s", g_fp_participant);
                        lv_label_set_text(g_fp_status_label, buf);
                    }
                    lvgl_ui_log("Check-in OK");
                    ci1302_uart_send(CI1302_CMD_FINGERPRINT_CHECKIN_OK);  /* 播报「打卡成功」 */
                }
            } else {
                /* 指纹已录入但没存名字：不置打卡态（无名字无法关联会议记录） */
                static char buf[56];
                sprintf(buf, "Check-in page %u (no name)", (unsigned)g_fp_page);
                lvgl_ui_log(buf);
            }
        } else {
            fp_log_fail("Check-in failed", g_fp_status);          /* 失败只打日志，不播报 */
        }
    }
}

/* ---- 按钮回调（薄封装 + 被动播报，均在 LVGL 任务上下文执行） ---- */

static void btn_fp_enroll_cb(lv_event_t *e)
{
    (void)e;
    if (fp_enroll_action()) {
        ci1302_uart_send(CI1302_CMD_FINGERPRINT_ENROLL_V2);   /* 播报「录入指纹功能」 */
    }
}
static void btn_fp_checkin_cb(lv_event_t *e)
{
    (void)e;
    if (fp_checkin_action()) {
        ci1302_uart_send(CI1302_CMD_FINGERPRINT_CHECKIN_V2);  /* 播报「指纹打卡功能」 */
    }
}
static void btn_fp_clear_cb(lv_event_t *e)
{
    (void)e;
    fp_show_clear_popup();   /* 先弹确认框，确认后才真正清空 */
}

static void btn_stt_cb(lv_event_t *e)
{
    (void)e;
    bool on = !g_stt_on;
    if (stt_set(on)) {
        ci1302_uart_send(on ? CI1302_CMD_STT_ON_V2 : CI1302_CMD_STT_OFF_V2);
    }
}
static void btn_ai_cb(lv_event_t *e)
{
    (void)e;
    bool on = !g_ai_on;
    if (ai_set(on)) {
        ci1302_uart_send(on ? CI1302_CMD_AI_ON_V2 : CI1302_CMD_AI_OFF_V2);
    }
}
static void btn_ppt_cb(lv_event_t *e)
{
    (void)e;
    bool on = !lvgl_ui_ppt_is_on();
    if (ppt_set(on)) {
        ci1302_uart_send(on ? CI1302_CMD_PPT_ON_V2 : CI1302_CMD_PPT_OFF_V2);
    }
}
static void btn_rec_start_cb(lv_event_t *e)
{
    (void)e;
    if (rec_start_action()) {
        ci1302_uart_send(CI1302_CMD_REC_START_V2);
    }
}
static void btn_rec_stop_cb(lv_event_t *e)
{
    (void)e;
    if (rec_stop_action()) {
        ci1302_uart_send(CI1302_CMD_REC_STOP_V2);
    }
}
static void btn_face_track_cb(lv_event_t *e)
{
    (void)e;
    bool on = !lvgl_ui_tracking_face_is_on();
    if (face_track_set(on)) {
        /* 被动播报：开启/取消人脸追踪（FF 组） */
        ci1302_uart_send(on ? CI1302_CMD_TRACK_FACE_V2 : CI1302_CMD_TRACK_CANCEL_V2);
    }
}
static void btn_sound_track_cb(lv_event_t *e)
{
    (void)e;
    bool on = !lvgl_ui_tracking_sound_is_on();
    if (sound_track_set(on)) {
        /* 被动播报：开=开始声源追踪(0xFF60)，关=取消追踪(0xFF5F)。
         * 取消追踪的播报词与人脸追踪共用一个。 */
        ci1302_uart_send(on ? CI1302_CMD_SOUND_TRACK_V2 : CI1302_CMD_TRACK_CANCEL_V2);
    }
}

/* ---- CI1302 声控命令处理 ---- */

static void ci1302_action_async(void *p)
{
    ci1302_cmd_t cmd = (ci1302_cmd_t)(uintptr_t)p;

    switch (cmd) {
    case CI1302_CMD_REC_START:
    case CI1302_CMD_REC_START_V2:
        rec_start_action();
        break;
    case CI1302_CMD_REC_STOP:
    case CI1302_CMD_REC_STOP_V2:
        rec_stop_action();
        break;
    case CI1302_CMD_MODE_SWITCH:
    case CI1302_CMD_MODE_SWITCH_V2:
        mode_toggle_action();
        break;
    case CI1302_CMD_STT_ON:
    case CI1302_CMD_STT_ON_V2:
        stt_set(true);
        break;
    case CI1302_CMD_STT_OFF:
    case CI1302_CMD_STT_OFF_V2:
        stt_set(false);
        break;
    case CI1302_CMD_AI_ON:
    case CI1302_CMD_AI_ON_V2:
        ai_set(true);
        break;
    case CI1302_CMD_AI_OFF:
    case CI1302_CMD_AI_OFF_V2:
        ai_set(false);
        break;
    case CI1302_CMD_PPT_ON:
    case CI1302_CMD_PPT_ON_V2:
        ppt_set(true);
        break;
    case CI1302_CMD_PPT_OFF:
    case CI1302_CMD_PPT_OFF_V2:
        ppt_set(false);
        break;

    case CI1302_CMD_PLAY_SONG:
        /* 播放 SD 卡中预存的歌曲（复用 page2 音频播放路径 + 互斥检查） */
        lvgl_ui_page2_play_audio_path("/meeting/audio/ForgetTime.wav");
        break;

    /* 追踪人脸：默认追踪 person1；一号/二号/三号切换目标人脸；取消追踪关闭 */
    case CI1302_CMD_TRACK_FACE:
    case CI1302_CMD_TRACK_FACE_V2:
    case CI1302_CMD_TRACK_FACE_1:
        face_track_select(0);
        break;
    case CI1302_CMD_TRACK_FACE_2:
        face_track_select(1);
        break;
    case CI1302_CMD_TRACK_FACE_3:
        face_track_select(2);
        break;
    case CI1302_CMD_TRACK_CANCEL:
    case CI1302_CMD_TRACK_CANCEL_V2:
        /* 取消追踪：人脸追踪与声源追踪一起关 */
        tracking_cancel_all();
        break;

    case CI1302_CMD_SOUND_TRACK:
    case CI1302_CMD_SOUND_TRACK_V2:
        sound_track_set(true);
        break;

    /* 指纹录入 / 打卡（声控走 0x00 组主动播报；0xFF 组是被动播报，不会作为 RX 收到） */
    case CI1302_CMD_FINGERPRINT_ENROLL:
        fp_enroll_action();
        break;
    case CI1302_CMD_FINGERPRINT_CHECKIN:
        fp_checkin_action();
        break;

    /* 音量/播报等系统命令由 CI1302 内部自动处理，这里忽略 */
    default:
        printf("[CI1302] unhandled cmd 0x%04X\r\n", (unsigned)cmd);
        break;
    }
}

/* 解析任务上下文调用：defer 到 LVGL 任务执行 */
static void ci1302_cmd_cb(ci1302_cmd_t cmd)
{
    printf("[CI1302] RX cmd 0x%04X\r\n", (unsigned)cmd);
    lv_async_call(ci1302_action_async, (void *)(uintptr_t)cmd);
}

/* 字幕异步更新（在 LVGL 任务上下文执行） */
static void esp32_subtitle_async(void *p)
{
    if (g_subtitle) {
        lv_label_set_text(g_subtitle, (const char *)p);
    }
    lv_free(p);
}

/* 解析任务上下文调用：拷贝字符串后 defer 到 LVGL 任务 */
static void esp32_subtitle_cb(const char *line)
{
    /* ASR 模式下同步把字幕写入会议记录 (CPU1 → SD 卡)。
     * AI 对话模式不保存 (g_stt_on 与 g_ai_on 互斥)。 */
    if (g_stt_on) {
        /* 收到第一条字幕时先写首行「参会人员有: <名字>」，再写字幕内容。
         * 若全程没有字幕，txt 保持为空，关闭时被 CPU1 丢弃。 */
        if (g_notes_opened && !g_notes_name_written) {
            /* 会议通常有多个参会人员：把本次会议记录期间所有已打卡的名字都写进首行，
             * 而不是只写最后一次打卡的名字（g_fp_participant 仅用于状态行显示）。
             * g_checked_names[] 在 ASR 关闭时才清空，此处即本次会议的完整名单。 */
            rpmsg_record_send_notes_append("参会人员有: ", (uint32_t)strlen("参会人员有: "));
            for (int i = 0; i < g_checked_count; i++) {
                if (i > 0) {
                    rpmsg_record_send_notes_append(",", (uint32_t)strlen(","));
                }
                rpmsg_record_send_notes_append(g_checked_names[i],
                                               (uint32_t)strlen(g_checked_names[i]));
            }
            rpmsg_record_send_notes_append("\n", 1u);
            g_notes_name_written = true;
        }
        rpmsg_record_send_notes_append(line, (uint32_t)strlen(line));
        rpmsg_record_send_notes_append("\n", 1u);
    }

    size_t n = strlen(line);
    char *copy = lv_malloc(n + 1);
    if (!copy) return;
    strcpy(copy, line);
    lv_async_call(esp32_subtitle_async, copy);
}

/* ======================================================================== */
/*  UI construction                                                           */
/* ======================================================================== */

/* ---- 控制台面板翻页（左右按键切换两个子面板） ---- */
#define PANEL_Y   48
#define PANEL_H   180
static lv_obj_t *g_panel_meeting = NULL;
static lv_obj_t *g_panel_periph  = NULL;
static int       g_console_sub   = 0;   /* 0=会议控制, 1=智能外设 */
static lv_obj_t *g_console_indicator = NULL;  /* "1/2" 指示 */

/* 创建一个透明面板容器（覆盖在渐变背景上，整组显示/隐藏） */
static lv_obj_t *make_panel(lv_obj_t *scr)
{
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_set_pos(p, 0, PANEL_Y);
    lv_obj_set_size(p, UI_W, PANEL_H);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static void console_show(int sub)
{
    g_console_sub = sub;
    if (g_panel_meeting) {
        if (sub == 0) lv_obj_clear_flag(g_panel_meeting, LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag(g_panel_meeting, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_panel_periph) {
        if (sub == 1) lv_obj_clear_flag(g_panel_periph, LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag(g_panel_periph, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_console_indicator) {
        lv_label_set_text(g_console_indicator, sub == 0 ? "1/2" : "2/2");
    }
}

static void console_prev_cb(lv_event_t *e) { (void)e; console_show(0); }
static void console_next_cb(lv_event_t *e) { (void)e; console_show(1); }

static void lvgl_ui_build(void)
{
    lv_obj_t *scr = lv_screen_active();
    ui_apply_bg_gradient(scr);

    /* ---- Title bar ---- */
    lv_obj_t *tbar = lv_obj_create(scr);
    lv_obj_set_size(tbar, UI_W, 44);
    lv_obj_set_pos(tbar, 0, 0);
    lv_obj_set_style_bg_color(tbar, lv_color_hex(C_TITLE_BAR), 0);
    lv_obj_set_style_border_width(tbar, 0, 0);
    set_radius(tbar, 0);

    lv_obj_t *title = lv_label_create(tbar);
    lv_label_set_text(title, "Meeting Terminal");
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(title, &bangers_28, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, 0);

    /* ---- 标题栏右侧：控制台翻页按钮 [<] 1/2 [>] ---- */
    lv_obj_t *prev_btn = lv_button_create(tbar);
    lv_obj_set_size(prev_btn, 40, 32);
    lv_obj_align(prev_btn, LV_ALIGN_RIGHT_MID, -96, 0);
    btn_base(prev_btn, C_BTN, C_ACCENT);
    lv_obj_t *prev_lbl = lv_label_create(prev_btn);
    lv_label_set_text(prev_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(prev_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(prev_lbl, UI_FONT_SM, 0);
    lv_obj_center(prev_lbl);
    lv_obj_add_event_cb(prev_btn, console_prev_cb, LV_EVENT_CLICKED, NULL);

    g_console_indicator = lv_label_create(tbar);
    lv_label_set_text(g_console_indicator, "1/2");
    lv_obj_set_style_text_color(g_console_indicator, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(g_console_indicator, UI_FONT_SM, 0);
    lv_obj_align(g_console_indicator, LV_ALIGN_RIGHT_MID, -50, 0);

    lv_obj_t *next_btn = lv_button_create(tbar);
    lv_obj_set_size(next_btn, 40, 32);
    lv_obj_align(next_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    btn_base(next_btn, C_BTN, C_ACCENT);
    lv_obj_t *next_lbl = lv_label_create(next_btn);
    lv_label_set_text(next_lbl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(next_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(next_lbl, UI_FONT_SM, 0);
    lv_obj_center(next_lbl);
    lv_obj_add_event_cb(next_btn, console_next_cb, LV_EVENT_CLICKED, NULL);

    /* ================= 面板 1：会议控制 ================= */
    g_panel_meeting = make_panel(scr);

    lv_obj_t *mode_btn = lv_button_create(g_panel_meeting);
    lv_obj_set_size(mode_btn, BTN_W, BTN_H);
    lv_obj_set_pos(mode_btn, X0, 0);
    btn_base(mode_btn, C_BTN, C_ACCENT);
    g_mode_label = lv_label_create(mode_btn);
    lv_label_set_text(g_mode_label, "Mode: Face");
    lv_obj_set_style_text_color(g_mode_label, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(g_mode_label, &cinzel_20, 0);
    lv_obj_center(g_mode_label);
    lv_obj_add_event_cb(mode_btn, btn_mode_toggle_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ppt_btn = lv_button_create(g_panel_meeting);
    lv_obj_set_size(ppt_btn, BTN_W, BTN_H);
    lv_obj_set_pos(ppt_btn, X0 + BTN_W + BTN_GAP, 0);
    btn_base(ppt_btn, C_BTN, C_ORANGE);
    g_ppt_label = lv_label_create(ppt_btn);
    lv_label_set_text(g_ppt_label, "PPT: Off");
    lv_obj_set_style_text_color(g_ppt_label, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(g_ppt_label, &cinzel_20, 0);
    lv_obj_center(g_ppt_label);
    lv_obj_add_event_cb(ppt_btn, btn_ppt_cb, LV_EVENT_CLICKED, NULL);

    make_text_btn(g_panel_meeting, "Start Rec", C_BTN_ALT, C_ORANGE, X0, 52, btn_rec_start_cb);
    make_text_btn(g_panel_meeting, "Stop Rec",  C_BTN_ALT, C_RED,    X0 + BTN_W + BTN_GAP, 52, btn_rec_stop_cb);

    lv_obj_t *stt_btn = lv_button_create(g_panel_meeting);
    lv_obj_set_size(stt_btn, BTN_W, BTN_H);
    lv_obj_set_pos(stt_btn, X0, 104);
    btn_base(stt_btn, C_BTN, C_ACCENT);
    g_stt_label = lv_label_create(stt_btn);
    lv_label_set_text(g_stt_label, "ASR: Off");
    lv_obj_set_style_text_color(g_stt_label, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(g_stt_label, &cinzel_20, 0);
    lv_obj_center(g_stt_label);
    lv_obj_add_event_cb(stt_btn, btn_stt_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ai_btn = lv_button_create(g_panel_meeting);
    lv_obj_set_size(ai_btn, BTN_W, BTN_H);
    lv_obj_set_pos(ai_btn, X0 + BTN_W + BTN_GAP, 104);
    btn_base(ai_btn, C_BTN, C_ACCENT);
    g_ai_label = lv_label_create(ai_btn);
    lv_label_set_text(g_ai_label, "AI: Off");
    lv_obj_set_style_text_color(g_ai_label, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(g_ai_label, &cinzel_20, 0);
    lv_obj_center(g_ai_label);
    lv_obj_add_event_cb(ai_btn, btn_ai_cb, LV_EVENT_CLICKED, NULL);

    /* ================= 面板 2：智能外设 ================= */
    g_panel_periph = make_panel(scr);

    lv_obj_t *ftrack_btn = lv_button_create(g_panel_periph);
    lv_obj_set_size(ftrack_btn, BTN_W, BTN_H);
    lv_obj_set_pos(ftrack_btn, X0, 0);
    btn_base(ftrack_btn, C_BTN, C_GREEN);
    g_face_track_label = lv_label_create(ftrack_btn);
    lv_label_set_text(g_face_track_label, "Face Track: Off");
    lv_obj_set_style_text_color(g_face_track_label, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(g_face_track_label, &righteous_20, 0);
    lv_obj_center(g_face_track_label);
    lv_obj_add_event_cb(ftrack_btn, btn_face_track_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *strack_btn = lv_button_create(g_panel_periph);
    lv_obj_set_size(strack_btn, BTN_W, BTN_H);
    lv_obj_set_pos(strack_btn, X0 + BTN_W + BTN_GAP, 0);
    btn_base(strack_btn, C_BTN, C_ACCENT);
    g_sound_track_label = lv_label_create(strack_btn);
    lv_label_set_text(g_sound_track_label, "Sound Track: Off");
    lv_obj_set_style_text_color(g_sound_track_label, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(g_sound_track_label, &righteous_20, 0);
    lv_obj_center(g_sound_track_label);
    lv_obj_add_event_cb(strack_btn, btn_sound_track_cb, LV_EVENT_CLICKED, NULL);

    make_text_btn(g_panel_periph, "Enroll FP", C_BTN, C_GREEN,  X0, 52, btn_fp_enroll_cb);
    make_text_btn(g_panel_periph, "Check-in",  C_BTN, C_ACCENT, X0 + BTN_W + BTN_GAP, 52, btn_fp_checkin_cb);

    lv_obj_t *clear_btn = lv_button_create(g_panel_periph);
    lv_obj_set_size(clear_btn, UI_W - 2 * X0, 48);
    lv_obj_set_pos(clear_btn, X0, 104);
    btn_base(clear_btn, 0x2A1215, C_RED);
    lv_obj_t *clear_lbl = lv_label_create(clear_btn);
    lv_label_set_text(clear_lbl, "Clear All Fingerprints");
    lv_obj_set_style_text_color(clear_lbl, lv_color_hex(C_RED), 0);
    lv_obj_set_style_text_font(clear_lbl, &righteous_20, 0);
    lv_obj_center(clear_lbl);
    lv_obj_add_event_cb(clear_btn, btn_fp_clear_cb, LV_EVENT_CLICKED, NULL);

    g_fp_status_label = lv_label_create(g_panel_periph);
    lv_label_set_text(g_fp_status_label, "Check-in: none");
    lv_obj_set_pos(g_fp_status_label, X0, 156);
    lv_obj_set_style_text_color(g_fp_status_label, lv_color_hex(C_TEXT_DIM), 0);
    lv_obj_set_style_text_font(g_fp_status_label, UI_FONT_MD, 0);

    /* 默认显示会议控制面板 */
    console_show(0);

    /* ================= 共享底部：计时 + 字幕 + 日志 + 小人 ================= */
    int y4 = PANEL_Y + PANEL_H + 8;
    lv_obj_t *dot = lv_obj_create(scr);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_pos(dot, X0, y4 + 7);
    lv_obj_set_style_bg_color(dot, lv_color_hex(C_RED), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    set_radius(dot, 4);

    g_rec_timer = lv_label_create(scr);
    lv_label_set_text(g_rec_timer, "--:--");
    lv_obj_set_pos(g_rec_timer, X0 + 14, y4 + 2);
    lv_obj_set_style_text_color(g_rec_timer, lv_color_hex(C_TEXT_DIM), 0);
    lv_obj_set_style_text_font(g_rec_timer, UI_FONT_SM, 0);

    /* ---- Subtitle (中文字幕) ---- */
    int y5 = y4 + 20;
    g_subtitle = lv_label_create(scr);
    lv_obj_set_width(g_subtitle, UI_W - 2 * X0);
    lv_obj_set_pos(g_subtitle, X0, y5);
    lv_label_set_long_mode(g_subtitle, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_subtitle, "");
    lv_obj_set_style_text_color(g_subtitle, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(g_subtitle, &myChineseFont, 0);

    /* ---- Log + Mascot split ---- */
    int y6 = y5 + 60;
    int split_h = UI_H - y6 - 32;

    /* Log (left, ~220px) */
    g_log_area = lv_textarea_create(scr);
    lv_obj_set_size(g_log_area, 230, split_h);
    lv_obj_set_pos(g_log_area, X0, y6);
    lv_obj_set_style_bg_color(g_log_area, lv_color_hex(C_LOG_BG), 0);
    lv_obj_set_style_bg_opa(g_log_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_log_area, 3, 0);
    lv_obj_set_style_border_color(g_log_area, lv_color_hex(C_BTN), 0);
    set_radius(g_log_area, 6);
    lv_obj_set_style_pad_all(g_log_area, 6, 0);
    lv_obj_set_style_text_color(g_log_area, lv_color_hex(C_TEXT_DIM), 0);
    lv_obj_set_style_text_font(g_log_area, UI_FONT_SM, 0);
    lv_obj_remove_flag(g_log_area, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_textarea_set_text(g_log_area, "");

        /* Mascot (animated line dog, 130x130, slow ~4 fps loop) */
    {
        int sc_w = 130, sc_h = 130;
        int mx = UI_W - sc_w - X0;
        int my = y6 + (split_h - sc_h) / 2;
        lvgl_ui_mascot_start(scr, mx, my);
    }


    /* ---- Status bar ---- */
    int y7 = UI_H - 24;
    g_status_bar = lv_label_create(scr);
    lv_label_set_text(g_status_bar, LV_SYMBOL_OK " Ready");
    lv_obj_set_pos(g_status_bar, X0, y7);
    lv_obj_set_style_text_color(g_status_bar, lv_color_hex(C_GREEN), 0);
    lv_obj_set_style_text_font(g_status_bar, UI_FONT_SM, 0);
}

/* ======================================================================== */
/*  LVGL task                                                                 */
/* ======================================================================== */

static void lvgl_task(void *pvParameters)
{
    (void)pvParameters;
    while (1) {
        uint32_t delay = lv_timer_handler();
        if (delay < 1)  delay = 1;
        if (delay > 10) delay = 10;

        /* 录制计时不再本地估算 —�?CPU1 每秒回报真实采集时长
         * (REC_EVT_STATUS �?lvgl_ui_set_rec_time), 单一数据源不跳变 */
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

/* ======================================================================== */
/*  Public API                                                                */
/* ======================================================================== */

void lvgl_ui_init(void)
{
    /*
     * Load image assets from W25Q256 XIP �?SDRAM.
     * LVGL reads pixel data from SDRAM during rendering �?avoids any
     * D-Cache / XIP coherency issues that could hang the draw engine.
     */
    lvgl_ui_assets_load();  /* no-op — 静态小人已由 lvgl_ui_anim 动图替换 */
    /* 预载动图帧到 SDRAM（必须在 OSPI_B 被 AI 模型加载器 w25q256_close 之前） */
    lvgl_ui_anim_init();
    printf("[UI] assets loaded, creating indev...\r\n");

    lv_indev_t *indev = lv_indev_create();
    printf("[UI] indev created, setting type...\r\n");
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    printf("[UI] indev ready, building UI...\r\n");

    lvgl_ui_build();
    printf("[UI] UI built, init page2...\r\n");

    /* 初始化第二页 (文件浏览) */
    lvgl_ui_page2_init(lv_screen_active());

    /* 初始化第三页 (声源定位雷达)。
     * 测试/真实模式由 micarray_init() 依据 MICARRAY_TEST_MODE 选择。 */
    lvgl_ui_page3_init();

    /* 初始化第四页 (语音命令词说明)。 */
    lvgl_ui_page4_init();

    /* 注册 ESP32 字幕回调（UART2 收到一行中文时更新字幕标签） */
    esp32_uart_set_line_cb(esp32_subtitle_cb);

    /* 注册 CI1302 声控命令回调（UART0 收到命令帧 → 声控触发 UI 功能） */
    ci1302_uart_set_cmd_cb(ci1302_cmd_cb);

    /* 注册指纹录入/打卡完成回调（worker 任务上下文 → defer 到 LVGL 任务） */
    zw111_fingerprint_set_done_cb(fp_done_cb);

    /* 启动 PPT 翻页手势任务（PPT 开关 + Hand 模式都满足才发命令） */
    lvgl_ui_ppt_init();

    /* 启动舵机追踪任务（任务内完成舵机初始化，人脸/声源追踪互斥） */
    lvgl_ui_tracking_init();

    printf("[UI] page2/page3 done, creating lvgl task...\r\n");

    /* 栈从 4096 加到 12288 (48KB): 第二页文件列表滚动 + D/AVE 2D 渲染栈开销大,
     * 栈溢出 (configCHECK_FOR_STACK_OVERFLOW=0 检测不到) 会随机 HardFault。 */
    if (xTaskCreate(lvgl_task, "lvgl", 12288, NULL, 3, NULL) != pdPASS) return;
    printf("[UI] lvgl task created, delaying...\r\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    lvgl_ui_log("Ready");
    printf("[UI] init complete\r\n");
}

/* ---- Cross-task API ---- */

void lvgl_ui_log(const char *msg)    { lv_async_call(async_log, (void *)msg); }
void lvgl_ui_set_status(const char *s) { lv_async_call(async_status, (void *)s); }

void lvgl_ui_set_rec_time(uint32_t sec)
{
    lv_async_call(async_rec_time, (void *)(uintptr_t)sec);
}

void lvgl_ui_recording_started(void)
{
    g_recording = true;
}

void lvgl_ui_recording_stopped(void)
{
    g_recording = false;
}

bool lvgl_ui_is_recording(void)
{
    return g_recording;
}
