/**
 * @file    rpmsg_record_cpu1.c
 * @brief   CPU1-side record-control RPMsg channel.
 *
 * A single FreeRTOS task loops on rpmsg_queue_recv, dispatches
 * REC_CMD_START / REC_CMD_STOP to the av_recorder module, and sends
 * status events back to CPU0 so the UI can update.
 *
 * Pattern copied from rpmsg_test.c handle_message() (the only existing
 * CPU1 command-receiver pattern).
 */

#include "rpmsg_record_cpu1.h"
#include "rpmsg_record.h"
#include "rpmsg_core.h"
#include "rpmsg_lite.h"
#include "rpmsg_queue.h"
#include "rpmsg_log.h"
#include "recorder/av_recorder.h"
#include "recorder/video_recorder.h"
#include "driver/sd_card/sdhi_driver.h"
#include "player/audio_player.h"
#include "player/video_player.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>

/* ---- Task config ---- */
#define REC_CTRL_TASK_STACK   2048
#define REC_CTRL_TASK_PRIO    3
#define REC_STATUS_INTERVAL_MS  1000   /* periodic status ping interval       */

/* Non-LIST events carry only the scalar header (see rpmsg_record.h). */
#define REC_STAT_HDR_LEN  ((uint32_t)offsetof(rec_stat_msg_t, names))

/* ---- Static state ---- */
static struct rpmsg_lite_instance *g_inst   = NULL;
static struct rpmsg_lite_endpoint *g_ept    = NULL;
static rpmsg_queue_handle          g_queue  = NULL;
static uint32_t                    g_remote = REC_EPT_CPU0;
static SemaphoreHandle_t           g_send_mutex = NULL;

/* One DAC — shared busy flag for PLAY and PLAY_SOUND workers.
 * 带超时自愈: 播放任务若挂死, 超时后强制恢复 (见 force_recover_playback)。 */
static volatile bool       g_play_busy = false;
static volatile TickType_t g_play_busy_since = 0;

/* 停止请求发出时间 (0 = 无待处理停止)。看门狗据此判定播放任务是否挂死。 */
static volatile TickType_t g_stop_req_tick = 0;

/*
 * 强制恢复: 播放任务崩溃/挂死时会 (1) 泄漏 SD 文件句柄, (2) 卡住
 * FreeRTOS+FAT 的 FAT 锁 (若死在 ff_fread 内部), 导致后续所有 SD 文件操作
 * (音频+视频播放) 永久阻塞。顺序很关键: 先强制释放 FAT 锁, 再关闭泄漏的
 * 文件句柄 (否则 fclose 会阻塞在锁上), 最后复位播放器状态与 busy 标志。
 */
static void force_recover_playback(void)
{
    sd_card_force_unlock();
    audio_player_force_close();
    video_player_force_close();
    g_play_busy    = false;
    g_stop_req_tick = 0;
    rpmsg_log_cpu1_printf("[REC-CPU1] Playback force-recovered (SD lock released)\r\n");
}

/* 看门狗: 停止请求发出后, 健康播放任务会在 ~500ms (一个播放块) 内退出;
 * 超过 1.5s 仍未 idle 说明任务已崩溃/挂死 → 强制恢复。 */
static void playback_watchdog(void)
{
    if (g_stop_req_tick == 0) return;

    bool idle = (audio_player_get_state() == PLAYER_IDLE)
                && !video_player_is_playing();
    if (idle) {
        g_stop_req_tick = 0;
        return;
    }
    if ((xTaskGetTickCount() - g_stop_req_tick) > pdMS_TO_TICKS(1500)) {
        force_recover_playback();
    }
}

static bool play_busy(void)
{
    if (!g_play_busy) return false;
    if ((xTaskGetTickCount() - g_play_busy_since)
            > pdMS_TO_TICKS(60U * 1000U)) {
        /* 挂死 >60s: 完整恢复 (释放 FAT 锁 + 清理句柄), 而非仅清标志,
         * 否则新播放任务会卡在 FF_LockFAT 上再次挂死。 */
        force_recover_playback();
        return false;
    }
    return true;
}

static void play_busy_set(void)
{
    g_play_busy       = true;
    g_play_busy_since = xTaskGetTickCount();
}

/* ---- Forward decls ---- */
static void send_status_msg(const rec_stat_msg_t *msg, uint32_t len);

