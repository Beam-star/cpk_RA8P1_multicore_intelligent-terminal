/**
 ******************************************************************************
 * @file    pdm_audio.h
 * @brief   PDM 麦克风阵列音频驱动头文件 (3路同步采集)
 *
 * 硬件:
 *   - 3路 MEMS PDM 麦克风 (板载线性阵列, d=30mm)
 *   - RA8P1 内置 PDM 接口 (硬件 PDM→PCM 解调)
 *   - DMA 多通道传输 (环形缓冲)
 *
 * 采样参数:
 *   - 采样率: 16kHz
 *   - 分辨率: 16-bit PCM
 *   - 通道数: 3 (Mic0/Mic1/Mic2)
 *   - 处理帧长: 512 采样点 (32ms/帧)
 *
 * 数据路由:
 *   - Mic0 (中置) → 会议录音主声道 → WAV 编码
 *   - Mic0+Mic1+Mic2 → 声源定位 (GCC-PHAT)
 ******************************************************************************
 */

#ifndef PDM_AUDIO_H_
#define PDM_AUDIO_H_

#include <stdint.h>
#include <stdbool.h>

/* ---- 音频参数 ---- */
#define PDM_SAMPLE_RATE         (16000)    /* 采样率 Hz */
#define PDM_CHANNELS            (3)        /* 通道数 */
#define PDM_PCM_BITS            (16)       /* PCM 位深 */
#define PDM_FRAME_SIZE          (512)      /* 每帧采样点数 */
#define PDM_BUFFER_COUNT        (4)        /* 环形缓冲数量 */

/* ---- 数据类型 ---- */

/** PDM 音频帧 (一帧 = 512 采样点 × 3 通道) */
typedef struct {
    int16_t  data[PDM_CHANNELS][PDM_FRAME_SIZE];  /* 3通道 × 512采样点 */
    uint32_t frame_id;                            /* 帧序号 (单调递增) */
    uint32_t timestamp_ms;                        /* 时间戳 (ms) */
} pdm_audio_frame_t;

/* ---- API ---- */

/**
 * @brief 初始化 PDM 麦克风阵列
 *
 * 流程:
 *   1. R_PDM_Open() — 打开 PDM 模块
 *   2. 配置 3 通道同步采集, 16kHz, 16-bit PCM
 *   3. 分配 DMA 环形缓冲区 (PDM_BUFFER_COUNT × PDM_FRAME_SIZE)
 *   4. 注册 DMA 完成中断回调 (pdma_callback)
 *   5. R_PDM_Start() — 启动连续采集
 */
bool pdm_audio_init(void);

/**
 * @brief 等待获取一帧音频数据 (阻塞)
 * @param frame  [out] 音频帧数据指针
 * @param timeout_ms  超时时间 (ms)
 * @return true=成功获取, false=超时
 */
bool pdm_audio_get_frame(pdm_audio_frame_t *frame, uint32_t timeout_ms);

/**
 * @brief 获取指定通道的最新 PCM 数据 (非阻塞)
 * @param channel  通道索引 (0/1/2)
 * @param buf      输出缓冲区
 * @param samples  采样点数
 * @return 实际读取的采样点数
 */
uint32_t pdm_audio_read_channel(uint8_t channel, int16_t *buf, uint32_t samples);

#endif /* PDM_AUDIO_H_ */
