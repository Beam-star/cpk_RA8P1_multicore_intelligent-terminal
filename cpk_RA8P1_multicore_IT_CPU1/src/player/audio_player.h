/**
 ******************************************************************************
 * @file    audio_player.h
 * @brief   WAV 音频播放器 — SD卡读取 → I2S DMA 播放
 *
 * 单任务模型: 调用 play() 阻塞直到播放完成, 或 stop() 中断。
 ******************************************************************************
 */

#ifndef AUDIO_PLAYER_H_
#define AUDIO_PLAYER_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 播放状态 */
typedef enum {
    PLAYER_IDLE,
    PLAYER_PLAYING,
    PLAYER_STOPPING,
} player_state_t;

/** 初始化播放器 (I2S 已就绪) */
bool audio_player_init(void);

/**
 * @brief 播放 WAV 文件 (阻塞直到完成或被 stop)
 * @param path  SD 卡路径, 如 "/meeting/audio/0001.wav"
 * @return true=播放成功, false=失败
 */
bool audio_player_play(const char *path);

/** 停止当前播放 */
void audio_player_stop(void);

/** 暂停当前播放 */
void audio_player_pause(void);

/** 恢复暂停的播放 */
void audio_player_resume(void);

/**
 * @brief 卡死自愈: 强制关闭当前打开的文件句柄并复位播放器状态
 *
 * 播放任务崩溃/挂死时会泄漏 SD 文件句柄且播放器状态卡在 PLAYING。
 * 在 FAT 锁已被 sd_card_force_unlock() 释放后调用, 用于清理残留句柄
 * 并让播放器回到 IDLE, 以便后续能正常播放其它文件。
 */
void audio_player_force_close(void);

/** 获取播放进度 (elapsed/total 毫秒); 未在播放返回 false */
bool audio_player_get_progress(uint32_t *elapsed_ms, uint32_t *total_ms);

/** 当前状态 */
player_state_t audio_player_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_PLAYER_H_ */
