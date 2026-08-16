/**
 ******************************************************************************
 * @file    wav_encoder.c
 * @brief   WAV 音频编码器实现
 ******************************************************************************
 */

#include "wav_encoder.h"
#include "sdhi_driver.h"
#include <stdio.h>
#include <string.h>

void wav_write_header(void *file)
{
    wav_header_t header;
    memset(&header, 0, sizeof(header));

    /* RIFF header */
    memcpy(header.riff_id, "RIFF", 4);
    header.riff_size = 0;  /* 录制结束后回填 */
    memcpy(header.wave_id, "WAVE", 4);

    /* fmt chunk */
    memcpy(header.fmt_id, "fmt ", 4);
    header.fmt_size        = 16;
    header.audio_format    = 1;          /* PCM */
    header.num_channels    = WAV_NUM_CHANNELS;
    header.sample_rate     = WAV_SAMPLE_RATE;
    header.byte_rate       = WAV_BYTE_RATE;
    header.block_align     = WAV_NUM_CHANNELS * WAV_BITS_PER_SAMPLE / 8;
    header.bits_per_sample = WAV_BITS_PER_SAMPLE;

    /* data chunk */
    memcpy(header.data_id, "data", 4);
    header.data_size = 0;  /* 录制结束后回填 */

    /* 写入 44 字节 WAV 头 */
    sd_card_fwrite(file, &header, sizeof(header));
    sd_card_fseek(file, sizeof(header));

    printf("[WAV] Header written (16kHz, 16-bit, mono)\r\n");
}

void wav_update_header(void *file, uint32_t total_pcm_bytes)
{
    /* 回填 RIFF 总大小: 文件大小 - 8 */
    uint32_t riff_size = sizeof(wav_header_t) + total_pcm_bytes - 8;
    sd_card_fseek(file, 4);
    sd_card_fwrite(file, &riff_size, 4);

    /* 回填 data chunk 大小 */
    sd_card_fseek(file, 40);
    sd_card_fwrite(file, &total_pcm_bytes, 4);

    printf("[WAV] Header updated: PCM=%lu bytes\r\n",
           (unsigned long)total_pcm_bytes);
}

void wav_append_pcm(void *file, const int16_t *pcm_data, uint32_t samples)
{
    /* PCM 数据直接写入 SD 卡 (通过聚合缓冲区) */
    uint32_t bytes = samples * sizeof(int16_t);
    sd_card_fwrite(file, pcm_data, bytes);
}