/* ---- Playback worker (one-shot, deletes itself) ---- */
static void playback_task(void *pvParams)
{
    const char *path = (const char *)pvParams;
    bool ok = audio_player_play(path);
    g_play_busy = false;
    rec_stat_msg_t st;
    memset(&st, 0, sizeof(st));
    st.event = REC_EVT_PLAY_DONE;
    st.state = ok ? 0 : 1;
    send_status_msg(&st, REC_STAT_HDR_LEN);
    vTaskDelete(NULL);
}

/* ---- Video-playback worker (one-shot, deletes itself) ---- */
static void video_playback_task(void *pvParams)
{
    const char *path = (const char *)pvParams;
    bool ok = video_player_play(path);
    rec_stat_msg_t st;
    memset(&st, 0, sizeof(st));
    st.event = REC_EVT_VIDEO_PLAY_DONE;
    st.state = ok ? 0 : 1;
    send_status_msg(&st, REC_STAT_HDR_LEN);
    vTaskDelete(NULL);
}

/* ---- Sound-effect worker (one-shot, deletes itself) ----
 * 已废弃：SD 卡预录 WAV 音效播放改用 CPU0 的 CI1302 声控模块播报。
 * 保留供历史参考，REC_CMD_PLAY_SOUND 分支已同步注释。 */
#if 0
static void sound_task(void *pvParams)
{
    const char *name = (const char *)pvParams;
    char path[64];
    snprintf(path, sizeof(path), "/meeting/sounds/%s.wav", name);
    audio_player_play(path);
    g_play_busy = false;
    vTaskDelete(NULL);
}
#endif

/* ---- Local helpers ---- */

/**
 * Send a status message back to CPU0 via the persistent endpoint.
 *
 * A temporary endpoint at REC_EPT_CPU1 cannot be created here —
 * rpmsg_lite_create_ept() returns NULL for an address already in use
 * (our RX endpoint owns it). `len` is the wire size: REC_STAT_HDR_LEN
 * for plain events, header + name_count*32 for LIST chunks.
 * Serialized by g_send_mutex (ctrl / playback / sound tasks all send).
 */
static void send_status_msg(const rec_stat_msg_t *msg, uint32_t len)
{
    if (!g_ept) return;
    if (g_send_mutex) xSemaphoreTake(g_send_mutex, portMAX_DELAY);
    rpmsg_lite_send(g_inst, g_ept, g_remote,
                    (char *)msg, len, RL_DONT_BLOCK);
    if (g_send_mutex) xSemaphoreGive(g_send_mutex);
}

/** Send recording status back to CPU0. */
static void send_status(uint32_t event)
{
    rec_stat_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.event = event;

    rec_info_t info = av_recorder_get_info();
    msg.state        = (uint32_t)info.state;
    msg.count        = info.video_frames;
    msg.duration_sec = info.duration_sec;
    msg.size_bytes   = info.file_size_bytes;

    send_status_msg(&msg, REC_STAT_HDR_LEN);
}

/** Scan a directory and send the file list to CPU0 in ≤14-name chunks. */
static void send_file_list(const char *dir_path, uint32_t tag)
{
    sd_file_info_t files[FILE_LIST_MAX];
    int n = sd_card_list_dir(dir_path, files, FILE_LIST_MAX);
    rpmsg_log_cpu1_printf("[REC-CPU1] LIST %s (tag=%lu): %d files\r\n",
                          dir_path, (unsigned long)tag, n);
    if (n < 0) n = 0;

    rec_stat_msg_t st;
    if (n == 0) {
        memset(&st, 0, sizeof(st));
        st.event = REC_EVT_LIST;
        st.state = tag;
        send_status_msg(&st, REC_STAT_HDR_LEN);
        return;
    }

    for (int start = 0; start < n; start += LIST_CHUNK_MAX) {
        int k = n - start;
        if (k > LIST_CHUNK_MAX) k = LIST_CHUNK_MAX;

        memset(&st, 0, sizeof(st));
        st.event       = REC_EVT_LIST;
        st.state       = tag;
        st.count       = (uint32_t)n;
        st.name_count  = (uint32_t)k;
        st.start_index = (uint32_t)start;
        for (int i = 0; i < k; i++) {
            strncpy(st.names[i], files[start + i].name, 31);
            st.names[i][31] = '\0';
        }
        send_status_msg(&st, REC_STAT_HDR_LEN + (uint32_t)k * 32U);
    }
}

/* ======================================================================== */
/*  Meeting-record notes (ASR text → /meeting/notes/record_XX.txt)          */
/* ======================================================================== */

