/**
 * @file    rpmsg_record_cpu0.c
 * @brief   CPU0-side record-control RPMsg channel.
 *
 * Pattern copied from rpmsg_bulk_test.c / rpmsg_log_cpu0.c.
 *
 * Architecture:
 *   - send endpoint: created once, used by UI button callbacks (best-effort,
 *     RL_DONT_BLOCK so the LVGL callback never stalls).
 *   - receive endpoint + queue: a small FreeRTOS task loops on status
 *     events from CPU1 and updates the LVGL UI via lv_async_call.
 */

#include "rpmsg_record_cpu0.h"
#include "rpmsg_record.h"
#include "rpmsg_core.h"
#include "rpmsg_lite.h"
#include "rpmsg_queue.h"
#include "lvgl_ui/lvgl_ui_main.h"
#include "lvgl_ui/lvgl_ui_page2.h"
#include "lvgl_ui/lvgl_ui_textview.h"
#include "ai_application/face_detection_task.h"
#include "mipi_camera_lcd.h"
#include "player/video_play_display.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>

/* ---- Task config ---- */
#define REC_STATUS_TASK_STACK  2048
#define REC_STATUS_TASK_PRIO   1       /* low — only receives status pings  */

/* ---- Static state ---- */
static struct rpmsg_lite_instance *g_inst  = NULL;
static struct rpmsg_lite_endpoint *g_ept   = NULL;
static rpmsg_queue_handle          g_queue = NULL;

/* ---- LIST chunk accumulator (rec_status_task context only) ---- */
static char     s_acc[FILE_LIST_MAX][32];
static uint32_t s_acc_tag = REC_LIST_TAG_AUDIO;

static void refresh_page2_async(void *p)
{
    (void)p;
    extern void lvgl_ui_page2_refresh(void);
    lvgl_ui_page2_refresh();
}

/* ---- Meeting-record notes read-back accumulation (rec_status_task ctx) ---- */
#define NOTES_TEXT_MAX  8192     /* max displayed txt bytes (safe for LVGL)  */
static char     s_notes_text[NOTES_TEXT_MAX];
static char     s_notes_read_name[32];
static uint32_t s_notes_text_len = 0;

/* Runs in LVGL task context (queued on REC_EVT_NOTES_END). */
static void show_notes_viewer_async(void *p)
{
    (void)p;
    lvgl_ui_textview_open(s_notes_read_name, s_notes_text);
}

/* Accumulate one EVT_LIST chunk; returns true when the list is complete. */
static bool accumulate_list_chunk(const rec_stat_msg_t *msg)
{
    if (msg->start_index == 0U) {          /* first chunk: reset accumulator */
        memset(s_acc, 0, sizeof(s_acc));
        s_acc_tag = msg->state;
    }
    for (uint32_t i = 0; i < msg->name_count; i++) {
        uint32_t idx = msg->start_index + i;
        if (idx >= FILE_LIST_MAX) break;
        strncpy(s_acc[idx], msg->names[i], 31);
        s_acc[idx][31] = '\0';
    }
    return (msg->start_index + msg->name_count >= msg->count);
}

