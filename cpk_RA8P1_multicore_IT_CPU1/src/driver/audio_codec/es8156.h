/**
 ******************************************************************************
 * @file    es8156.h
 * @brief   ES8156 音频 DAC 驱动头文件
 *
 * 芯片: Everest ES8156 (24-bit Stereo Audio DAC)
 * 接口: SSI/I2S (Serial Sound Interface)
 * 采样率: 8kHz ~ 192kHz
 * 功能: 会议录音回放, PCM 音频输出至耳机/扬声器
 ******************************************************************************
 */

#ifndef ES8156_H_
#define ES8156_H_

#include <stdint.h>
#include <stdbool.h>

/* ---- 音频参数 ---- */
#define ES8156_SAMPLE_RATE      (16000)    /* 默认采样率 */
#define ES8156_BIT_DEPTH        (16)       /* 位深 */
#define ES8156_CHANNELS         (1)        /* 单声道 (会议录音回放) */

/* ---- API ---- */

/**
 * @brief 初始化 ES8156 音频 DAC
 *
 * 流程:
 *   1. 配置 I2C 控制接口 (I2C1, 地址 0x10)
 *   2. 复位 ES8156 (写寄存器 0x00)
 *   3. 配置时钟分频 (MCLK/BCLK/LRCLK)
 *   4. 配置 I2S 格式 (16-bit, 左对齐)
 *   5. 设置初始音量 (60%)
 *   6. 静音解除
 */
bool es8156_init(void);

/**
 * @brief 设置输出音量
 * @param volume  音量 (0-100)
 */
void es8156_set_volume(uint8_t volume);

/**
 * @brief 静音/解除静音
 */
void es8156_set_mute(bool mute);

/**
 * @brief 通过 I2S DMA 播放 PCM 音频数据
 * @param pcm_data  16-bit PCM 数据缓冲区
 * @param samples   采样点数
 * @return true=播放已启动, false=失败
 */
bool es8156_play(const int16_t *pcm_data, uint32_t samples);

/**
 * @brief 检查播放是否完成
 */
bool es8156_is_playing(void);

#endif /* ES8156_H_ */