static void *g_notes_file = NULL;      /* open record_XX.txt handle         */
static char  g_notes_name[32];         /* current record filename           */
static bool  g_notes_has_data = false; /* 本次是否有字幕写入 (空则丢弃)     */

/* Text chunks ride in the rec_stat_msg_t.names area (448 bytes contiguous). */
#define NOTES_CHUNK_MAX  ((uint32_t)sizeof(((rec_stat_msg_t *)0)->names))

/* Scan /meeting/notes for the highest record_NN.txt number, return the
 * next free number (max + 1, so a fresh card starts at record_01.txt). */
static int find_next_record_number(void)
{
    sd_file_info_t files[FILE_LIST_MAX];
    int n = sd_card_list_dir("/meeting/notes", files, FILE_LIST_MAX);
    int max_num = 0;
    for (int i = 0; i < n; i++) {
        if (files[i].is_directory) continue;
        int num = 0;
        if (sscanf(files[i].name, "record_%d.txt", &num) == 1 && num > max_num) {
            max_num = num;
        }
    }
    return max_num + 1;
}

/* Scan /meeting/audio + /meeting/video for the highest meeting_NN.* number,
 * return the next free number (audio and video share the same number so the
 * delete-pairing works). */
static int find_next_meeting_number(void)
{
    sd_file_info_t files[FILE_LIST_MAX];
    int max_num = 0;

    int n = sd_card_list_dir("/meeting/audio", files, FILE_LIST_MAX);
    for (int i = 0; i < n; i++) {
        if (files[i].is_directory) continue;
        int num = 0;
        if (sscanf(files[i].name, "meeting_%d.wav", &num) == 1 && num > max_num) {
            max_num = num;
        }
    }

    n = sd_card_list_dir("/meeting/video", files, FILE_LIST_MAX);
    for (int i = 0; i < n; i++) {
        if (files[i].is_directory) continue;
        int num = 0;
        if (sscanf(files[i].name, "meeting_%d.avi", &num) == 1 && num > max_num) {
            max_num = num;
        }
    }

    return max_num + 1;
}

/* Send a scalar notes event, optionally carrying a filename in names[0]. */
static void send_notes_event(uint32_t event, const char *name, uint32_t code)
{
    rec_stat_msg_t st;
    memset(&st, 0, sizeof(st));
    st.event = event;
    st.state = code;
    if (name) {
        strncpy(st.names[0], name, 31);
        st.names[0][31] = '\0';
        send_status_msg(&st, REC_STAT_HDR_LEN + 32U);
    } else {
        send_status_msg(&st, REC_STAT_HDR_LEN);
    }
}

/* Send one raw text chunk of a read-back (no NUL termination needed — CPU0
 * reassembles the bytes contiguously and NUL-terminates at NOTES_END). */
static void send_notes_data(const char *buf, uint32_t len, uint32_t offset)
{
    rec_stat_msg_t st;
    memset(&st, 0, sizeof(st));
    if (len > NOTES_CHUNK_MAX) len = NOTES_CHUNK_MAX;
    st.event       = REC_EVT_NOTES_DATA;
    st.start_index = offset;
    st.name_count  = len;
    memcpy(&st.names[0][0], buf, len);
    send_status_msg(&st, REC_STAT_HDR_LEN + len);
}

static void handle_notes_open(void)
{
    /* Stale open file (shouldn't happen in normal flow) — close first. */
    if (g_notes_file) {
        sd_card_fclose(g_notes_file);
        g_notes_file = NULL;
    }

    int num = find_next_record_number();
    snprintf(g_notes_name, sizeof(g_notes_name), "record_%02d.txt", num);
    char path[64];
    snprintf(path, sizeof(path), "/meeting/notes/%s", g_notes_name);

    g_notes_file = sd_card_fopen(path, "w");
    g_notes_has_data = false;
    rpmsg_log_cpu1_printf("[NOTES] open %s: %s\r\n", path,
                          g_notes_file ? "OK" : "FAILED");
    if (g_notes_file) {
        send_notes_event(REC_EVT_NOTES_OPENED, g_notes_name, 0);
    } else {
        send_notes_event(REC_EVT_NOTES_ERROR, NULL, 1);
    }
}

static void handle_notes_append(const rec_notes_msg_t *msg)
{
    if (!g_notes_file) {
        send_notes_event(REC_EVT_NOTES_ERROR, NULL, 2);
        return;
    }
    uint32_t len = msg->param2;
    if (len > NOTES_DATA_MAX) len = NOTES_DATA_MAX;
    if (len > 0) {
        sd_card_fwrite(g_notes_file, msg->data, len);
        g_notes_has_data = true;
    }
}

