/**
 ******************************************************************************
 * @file    av_sync.h
 * @brief   音视频同步模块头文件
 *
 * 同步策略:
 *   视频为主时钟 (Master Clock), 音频同步至视频时间戳
 *
 *   时间戳来源: FreeRTOS xTaskGetTickCount() (1ms 精度)
 *   视频帧间隔: 100ms (10fps 模式) 或 200ms (5fps 模式)
 *   音频缓冲间隔: 32ms (512 采样点)
 *
 * AVI 交错:
 *   每 1 个视频帧 + 约 0.1s 的 PCM 音频交错排列
 *   movi 中的块顺序: 00dc → 01wb → 01wb → 01wb → 00dc → ...
 *   (视频帧后紧跟若干个音频块, 每个音频块 = 32ms)
 ******************************************************************************
 */

#ifndef AV_SYNC_H_
#define AV_SYNC_H_

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "task.h"

/* ---- 时间戳 (基于 FreeRTOS tick) ---- */
typedef uint32_t av_timestamp_t;

/**
 * @brief 获取当前时间戳 (ms)
 */
static inline av_timestamp_t av_sync_get_time(void)
{
    return (av_timestamp_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/**
 * @brief 计算音视频时间戳差值 (ms)
 * @return 正值=音频超前, 负值=视频超前
 */
int32_t av_sync_diff_ms(av_timestamp_t video_ts, av_timestamp_t audio_ts);

/**
 * @brief 检查是否需要丢帧 (视频编码太慢, 跳帧追赶)
 * @param current_time  当前时间
 * @param last_frame_ts 上一帧时间戳
 * @param frame_interval_ms  帧间隔 (100ms=10fps, 200ms=5fps)
 * @return true=应跳过此帧, false=正常编码
 */
bool av_sync_should_drop(av_timestamp_t current_time,
                         av_timestamp_t last_frame_ts,
                         uint32_t frame_interval_ms);

#endif /* AV_SYNC_H_ */
