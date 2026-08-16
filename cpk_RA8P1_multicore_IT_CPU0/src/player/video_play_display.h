/**
 ******************************************************************************
 * @file    video_play_display.h
 * @brief   视频回放显示 (CPU0) — 轮询共享帧 → 2× 放大 → layer1 上屏
 *
 * 播放期间由 CPU1 解码 AVI 把 320×240 RGB565 写入共享双缓冲 (rpmsg_video.h),
 * 本模块的显示任务轮询 frame_id, 读到新帧后 2× 最近邻放大到 640×480 写入
 * GLCDC layer1 帧缓冲并 R_GLCDC_BufferChange。摄像头任务在播放期间暂停写
 * 帧缓冲 (由 mipi_camera_lcd_set_playback 控制), 避免冲突。
 ******************************************************************************
 */

#ifndef VIDEO_PLAY_DISPLAY_H_
#define VIDEO_PLAY_DISPLAY_H_

#include <stdint.h>
#include <stdbool.h>

/** 开始视频回放显示 (暂停摄像头显示, 启动显示任务) */
void video_play_display_start(void);

/** 停止视频回放显示 (恢复摄像头显示) */
void video_play_display_stop(void);

/** 是否正在回放显示 */
bool video_play_display_is_active(void);

#endif /* VIDEO_PLAY_DISPLAY_H_ */