/* ---- Status receive task ---- */
static void rec_status_task(void *pvParams)
{
    (void)pvParams;
    rec_stat_msg_t msg;
    uint32_t len, src;

    printf("[REC-CPU0] Status task running\r\n");

    while (1) {
        memset(&msg, 0, sizeof(msg));
        int st = rpmsg_queue_recv(g_inst, g_queue, &src,
                                  (char *)&msg, sizeof(msg), &len, RL_BLOCK);
        if (st != RL_SUCCESS) continue;

        switch (msg.event) {
        case REC_EVT_STARTED:
            printf("[REC-CPU0] Recording started\r\n");
            lvgl_ui_recording_started();
            lvgl_ui_log("Rec started");
            mipi_camera_lcd_set_video_record(true);   /* 开始抓带框视频帧 */
            face_detection_set_infer_interval(INFER_EVERY_N_RECORD);     /* 释放 SDRAM 带宽给 MJPEG 编码 */
            break;
        case REC_EVT_STOPPED:
            printf("[REC-CPU0] Stopped: %lu sec\r\n",
                   (unsigned long)msg.duration_sec);
            mipi_camera_lcd_set_video_record(false);
            lvgl_ui_recording_stopped();
            lvgl_ui_set_rec_time(msg.duration_sec);
            lvgl_ui_set_status("Ready");
            face_detection_set_infer_interval(INFER_EVERY_N_PREVIEW);     /* 恢复实时预览帧率 */
            break;
        case REC_EVT_STATUS:
            lvgl_ui_set_rec_time(msg.duration_sec);
            break;
        case REC_EVT_ERROR:
            printf("[REC-CPU0] ERROR (state=%lu)\r\n", (unsigned long)msg.state);
            /* state: 2=播放忙, 3=播放中拒绝录制, 4=录制中拒绝播放, 其它=录制失败 */
            if (msg.state == 4) {
                /* 录制仍在进行, 不要打断 */
                lvgl_ui_log("Busy: recording");
            } else {
                mipi_camera_lcd_set_video_record(false);
                lvgl_ui_recording_stopped();
                lvgl_ui_log(msg.state == 2 ? "Player busy" :
                            msg.state == 3 ? "Busy: playing" : "Rec error");
                face_detection_set_infer_interval(INFER_EVERY_N_PREVIEW);   /* 录制未启动, 恢复预览 */
            }
            lvgl_ui_set_status("Error");
            break;
        case REC_EVT_PLAY_DONE:
            printf("[REC-CPU0] Playback done (ok=%lu)\r\n",
                   (unsigned long)(msg.state == 0));
            lvgl_ui_page2_set_playing(false);
            break;
        case REC_EVT_VIDEO_PLAY_DONE:
            printf("[REC-CPU0] Video playback done (ok=%lu)\r\n",
                   (unsigned long)(msg.state == 0));
            video_play_display_stop();
            lvgl_ui_page2_set_playing(false);
            face_detection_set_infer_interval(INFER_EVERY_N_PREVIEW);     /* 恢复实时预览帧率 */
            break;
        case REC_EVT_PLAY_PROGRESS:
            lvgl_ui_page2_set_progress(msg.count, msg.duration_sec);
            break;
        case REC_EVT_VIDEO_SAVED:
            printf("[REC-CPU0] Video saved (%lu frames)\r\n",
                   (unsigned long)msg.count);
            lvgl_ui_log("Save successfully");
            lvgl_ui_set_status("Saved");
            break;
        case REC_EVT_LIST:
            if (accumulate_list_chunk(&msg)) {
                uint32_t total = (msg.count < FILE_LIST_MAX)
                               ? msg.count : FILE_LIST_MAX;
                printf("[REC-CPU0] File list (tag=%lu): %lu files\r\n",
                       (unsigned long)s_acc_tag, (unsigned long)total);
                lvgl_ui_page2_on_list(s_acc_tag, s_acc, (int)total);
                lv_async_call(refresh_page2_async, NULL);
            }
            break;
        case REC_EVT_NOTES_OPENED:
            printf("[REC-CPU0] Notes opened: %s\r\n", msg.names[0]);
            lvgl_ui_log("Notes started");
            break;
        case REC_EVT_NOTES_SAVED:
            printf("[REC-CPU0] Notes saved: %s\r\n", msg.names[0]);
            lvgl_ui_log("Notes saved");
            lv_async_call(refresh_page2_async, NULL);   /* new txt in list */
            break;
        case REC_EVT_NOTES_ERROR:
            printf("[REC-CPU0] Notes error (state=%lu)\r\n",
                   (unsigned long)msg.state);
            /* state=5: 无字幕数据, 空文件已丢弃 */
            lvgl_ui_log(msg.state == 5 ? "Notes empty (not saved)" : "Notes error");
            break;
        case REC_EVT_NOTES_DATA: {
            /* Raw UTF-8 bytes reassembled contiguously; NUL added at END. */
            uint32_t n = msg.name_count;
            if (n > sizeof(msg.names)) n = sizeof(msg.names);
            if (msg.start_index == 0) s_notes_text_len = 0;
            if (msg.start_index + n > NOTES_TEXT_MAX - 1) {
                if (msg.start_index >= NOTES_TEXT_MAX - 1) n = 0;
                else n = NOTES_TEXT_MAX - 1 - msg.start_index;
            }
            if (n > 0) {
                memcpy(s_notes_text + msg.start_index, &msg.names[0][0], n);
                s_notes_text_len = msg.start_index + n;
            }
            break;
        }
        case REC_EVT_NOTES_END:
            if (s_notes_text_len >= NOTES_TEXT_MAX) s_notes_text_len = NOTES_TEXT_MAX - 1;
            s_notes_text[s_notes_text_len] = '\0';
            lv_async_call(show_notes_viewer_async, NULL);
            break;
        default:
            break;
        }
    }
}

