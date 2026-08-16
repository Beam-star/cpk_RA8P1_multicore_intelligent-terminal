/**
 ******************************************************************************
 * @file    video_player.h
 * @brief   视频回放 (AVI → MJPEG 解码 → 共享 SDRAM) — 头文件
 *
 * CPU1 从 SD 卡读 AVI, 逐帧 JPEG 解码为 320×240 RGB565 写入共享双缓冲
 * (rpmsg_video.h), 递增 frame_id; CPU0 播放显示任务轮询后 2× 放大上屏。
 ******************************************************************************
 */

#ifndef VIDEO_PLAYER_H_
#define VIDEO_PLAYER_H_

#include <stdint.h>
#include <stdbool.h>

bool video_player_init(void);

/**
 * @brief 阻塞式播放 AVI (在独立任务中调用)
 * @return true=播放完成, false=打开/解码失败
 */
bool video_player_play(const char *path);

/** 请求停止当前播放 */
void video_player_stop(void);

/** 暂停当前播放 (在帧边界生效) */
void video_player_pause(void);

/** 恢复暂停的播放 */
void video_player_resume(void);

/** 获取播放进度 (elapsed/total 毫秒); 未在播放返回 false */
bool video_player_get_progress(uint32_t *elapsed_ms, uint32_t *total_ms);

/** 是否正在播放 */
bool video_player_is_playing(void);

/**
 * @brief 卡死自愈: 强制关闭当前打开的文件句柄并复位播放器状态
 *
 * 播放任务崩溃/挂死时泄漏 SD 文件句柄且状态卡在 PLAYING。在 FAT 锁已被
 * sd_card_force_unlock() 释放后调用, 清理残留句柄并回到 IDLE。
 */
void video_player_force_close(void);

#endif /* VIDEO_PLAYER_H_ */
