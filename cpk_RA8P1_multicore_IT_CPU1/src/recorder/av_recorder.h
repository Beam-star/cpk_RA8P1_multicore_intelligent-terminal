/**
 ******************************************************************************
 * @file    av_recorder.h
 * @brief   音视频录制任务头文件
 *
 * 录制流程 (CPU1 FreeRTOS 任务):
 *   1. 创建 SD 卡目录 /MEETING/YYYYMMDD_HHMMSS/
 *   2. 创建 AVI 文件 + 写入 AVI 头
 *   3. 主循环:
 *      a. 等待新视频帧 (通过 RPMsg cam_shmem_t 获取帧地址)
 *      b. MJPEG 软件编码 (Q 因子可配置)
 *      c. 写入 AVI movi '00dc' 视频块
 *      d. 采集 PCM 音频缓冲 → 写入 AVI movi '01wb' 音频块
 *      e. 更新录制状态 (帧计数/时长) → RPMsg 通知 CPU0 LVGL
 *   4. 录制结束:
 *      a. 回填 AVI 头 (RIFF 大小/总帧数)
 *      b. 写入 idx1 索引
 *      c. 关闭 AVI 文件
 *      d. 写 info.json 元信息文件
 ******************************************************************************
 */

#ifndef AV_RECORDER_H_
#define AV_RECORDER_H_

#include <stdint.h>
#include <stdbool.h>

/* ---- 录制状态 ---- */
typedef enum {
    REC_IDLE = 0,           /* 空闲 */
    REC_RECORDING,          /* 录制中 */
    REC_PAUSED,             /* 暂停 */
    REC_FINALIZING,         /* 正在结束 */
} rec_state_t;

/* ---- 录制配置 ---- */
typedef struct {
    uint8_t  video_quality;    /* MJPEG Q 因子 (50-90) */
    uint8_t  video_fps;        /* 录制帧率 (5/10) */
    uint16_t max_duration_sec; /* 最大录制时长 (0=无限制) */
} rec_config_t;

/* ---- 录制信息 (运行时) ---- */
typedef struct {
    rec_state_t state;
    uint32_t    video_frames;     /* 已编码视频帧数 */
    uint32_t    audio_bytes;      /* 已写入音频字节数 */
    uint32_t    duration_sec;     /* 当前录制时长 */
    uint32_t    file_size_bytes;  /* 当前文件大小 */
    char        filename[64];     /* 文件名 */
} rec_info_t;

/* ---- API ---- */

/**
 * @brief 初始化录制模块
 */
bool av_recorder_init(void);

/**
 * @brief 开始录制
 * @param config  录制配置参数
 * @param meeting_num  会议序号（文件名 meeting_XX.wav，与视频同名配对）
 * @return true=录制已启动, false=失败
 */
bool av_recorder_start(const rec_config_t *config, int meeting_num);

/**
 * @brief 暂停录制
 */
void av_recorder_pause(void);

/**
 * @brief 恢复录制
 */
void av_recorder_resume(void);

/**
 * @brief 停止录制 (阻塞等待文件完成)
 */
void av_recorder_stop(void);

/**
 * @brief 获取当前录制状态
 */
rec_info_t av_recorder_get_info(void);

/**
 * @brief 检查是否正在录制
 */
bool av_recorder_is_recording(void);

#endif /* AV_RECORDER_H_ */