static void handle_notes_close(void)
{
    if (g_notes_file) {
        sd_card_fclose(g_notes_file);
        g_notes_file = NULL;
        if (g_notes_has_data) {
            rpmsg_log_cpu1_printf("[NOTES] saved %s\r\n", g_notes_name);
            send_notes_event(REC_EVT_NOTES_SAVED, g_notes_name, 0);
        } else {
            /* 无字幕写入: 删除空文件, 不保留 */
            char path[64];
            snprintf(path, sizeof(path), "/meeting/notes/%s", g_notes_name);
            sd_card_delete(path);
            rpmsg_log_cpu1_printf("[NOTES] empty, discarded %s\r\n", g_notes_name);
            send_notes_event(REC_EVT_NOTES_ERROR, NULL, 5);  /* 5 = 无数据 */
        }
    } else {
        send_notes_event(REC_EVT_NOTES_ERROR, NULL, 3);
    }
}

static void handle_notes_read(const rec_notes_msg_t *msg)
{
    char path[64];
    snprintf(path, sizeof(path), "/meeting/notes/%s", msg->data);

    void *fp = sd_card_fopen(path, "r");
    if (!fp) {
        rpmsg_log_cpu1_printf("[NOTES] read %s: FAILED\r\n", path);
        send_notes_event(REC_EVT_NOTES_ERROR, NULL, 4);
        return;
    }

    /* static — avoids growing rec_ctrl_task's 2 KB stack. */
    static uint8_t chunk[NOTES_CHUNK_MAX];
    uint32_t offset = 0;
    for (;;) {
        uint32_t n = sd_card_fread(fp, chunk, sizeof(chunk));
        if (n == 0) break;
        send_notes_data((const char *)chunk, n, offset);
        offset += n;
        if (n < sizeof(chunk)) break;   /* short read = EOF */
    }
    sd_card_fclose(fp);

    rec_stat_msg_t st;
    memset(&st, 0, sizeof(st));
    st.event = REC_EVT_NOTES_END;
    st.count = offset;                   /* total bytes read */
    send_status_msg(&st, REC_STAT_HDR_LEN);
}

static void handle_notes_message(const rec_notes_msg_t *msg)
{
    switch (msg->command) {
    case REC_CMD_NOTES_OPEN:   handle_notes_open();        break;
    case REC_CMD_NOTES_APPEND: handle_notes_append(msg);   break;
    case REC_CMD_NOTES_CLOSE:  handle_notes_close();       break;
    case REC_CMD_NOTES_READ:   handle_notes_read(msg);     break;
    default: break;
    }
}

