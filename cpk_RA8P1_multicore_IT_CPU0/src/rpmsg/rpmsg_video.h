/*
 * rpmsg_video.h — CPU0/CPU1 视频录制帧共享协议 (两核内容一致)
 *
 * CPU0 摄像头任务把"带人脸框"的 640×480 RGB565 画面写入双缓冲 SDRAM,
 * 递增 frame_id; CPU1 视频录像任务轮询 frame_id, 读到新帧后 MJPEG 编码
 * 写入 AVI。双缓冲避免 CPU0 写入与 CPU1 编码读同一块导致撕裂。
 *
 * 地址 (非缓存 SDRAM 区, 手掌模型 0x68600000 之后):
 *   0x6863C000  video_shmem_t (同步结构)
 *   0x68640000  frame buffer 0 (614,400 B)
 *   0x686E0000  frame buffer 1 (614,400 B)
 *
 * 同步协议 (单调 frame_id, 与 cam_shmem 同思路, 避免 ready 标志的 D-Cache 陈旧):
 *   CPU0: 写 buffer[write_idx] → CleanDCache → 写 write_idx → CleanDCache
 *         → frame_id++ → CleanDCache
 *   CPU1: InvalidateDCache(shmem) → 读 frame_id, 变化则 InvalidateDCache(帧)
 *         → 读 buffer[write_idx] 编码
 */

#ifndef RPMSG_VIDEO_H_
#define RPMSG_VIDEO_H_

#include <stdint.h>

#define VIDEO_FRAME_W       640
#define VIDEO_FRAME_H       480
#define VIDEO_FRAME_SIZE    (VIDEO_FRAME_W * VIDEO_FRAME_H * 2)   /* 614,400 */

#define VIDEO_SHMEM_ADDR    0x6863C000UL
#define VIDEO_FRAME0_ADDR   0x68640000UL
#define VIDEO_FRAME1_ADDR   0x686E0000UL

/* ---- 播放方向 (CPU1 解码 → CPU0 显示) ----
 * 复用同一套 video_shmem_t 同步 + 双缓冲 frame buffer, 只是生产者/消费者角色
 * 对调: CPU1 的 video_player 解码 AVI 后把 320×240 RGB565 写入
 * buffer[write_idx] 并递增 frame_id; CPU0 的播放显示任务轮询 frame_id,
 * 读到新帧后 2× 放大上屏。录制与播放互斥, 复用无冲突。 */
#define VIDEO_PLAY_W        320
#define VIDEO_PLAY_H        240
#define VIDEO_PLAY_SIZE     (VIDEO_PLAY_W * VIDEO_PLAY_H * 2)   /* 153,600 */

typedef struct {
    volatile uint32_t frame_id;     /* CPU0 每写完一帧递增; CPU1 比较检测新帧 */
    volatile uint32_t write_idx;    /* 最新完整帧所在 buffer (0 或 1) */
} video_shmem_t;

static inline video_shmem_t *video_shmem(void)
{
    return (video_shmem_t *)VIDEO_SHMEM_ADDR;
}

#endif /* RPMSG_VIDEO_H_ */