/* ---- Public API ---- */

bool rpmsg_record_cpu0_init(void)
{
    g_inst = (struct rpmsg_lite_instance *)rpmsg_core_get_instance();
    if (!g_inst) {
        printf("[REC-CPU0] RPMsg instance not available\r\n");
        return false;
    }

    /* Receive queue + endpoint (also used as the send source endpoint) */
    g_queue = rpmsg_queue_create(g_inst);
    if (!g_queue) return false;
    g_ept = rpmsg_lite_create_ept(g_inst, REC_EPT_CPU0,
                                  rpmsg_queue_rx_cb, g_queue);
    if (!g_ept) return false;

    /* Status receive task */
    BaseType_t ret = xTaskCreate(rec_status_task, "rec_stat",
                                 REC_STATUS_TASK_STACK, NULL,
                                 REC_STATUS_TASK_PRIO, NULL);
    if (ret != pdPASS) {
        printf("[REC-CPU0] Failed to create status task\r\n");
        return false;
    }

    printf("[REC-CPU0] Channel ready (ept=%u)\r\n", REC_EPT_CPU0);
    return true;
}

/*
 * Send via the persistent endpoint. Creating a temporary endpoint at
 * REC_EPT_CPU0 would fail: rpmsg_lite_create_ept() returns NULL for an
 * address that is already in use (our RX endpoint owns it).
 * All senders run in the LVGL task context, so no lock is needed here.
 */
static int do_send(const rec_ctrl_msg_t *msg)
{
    if (!g_ept) return -1;
    return rpmsg_lite_send(g_inst, g_ept, REC_EPT_CPU1,
                           (char *)msg, sizeof(*msg), RL_DONT_BLOCK);
}

/* Variable-length send for notes messages (12-byte header + payload). */
static int do_send_notes(const rec_notes_msg_t *msg, uint32_t len)
{
    if (!g_ept) return -1;
    return rpmsg_lite_send(g_inst, g_ept, REC_EPT_CPU1,
                           (char *)msg, len, RL_DONT_BLOCK);
}

bool rpmsg_record_send_start(uint8_t quality, uint8_t fps, uint16_t max_dur_sec)
{
    if (!g_inst) {
        printf("[REC-CPU0] RPMsg not ready\r\n");
        return false;
    }

    rec_ctrl_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.command = REC_CMD_START;
    msg.param1  = quality;
    msg.param2  = max_dur_sec;

    int st = do_send(&msg);
    printf("[REC-CPU0] SEND START ret=%d\r\n", st);
    return (st == RL_SUCCESS);
}

bool rpmsg_record_send_stop(void)
{
    if (!g_inst) return false;
    rec_ctrl_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.command = REC_CMD_STOP;
    return (do_send(&msg) == RL_SUCCESS);
}

bool rpmsg_record_send_play(const char *path)
{
    if (!g_inst || !path) return false;
    rec_ctrl_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.command = REC_CMD_PLAY;
    strncpy(msg.path, path, sizeof(msg.path) - 1);
    return (do_send(&msg) == RL_SUCCESS);
}

bool rpmsg_record_send_play_video(const char *path)
{
    if (!g_inst || !path) return false;
    rec_ctrl_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.command = REC_CMD_PLAY_VIDEO;
    strncpy(msg.path, path, sizeof(msg.path) - 1);
    int st = do_send(&msg);
    if (st == RL_SUCCESS) {
        /* 暂停摄像头 layer1, 启动回放显示任务 */
        video_play_display_start();
        face_detection_set_infer_interval(INFER_EVERY_N_RECORD);   /* 释放 SDRAM 带宽给 MJPEG 解码 */
    }
    return (st == RL_SUCCESS);
}