/** Handle a single incoming control message. */
static void handle_message(const rec_ctrl_msg_t *cmd, uint32_t src_addr)
{
    g_remote = src_addr;

    switch (cmd->command) {
    case REC_CMD_START: {
        /* 播放中不允许录制 (录制与播放共用 SD 卡 + CPU1, 会互相抢占) */
        if (audio_player_get_state() != PLAYER_IDLE || video_player_is_playing()) {
            rec_stat_msg_t st;
            memset(&st, 0, sizeof(st));
            st.event = REC_EVT_ERROR;
            st.state = 3;              /* busy playing */
            send_status_msg(&st, REC_STAT_HDR_LEN);
            break;
        }
        rec_config_t cfg = {
            .video_quality    = (uint8_t)cmd->param1,
            .video_fps        = 0,
            .max_duration_sec = (uint16_t)cmd->param2,
        };
        int meeting_num = find_next_meeting_number();
        bool audio_ok = av_recorder_start(&cfg, meeting_num);
        bool video_ok = video_recorder_start((uint8_t)cmd->param1, meeting_num);
        if (audio_ok || video_ok) {
            send_status(REC_EVT_STARTED);
        } else {
            send_status(REC_EVT_ERROR);
        }
        break;
    }
    case REC_CMD_STOP:
        av_recorder_stop();
        video_recorder_stop();
        {
            /* 视频编码保存完成 → 通知 CPU0 显示 "Save successfully" */
            rec_stat_msg_t st;
            memset(&st, 0, sizeof(st));
            st.event = REC_EVT_VIDEO_SAVED;
            st.count = video_recorder_get_frame_count();
            send_status_msg(&st, REC_STAT_HDR_LEN);
        }
        send_status(REC_EVT_STOPPED);
        break;

    case REC_CMD_PLAY: {
        /* 录制中不允许播放 */
        if (av_recorder_is_recording()) {
            rec_stat_msg_t st;
            memset(&st, 0, sizeof(st));
            st.event = REC_EVT_ERROR;
            st.state = 4;              /* busy recording */
            send_status_msg(&st, REC_STAT_HDR_LEN);
            break;
        }
        /* Spawn a short-lived task for blocking playback */
        static char play_path[52];
        if (play_busy()) {
            rec_stat_msg_t st;
            memset(&st, 0, sizeof(st));
            st.event = REC_EVT_ERROR;
            st.state = 2;              /* player busy */
            send_status_msg(&st, REC_STAT_HDR_LEN);
            break;
        }
        play_busy_set();
        strncpy(play_path, cmd->path, sizeof(play_path) - 1);
        play_path[sizeof(play_path) - 1] = '\0';
        if (xTaskCreate(playback_task, "play", 8192,
                        play_path, 3, NULL) != pdPASS) {
            rpmsg_log_cpu1_printf("[REC-CPU1] play task create FAILED (heap low?)\r\n");
            g_play_busy = false;
        }
        break;
    }
    case REC_CMD_PLAY_VIDEO: {
        /* 录制中不允许播放 */
        if (av_recorder_is_recording()) {
            rec_stat_msg_t st;
            memset(&st, 0, sizeof(st));
            st.event = REC_EVT_ERROR;
            st.state = 4;              /* busy recording */
            send_status_msg(&st, REC_STAT_HDR_LEN);
            break;
        }
        static char vplay_path[52];
        strncpy(vplay_path, cmd->path, sizeof(vplay_path) - 1);
        vplay_path[sizeof(vplay_path) - 1] = '\0';
        if (xTaskCreate(video_playback_task, "vplay", 8192,
                        vplay_path, 3, NULL) != pdPASS) {
            rpmsg_log_cpu1_printf("[REC-CPU1] vplay task create FAILED\r\n");
        }
        break;
    }

    case REC_CMD_STOP_PLAY:
        audio_player_stop();
        video_player_stop();
        g_stop_req_tick = xTaskGetTickCount();   /* 启动卡死看门狗 */
        break;

    case REC_CMD_PAUSE:
        audio_player_pause();
        video_player_pause();
        break;

    case REC_CMD_RESUME:
        audio_player_resume();
        video_player_resume();
        break;

    case REC_CMD_LIST:
        send_file_list(cmd->path, cmd->param1);
        break;

    /* 已废弃：SD 卡预录 WAV 音效播放改用 CPU0 的 CI1302 声控模块播报。
     * REC_CMD_PLAY_SOUND 不再有发送方，分支保留供历史参考。 */
#if 0
    case REC_CMD_PLAY_SOUND: {
        static char snd_name[32];
        if (play_busy()) {
            rpmsg_log_cpu1_printf("[REC-CPU1] sound '%s' skipped (busy)\r\n",
                                  cmd->path);
            break;
        }
        play_busy_set();
        strncpy(snd_name, cmd->path, sizeof(snd_name) - 1);
        snd_name[sizeof(snd_name) - 1] = '\0';
        if (xTaskCreate(sound_task, "snd", 6144,
                        snd_name, 3, NULL) != pdPASS) {
            rpmsg_log_cpu1_printf("[REC-CPU1] snd task create FAILED (heap low?)\r\n");
            g_play_busy = false;
        }
        break;
    }
#endif
    case REC_CMD_DELETE: {
        bool ok = sd_card_delete(cmd->path);
        rpmsg_log_cpu1_printf("[REC-CPU1] DELETE %s: %s\r\n",
                              cmd->path, ok ? "OK" : "FAILED");

        /* 同名配对文件同步删除: /meeting/audio/X.wav ↔ /meeting/video/X.avi
         * 双向 — 删音频连带删视频, 删视频也连带删音频。 */
        const char *base = strrchr(cmd->path, '/');
        if (ok && base) {
            const char *pair_dir = NULL;
            const char *pair_ext = NULL;
            if (strncmp(cmd->path, "/meeting/audio/", 15) == 0) {
                pair_dir = "/meeting/video"; pair_ext = ".avi";
            } else if (strncmp(cmd->path, "/meeting/video/", 15) == 0) {
                pair_dir = "/meeting/audio"; pair_ext = ".wav";
            }
            if (pair_dir) {
                char pair[64];
                snprintf(pair, sizeof(pair), "%s%s", pair_dir, base);
                char *dot = strrchr(pair, '.');
                if (dot && (size_t)(dot - pair) + 5 <= sizeof(pair)) {
                    strcpy(dot, pair_ext);
                    if (sd_card_delete(pair)) {
                        rpmsg_log_cpu1_printf("[REC-CPU1] Deleted paired %s\r\n", pair);
                    }
                }
            }
        }
        break;
    }

    case REC_CMD_NONE:
    default:
        break;
    }
}

