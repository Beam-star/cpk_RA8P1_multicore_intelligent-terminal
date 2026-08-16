/**
 ******************************************************************************
 * @file    avi_muxer.h
 * @brief   AVI 容器封装器头文件 (MJPEG + PCM → AVI)
 *
 * AVI (Audio Video Interleaved) 基于 RIFF 块结构:
 *   RIFF 'AVI '
 *   ├── LIST 'hdrl' (文件头: 视频/音频流格式描述)
 *   │   ├── 'avih' (AVI 主头: 帧率/总帧数/宽高/标志)
 *   │   ├── LIST 'strl' (视频流: MJPEG 'MJPG')
 *   │   │   ├── 'strh' (流头)
 *   │   │   └── 'strf' (流格式: BITMAPINFOHEADER)
 *   │   └── LIST 'strl' (音频流: PCM)
 *   │       ├── 'strh' (流头)
 *   │       └── 'strf' (流格式: WAVEFORMATEX)
 *   ├── LIST 'movi' (交错音视频数据)
 *   │   ├── '00dc' (视频帧0: JPEG data)
 *   │   ├── '01wb' (音频块0: PCM data)
 *   │   ├── '00dc' (视频帧1: JPEG data)
 *   │   ├── '01wb' (音频块1: PCM data)
 *   │   └── ...
 *   └── 'idx1' (可选 AVI 索引)
 *
 * 兼容性: VLC, Windows Media Player, 手机相册等全平台原生支持
 ******************************************************************************
 */

#ifndef AVI_MUXER_H_
#define AVI_MUXER_H_

#include <stdint.h>
#include <stdbool.h>

/* ---- AVI 录制参数 ---- */
#define AVI_VIDEO_FPS           (10)       /* 视频帧率 (仅作回退值, finalize 按真实时长重算) */
#define AVI_VIDEO_WIDTH         (320)      /* 与 mjpeg_encoder 下采样后的编码尺寸一致 */
#define AVI_VIDEO_HEIGHT        (240)
#define AVI_AUDIO_SAMPLE_RATE   (16000)
#define AVI_AUDIO_BITS          (16)
#define AVI_AUDIO_CHANNELS      (1)

/* ---- AVI 块 ID ---- */
#define AVI_VIDEO_CHUNK_ID      "00dc"     /* 视频数据块 */
#define AVI_AUDIO_CHUNK_ID      "01wb"     /* 音频数据块 */
#define AVI_LIST_ID             "LIST"
#define AVI_RIFF_ID             "RIFF"
#define AVI_HDRL_ID             "hdrl"
#define AVI_MOVI_ID             "movi"

/* ---- API ---- */

/**
 * @brief 创建 AVI 文件并写入完整头 (hdrl)
 *
 * @param file  SD 卡文件句柄
 *
 * 写入顺序:
 *   1. RIFF 头 + AVI 签名
 *   2. hdrl LIST (avih + 视频 strl + 音频 strl)
 *   3. movi LIST 起始标记
 *   → 文件指针定位到 movi 数据区起始位置
 */
void avi_muxer_create(void *file);

/**
 * @brief 追加一个视频帧 (JPEG 数据) 到 AVI
 * @param file       SD 卡文件句柄
 * @param jpeg_data  JPEG 压缩数据
 * @param jpeg_size  JPEG 数据大小 (字节)
 * @param is_keyframe 是否为关键帧 (MJPEG 每帧都是关键帧)
 */
void avi_muxer_write_video_frame(void *file, const uint8_t *jpeg_data,
                                 uint32_t jpeg_size, bool is_keyframe);

/**
 * @brief 追加一个音频块 (PCM 数据) 到 AVI
 * @param file       SD 卡文件句柄
 * @param pcm_data   PCM 音频数据
 * @param pcm_size   PCM 数据大小 (字节)
 */
void avi_muxer_write_audio_chunk(void *file, const int16_t *pcm_data,
                                 uint32_t pcm_size);

/**
 * @brief 完成 AVI 文件 (回填 RIFF 大小 + 帧数 + 写入 idx1 索引)
 * @param file          SD 卡文件句柄
 * @param total_frames  总视频帧数
 * @param total_audio_bytes  总音频数据字节数
 */
void avi_muxer_finalize(void *file, uint32_t total_frames,
                        uint32_t total_audio_bytes);

#endif /* AVI_MUXER_H_ */