bool rpmsg_record_send_stop_play(void)
{
    if (!g_inst) return false;
    rec_ctrl_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.command = REC_CMD_STOP_PLAY;
    return (do_send(&msg) == RL_SUCCESS);
}

bool rpmsg_record_send_pause(void)
{
    if (!g_inst) return false;
    rec_ctrl_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.command = REC_CMD_PAUSE;
    return (do_send(&msg) == RL_SUCCESS);
}

bool rpmsg_record_send_resume(void)
{
    if (!g_inst) return false;
    rec_ctrl_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.command = REC_CMD_RESUME;
    return (do_send(&msg) == RL_SUCCESS);
}

bool rpmsg_record_send_list(const char *dir_path, uint32_t tag)
{
    if (!g_inst || !dir_path) return false;
    rec_ctrl_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.command = REC_CMD_LIST;
    msg.param1  = tag;                  /* echoed back in rec_stat_msg_t.state */
    strncpy(msg.path, dir_path, sizeof(msg.path) - 1);
    int st = do_send(&msg);
    printf("[REC-CPU0] LIST req '%s' tag=%lu → %s\r\n",
           dir_path, (unsigned long)tag, (st == RL_SUCCESS) ? "OK" : "FAIL");
    return (st == RL_SUCCESS);
}

/* 已废弃：改用 CI1302 声控模块播报（见 src/driver/ci1302/）。SD 卡预录 WAV
 * 音效播放功能移除，函数保留供历史参考，调用处已删除。 */
#if 0
bool rpmsg_record_send_sound(const char *sound_name)
{
    if (!g_inst || !sound_name) return false;
    rec_ctrl_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.command = REC_CMD_PLAY_SOUND;
    strncpy(msg.path, sound_name, sizeof(msg.path) - 1);
    return (do_send(&msg) == RL_SUCCESS);
}
#endif

bool rpmsg_record_send_delete(const char *path)
{
    if (!g_inst || !path) return false;
    rec_ctrl_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.command = REC_CMD_DELETE;
    strncpy(msg.path, path, sizeof(msg.path) - 1);
    return (do_send(&msg) == RL_SUCCESS);
}

/* ---- Meeting-record notes (ASR text) ---- */

bool rpmsg_record_send_notes_open(void)
{
    if (!g_inst) return false;
    rec_notes_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.command = REC_CMD_NOTES_OPEN;
    return (do_send_notes(&msg, offsetof(rec_notes_msg_t, data)) == RL_SUCCESS);
}

bool rpmsg_record_send_notes_append(const char *text, uint32_t len)
{
    if (!g_inst || !text) return false;
    bool ok = true;
    uint32_t off = 0;
    while (off < len && ok) {
        rec_notes_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.command = REC_CMD_NOTES_APPEND;
        uint32_t n = len - off;
        if (n > NOTES_DATA_MAX) n = NOTES_DATA_MAX;
        memcpy(msg.data, text + off, n);
        msg.param2 = n;
        ok = (do_send_notes(&msg, offsetof(rec_notes_msg_t, data) + n)
              == RL_SUCCESS);
        off += n;
    }
    return ok;
}

bool rpmsg_record_send_notes_close(void)
{
    if (!g_inst) return false;
    rec_notes_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.command = REC_CMD_NOTES_CLOSE;
    return (do_send_notes(&msg, offsetof(rec_notes_msg_t, data)) == RL_SUCCESS);
}

bool rpmsg_record_send_notes_read(const char *filename)
{
    if (!g_inst || !filename) return false;
    rec_notes_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.command = REC_CMD_NOTES_READ;
    strncpy(msg.data, filename, NOTES_DATA_MAX - 1);
    msg.data[NOTES_DATA_MAX - 1] = '\0';

    /* Remember the name locally so the viewer can title itself on NOTES_END. */
    strncpy(s_notes_read_name, filename, sizeof(s_notes_read_name) - 1);
    s_notes_read_name[sizeof(s_notes_read_name) - 1] = '\0';

    /* 复位累积缓冲: 空文件(或读取失败)时不会收到 NOTES_DATA, 避免残留上一次内容 */
    s_notes_text_len = 0;
    s_notes_text[0]  = '\0';

    return (do_send_notes(&msg,
                          offsetof(rec_notes_msg_t, data) + strlen(msg.data) + 1)
            == RL_SUCCESS);
}