/** 上报播放进度 (当前毫秒 / 总毫秒) 到 CPU0 */
static void send_play_progress(void)
{
    uint32_t elapsed_ms = 0, total_ms = 0;
    bool have = video_player_get_progress(&elapsed_ms, &total_ms);
    if (!have) have = audio_player_get_progress(&elapsed_ms, &total_ms);
    if (!have) return;

    rec_stat_msg_t st;
    memset(&st, 0, sizeof(st));
    st.event = REC_EVT_PLAY_PROGRESS;
    st.count = elapsed_ms;          /* 当前毫秒 */
    st.duration_sec = total_ms;     /* 总毫秒 */
    send_status_msg(&st, REC_STAT_HDR_LEN);
}

/** Periodic status ping to CPU0 while recording / playing. */
static void send_periodic_status(void)
{
    if (av_recorder_is_recording()) {
        send_status(REC_EVT_STATUS);
    } else {
        send_play_progress();
    }
}

/* ---- Main receiver task ---- */

static void rec_ctrl_task(void *pvParams)
{
    (void)pvParams;
    /* Notes messages (412 B) are larger than ctrl messages (64 B). Both
     * begin with a uint32_t command at offset 0, so receive into the larger
     * union and dispatch on the leading command field. */
    union {
        rec_ctrl_msg_t  ctrl;
        rec_notes_msg_t notes;
    } rx;
    uint32_t len, src;
    TickType_t last_status = xTaskGetTickCount();

    rpmsg_log_cpu1_printf("[REC-CPU1] Control task ready (ept=%u)\r\n", REC_EPT_CPU1);

    while (1) {
        /* Block with a timeout so we can send periodic status */
        int st = rpmsg_queue_recv(g_inst, g_queue, &src,
                                  (char *)&rx, sizeof(rx), &len,
                                  pdMS_TO_TICKS(500));
        if (st == RL_SUCCESS) {
            uint32_t cmd = rx.notes.command;
            if (cmd >= REC_CMD_NOTES_OPEN && cmd <= REC_CMD_NOTES_READ) {
                g_remote = src;
                handle_notes_message(&rx.notes);
            } else {
                handle_message(&rx.ctrl, src);
            }
        }

        /* 卡死看门狗: 检测停止后未退出的播放任务并强制恢复 */
        playback_watchdog();

        /* Periodic status ping */
        TickType_t now = xTaskGetTickCount();
        if ((now - last_status) >= pdMS_TO_TICKS(REC_STATUS_INTERVAL_MS)) {
            send_periodic_status();
            last_status = now;
        }
    }
}

/* ---- Public API ---- */

bool rpmsg_record_cpu1_init(void)
{
    g_inst = (struct rpmsg_lite_instance *)rpmsg_core_get_instance();
    if (!g_inst) {
        rpmsg_log_cpu1_printf("[REC-CPU1] RPMsg instance not available\r\n");
        return false;
    }

    g_send_mutex = xSemaphoreCreateMutex();
    if (!g_send_mutex) return false;

    g_queue = rpmsg_queue_create(g_inst);
    if (!g_queue) return false;

    g_ept = rpmsg_lite_create_ept(g_inst, REC_EPT_CPU1,
                                  rpmsg_queue_rx_cb, g_queue);
    if (!g_ept) return false;

    BaseType_t ret = xTaskCreate(rec_ctrl_task, "rec_ctrl",
                                 REC_CTRL_TASK_STACK, NULL,
                                 REC_CTRL_TASK_PRIO, NULL);
    if (ret != pdPASS) {
        rpmsg_log_cpu1_printf("[REC-CPU1] Failed to create ctrl task\r\n");
        return false;
    }

    rpmsg_log_cpu1_printf("[REC-CPU1] Channel ready (ept=%u)\r\n", REC_EPT_CPU1);
    return true;
}
