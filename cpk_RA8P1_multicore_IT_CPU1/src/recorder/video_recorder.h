/**
 ******************************************************************************
 * @file    video_recorder.h
 * @brief   视频录像任务 — 轮询 CPU0 共享帧 → MJPEG 编码 → AVI → SD 卡
 *
 * 独立于音频录音 (av_recorder), 音频/视频分离保存。CPU0 摄像头任务把带人脸
 * 框的 640×480 RGB565 画面写入 rpmsg_video.h 定义的双缓冲 SDRAM 并递增
 * frame_id; 本任务轮询 frame_id, 读到新帧后 MJPEG 编码并写入 AVI。
 ******************************************************************************
 */

#ifndef VIDEO_RECORDER_H_
#define VIDEO_RECORDER_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 MJPEG 编码器 */
bool video_recorder_init(void);

/**
 * @brief 开始视频录制 (创建 AVI 文件 + 启动编码任务)
 * @param quality  MJPEG 质量因子 (50-90)
 * @param meeting_num  会议序号（文件名 meeting_XX.avi，与音频同名配对）
 * @return true=已启动
 */
bool video_recorder_start(uint8_t quality, int meeting_num);

/** 停止录制 (阻塞等待 AVI 封装完成) */
void video_recorder_stop(void);

/** 是否正在录制 */
bool video_recorder_is_recording(void);

/** 已编码的视频帧数 */
uint32_t video_recorder_get_frame_count(void);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_RECORDER_H_ */
