/**
 ******************************************************************************
 * @file    wav_encoder.h
 * @brief   WAV 音频编码器头文件 (PCM → WAV 封装)
 *
 * 格式: RIFF WAVE, PCM 16-bit, 单声道, 16kHz
 * 结构: 44 字节 RIFF 头 + PCM 原始数据
 * CPU占用: 极低 (仅 memcpy + 头写入)
 *
 * WAV 文件结构:
 *   RIFF Header (12B):  'RIFF' + file_size + 'WAVE'
 *   fmt  Chunk  (24B):  'fmt ' + chunk_size + PCM format info
 *   data Chunk  (8B):   'data' + data_size
 *   data         (nB):   PCM samples (16-bit interleaved)
 ******************************************************************************
 */

#ifndef WAV_ENCODER_H_
#define WAV_ENCODER_H_

#include <stdint.h>
#include <stdbool.h>

/* ---- WAV 格式参数 ---- */
#define WAV_SAMPLE_RATE        (16000)
#define WAV_BITS_PER_SAMPLE    (16)
#define WAV_NUM_CHANNELS       (1)
#define WAV_BYTE_RATE          (WAV_SAMPLE_RATE * WAV_NUM_CHANNELS * WAV_BITS_PER_SAMPLE / 8)  /* 32000 */

/* ---- WAV 头结构 (44 字节) ---- */
#pragma pack(push, 1)
typedef struct {
    /* RIFF Header */
    uint8_t  riff_id[4];       /* "RIFF" */
    uint32_t riff_size;        /* 文件大小 - 8 */
    uint8_t  wave_id[4];       /* "WAVE" */
    /* fmt Chunk */
    uint8_t  fmt_id[4];        /* "fmt " */
    uint32_t fmt_size;         /* 16 (PCM) */
    uint16_t audio_format;     /* 1 = PCM */
    uint16_t num_channels;     /* 1 = Mono */
    uint32_t sample_rate;      /* 16000 */
    uint32_t byte_rate;        /* 32000 */
    uint16_t block_align;      /* 2 (= num_channels * bits_per_sample/8) */
    uint16_t bits_per_sample;  /* 16 */
    /* data Chunk */
    uint8_t  data_id[4];       /* "data" */
    uint32_t data_size;        /* PCM 数据大小 (字节) */
} wav_header_t;
#pragma pack(pop)

/* ---- API ---- */

/**
 * @brief 写入 WAV 文件头
 * @param file  文件句柄 (由 sd_card_fopen 返回)
 */
void wav_write_header(void *file);

/**
 * @brief 更新 WAV 文件头中的大小字段 (录制结束时调用)
 * @param file  文件句柄
 * @param total_pcm_bytes  PCM 数据总字节数
 */
void wav_update_header(void *file, uint32_t total_pcm_bytes);

/**
 * @brief 追加 PCM 音频数据到 WAV 文件
 * @param file       文件句柄
 * @param pcm_data   PCM 音频数据 (16-bit, 单声道)
 * @param samples    采样点数
 */
void wav_append_pcm(void *file, const int16_t *pcm_data, uint32_t samples);

#endif /* WAV_ENCODER_H_ */
