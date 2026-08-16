/**
 ******************************************************************************
 * @file    i2s_driver.h
 * @brief   SSI/I2S 音频接口驱动头文件
 *
 * 硬件: RA8P1 SSI (Serial Sound Interface) 模块
 * 模式: I2S Master (BCLK + LRCLK 由 RA8P1 输出)
 * 连接: SSI ← ES8156 DAC (播放), SSI ← PDM (采集已由 CPU0 处理)
 *
 * DMA: 双缓冲乒乓传输, 1ms 中断一次
 ******************************************************************************
 */

#ifndef I2S_DRIVER_H_
#define I2S_DRIVER_H_

#include <stdint.h>
#include <stdbool.h>

/* ---- I2S 参数 ---- */
#define I2S_SAMPLE_RATE         (16000)
#define I2S_BIT_DEPTH           (16)
#define I2S_CHANNELS            (1)
#define I2S_DMA_BUF_SIZE        (512)      /* 每通道 DMA 缓冲大小 */

/* ---- API ---- */

/**
 * @brief 启动 MCLK (GPT2 PWM 6.144MHz → ES8156 MCLK + SSI AUDIO_CLK)
 *
 * 必须在 es8156_init() 与 i2s_init() 之前调用(幂等,重复调用安全)。
 */
bool i2s_mclk_start(void);

/**
 * @brief 初始化 SSI/I2S 接口
 * @param direction  true=TX(播放), false=RX(录音)
 */
bool i2s_init(bool direction);

/**
 * @brief 启动 I2S DMA 传输
 * @param buf       PCM 数据缓冲区
 * @param samples   采样点数
 */
bool i2s_start_tx(const int16_t *buf, uint32_t samples);

/**
 * @brief 停止 I2S DMA 传输
 */
void i2s_stop(void);

/**
 * @brief 等待 DMA 传输完成
 */
bool i2s_wait_complete(uint32_t timeout_ms);

#endif /* I2S_DRIVER_H_ */
