/*
 * rpmsg_record.h — RPMsg record + audio control protocol (shared CPU0↔CPU1)
 *
 * CPU0 UI → CPU1: start/stop recording, play audio, list files, play sound
 * CPU1     → CPU0: status events, file lists, play-complete notifications
 *
 * Wire sizes (RPMsg payload limit is 496 bytes):
 *   - rec_ctrl_msg_t: always sizeof (64 B).
 *   - rec_stat_msg_t non-LIST events: send offsetof(rec_stat_msg_t, names)
 *     = 28 bytes (scalar header only).
 *   - REC_EVT_LIST: chunked. Each chunk carries up to LIST_CHUNK_MAX names;
 *     send offsetof(names) + name_count*32 bytes. `count` = TOTAL files,
 *     `start_index` = index of names[0] in the full list, `state` echoes
 *     the list tag from the REC_CMD_LIST request (param1).
 *
 * Copy this file to CPU1's src/rpmsg/ with identical content.
 */

#ifndef RPMSG_RECORD_H_
#define RPMSG_RECORD_H_

#include <stdint.h>

/* ---- Endpoint addresses ---- */
#define REC_EPT_CPU1  (60U)    /* CPU1 receiver (recorder/player)              */
#define REC_EPT_CPU0  (61U)    /* CPU0 receiver (status → UI)                  */

/* ======================================================================== */
/*  Commands (CPU0 → CPU1)                                                    */
/* ======================================================================== */

typedef struct {
    uint32_t command;              /* CMD_* (see below)                        */
    uint32_t param1;               /* context-dependent; LIST: list tag        */
    uint32_t param2;               /* context-dependent                        */
    char     path[52];             /* file path for play/list operations       */
} rec_ctrl_msg_t;
_Static_assert(sizeof(rec_ctrl_msg_t) == 64, "rec_ctrl_msg_t must be 64 B");

/* ======================================================================== */
/*  Meeting-record notes (ASR text) transfer — CPU0 ↔ CPU1                    */
/* ======================================================================== */

/*
 * A separate, larger message than rec_ctrl_msg_t so a whole UTF-8 subtitle
 * line can cross in one shot. The leading 12 bytes (command/param1/param2)
 * are laid out identically to rec_ctrl_msg_t, so a receiver can read the
 * command field first and dispatch on it regardless of which struct arrived.
 */
#define NOTES_DATA_MAX  400   /* max UTF-8 bytes per notes payload            */

typedef struct {
    uint32_t command;              /* REC_CMD_NOTES_* / REC_EVT_NOTES_*       */
    uint32_t param1;               /* APPEND: reserved; READ: chunk index     */
    uint32_t param2;               /* APPEND: data byte length                */
    char     data[NOTES_DATA_MAX]; /* APPEND: UTF-8 text; READ: filename      */
} rec_notes_msg_t;                 /* 12 + 400 = 412 B ≤ 496 payload          */
_Static_assert(sizeof(rec_notes_msg_t) <= 496, "rec_notes_msg_t exceeds RPMsg payload");

/* ---- Command codes ---- */
#define REC_CMD_NONE        0x00   /* not set / NS announcement               */
#define REC_CMD_START       0x30   /* start audio recording                    */
#define REC_CMD_STOP        0x31   /* stop recording                           */
#define REC_CMD_PLAY        0x32   /* play a WAV file by path                  */
#define REC_CMD_STOP_PLAY   0x33   /* stop current playback                    */
#define REC_CMD_LIST        0x34   /* list files in a directory                */
#define REC_CMD_PLAY_SOUND  0x35   /* play a short sound effect by name        */
#define REC_CMD_DELETE      0x36   /* delete a file (audio/video sync-delete
                                    * its paired counterpart)                 */
#define REC_CMD_PLAY_VIDEO  0x37   /* play a video (AVI) file by path         */
#define REC_CMD_PAUSE       0x38   /* pause current playback (audio/video)    */
#define REC_CMD_RESUME      0x39   /* resume paused playback                  */

/* ---- Meeting-record notes commands (carried in rec_notes_msg_t) ---- */
#define REC_CMD_NOTES_OPEN    0x3A  /* open a new record_XX.txt, start saving */
#define REC_CMD_NOTES_APPEND  0x3B  /* append text (param2=len, data=bytes)   */
#define REC_CMD_NOTES_CLOSE   0x3C  /* finalize + close the open txt          */
#define REC_CMD_NOTES_READ    0x3D  /* read a txt file (data=filename)        */

/* ---- List tags (REC_CMD_LIST param1, echoed in rec_stat_msg_t.state) ---- */
#define REC_LIST_TAG_AUDIO  0U
#define REC_LIST_TAG_VIDEO  1U
#define REC_LIST_TAG_NOTES  2U

/* ======================================================================== */
/*  Events (CPU1 → CPU0)                                                      */
/* ======================================================================== */

#define FILE_LIST_MAX   32     /* CPU0 accumulator / CPU1 dir-scan cap        */
#define LIST_CHUNK_MAX  14     /* names per RPMsg message (fits 496 B)        */

typedef struct {
    uint32_t event;                /* EVT_* (see below)                        */
    uint32_t state;                /* rec/player state; EVT_LIST: list tag     */
    uint32_t count;                /* EVT_LIST: TOTAL file count; else frames  */
    uint32_t duration_sec;
    uint32_t size_bytes;
    uint32_t name_count;           /* names valid in THIS message (≤ chunk)    */
    uint32_t start_index;          /* index of names[0] within the full list   */
    char     names[LIST_CHUNK_MAX][32];  /* file names for EVT_LIST chunks     */
} rec_stat_msg_t;                  /* 28 + 448 = 476 B ≤ 496 payload limit     */
_Static_assert(sizeof(rec_stat_msg_t) <= 496, "rec_stat_msg_t exceeds RPMsg payload");

/* ---- Event codes ---- */
#define REC_EVT_STATUS      0x40   /* periodic recording status                */
#define REC_EVT_STARTED     0x41   /* recording started OK                     */
#define REC_EVT_STOPPED     0x42   /* recording finished                       */
#define REC_EVT_ERROR       0x43   /* recording/playback error                 */
#define REC_EVT_PLAY_DONE   0x44   /* audio playback finished                  */
#define REC_EVT_LIST        0x45   /* file list response (chunked)             */
#define REC_EVT_VIDEO_SAVED 0x46   /* video encoding + save finished           */
#define REC_EVT_VIDEO_PLAY_DONE 0x47  /* video playback finished               */
#define REC_EVT_PLAY_PROGRESS 0x48  /* playback progress: count=elapsed_ms, duration_sec=total_ms */

/* ---- Meeting-record notes events (CPU1 → CPU0, via rec_stat_msg_t) ---- */
#define REC_EVT_NOTES_OPENED  0x49   /* notes file opened: names[0]=filename  */
#define REC_EVT_NOTES_SAVED   0x4A   /* notes file saved:  names[0]=filename  */
#define REC_EVT_NOTES_DATA    0x4B   /* read chunk: count=total bytes,        */
                                     /*   start_index=chunk offset,            */
                                     /*   name_count=bytes in names[] (raw)    */
#define REC_EVT_NOTES_END     0x4C   /* read complete: count=total bytes      */
#define REC_EVT_NOTES_ERROR   0x4D   /* notes error: state=code               */

#endif /* RPMSG_RECORD_H_ */
