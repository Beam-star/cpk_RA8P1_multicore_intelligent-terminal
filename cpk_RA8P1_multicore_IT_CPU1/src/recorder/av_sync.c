/**
 ******************************************************************************
 * @file    av_sync.c
 * @brief   音视频同步模块实现
 ******************************************************************************
 */

#include "av_sync.h"
#include "FreeRTOS.h"

int32_t av_sync_diff_ms(av_timestamp_t video_ts, av_timestamp_t audio_ts)
{
    return (int32_t)(audio_ts - video_ts);
}

bool av_sync_should_drop(av_timestamp_t current_time,
                         av_timestamp_t last_frame_ts,
                         uint32_t frame_interval_ms)
{
    if (last_frame_ts == 0) {
        return false;  /* 第一帧不跳 */
    }

    /* 如果距离上一帧的时间已超过 2 倍帧间隔, 说明编码积压严重,
     * 跳帧追赶: 下一帧的预期时间 = last_frame_ts + frame_interval_ms */
    uint32_t elapsed = current_time - last_frame_ts;
    return (elapsed > frame_interval_ms * 2);
}
