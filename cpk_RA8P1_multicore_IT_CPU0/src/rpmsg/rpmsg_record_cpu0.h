/*
 * rpmsg_record_cpu0.h — CPU0-side record + audio control channel
 */

#ifndef RPMSG_RECORD_CPU0_H_
#define RPMSG_RECORD_CPU0_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One-time init (called from cpu0main_thread_entry). */
bool rpmsg_record_cpu0_init(void);

/* ---- Recording ---- */
bool rpmsg_record_send_start(uint8_t quality, uint8_t fps, uint16_t max_dur_sec);
bool rpmsg_record_send_stop(void);

/* ---- Audio playback ---- */
bool rpmsg_record_send_play(const char *path);
bool rpmsg_record_send_stop_play(void);

/* ---- Video playback ---- */
bool rpmsg_record_send_play_video(const char *path);

/* ---- Playback control (pause/resume, applies to audio + video) ---- */
bool rpmsg_record_send_pause(void);
bool rpmsg_record_send_resume(void);

/* ---- File listing ----
 * tag: REC_LIST_TAG_AUDIO / REC_LIST_TAG_VIDEO (rpmsg_record.h) — echoed
 * back by CPU1 so the UI knows which section the reply belongs to. */
bool rpmsg_record_send_list(const char *dir_path, uint32_t tag);

/* ---- Sound effect (short .wav played on speaker) ----
 * 已废弃：改用 CI1302 声控模块播报（见 src/driver/ci1302/）。保留定义仅
 * 供历史参考，调用处已移除。 */
//bool rpmsg_record_send_sound(const char *sound_name);

/* ---- Delete a file (audio path also deletes the paired video) ---- */
bool rpmsg_record_send_delete(const char *path);

/* ---- Meeting-record notes (ASR text → /meeting/notes/record_XX.txt) ---- */
bool rpmsg_record_send_notes_open(void);                          /* start saving */
bool rpmsg_record_send_notes_append(const char *text, uint32_t len); /* append text */
bool rpmsg_record_send_notes_close(void);                         /* stop + save   */
bool rpmsg_record_send_notes_read(const char *filename);          /* read back     */

#ifdef __cplusplus
}
#endif

#endif /* RPMSG_RECORD_CPU0_H_ */
