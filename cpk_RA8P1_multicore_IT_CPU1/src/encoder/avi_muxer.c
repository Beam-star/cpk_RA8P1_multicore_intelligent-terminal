/**
 ******************************************************************************
 * @file    avi_muxer.c
 * @brief   AVI 容器封装器 — 视频专用 MJPEG (无音频流)
 *
 * 音频与视频分离: 本文件只封装视频 (MJPEG → 00dc chunk), 音频由 wav_encoder
 * 独立保存为 WAV。生成的 .avi 为单视频流 AVI, VLC / Windows Media Player
 * / RA8P1 解码器均可播放。
 *
 * 实现要点 (与 wav_encoder 一致, 已验证的可靠模式):
 *   - 头结构固定 224 字节, 在内存缓冲区里拼好后一次性 sd_card_fwrite,
 *     避免 FreeRTOS+FAT 多次小写入的 ftell 偏移不可靠问题。
 *   - 回填偏移全部硬编码 (见下方 #define), 不依赖 ff_ftell。
 *
 * RIFF 结构 (video-only):
 *   RIFF 'AVI '
 *   ├── LIST 'hdrl'  (avih + strl[vids/MJPG: strh + strf])
 *   └── LIST 'movi'  ('00dc' + size + JPEG data) × N
 ******************************************************************************
 */

#include "avi_muxer.h"
#include "sdhi_driver.h"
#include <stdio.h>
#include <string.h>

/* 头结构固定 224 字节; 各回填字段的绝对偏移 */
#define AVI_HEADER_SIZE       224
#define AVI_RIFF_SIZE_OFF     4     /* "RIFF" 后的 riff size                 */
#define AVI_MICROSEC_PER_FRAME_OFF 32 /* avih.dwMicroSecPerFrame (回放帧率)  */
#define AVI_AVIH_FRAMES_OFF   48    /* avih.dwTotalFrames                    */
#define AVI_STRH_SCALE_OFF    128   /* strh.dwScale                          */
#define AVI_STRH_RATE_OFF     132   /* strh.dwRate                           */
#define AVI_STRH_LENGTH_OFF   140   /* strh.dwLength                         */
#define AVI_MOVI_SIZE_OFF     216   /* movi LIST 的 size 字段                */

static void    *g_file      = NULL;
static uint32_t g_frames    = 0;    /* 已写入视频帧数                       */
static uint32_t g_movi_data = 0;    /* movi 数据区累计字节数                */

/* ---- 头缓冲区写入 (小端) ---- */
static void put_u32(uint8_t *p, uint32_t *o, uint32_t v)
{
    p[(*o)++] = (uint8_t)(v & 0xFF);
    p[(*o)++] = (uint8_t)((v >> 8) & 0xFF);
    p[(*o)++] = (uint8_t)((v >> 16) & 0xFF);
    p[(*o)++] = (uint8_t)((v >> 24) & 0xFF);
}
static void put_u16(uint8_t *p, uint32_t *o, uint16_t v)
{
    p[(*o)++] = (uint8_t)(v & 0xFF);
    p[(*o)++] = (uint8_t)((v >> 8) & 0xFF);
}
static void put_tag(uint8_t *p, uint32_t *o, const char *s)
{
    p[(*o)++] = (uint8_t)s[0];
    p[(*o)++] = (uint8_t)s[1];
    p[(*o)++] = (uint8_t)s[2];
    p[(*o)++] = (uint8_t)s[3];
}

/* 数据区写入辅助 (运行时) */
static void w_u32(uint32_t v) { sd_card_fwrite(g_file, &v, 4); }
static void w_tag(const char *s) { sd_card_fwrite(g_file, s, 4); }

void avi_muxer_create(void *file)
{
    g_file = file;
    g_frames = 0;
    g_movi_data = 0;

    uint8_t h[AVI_HEADER_SIZE];
    uint32_t o = 0;

    /* ---- RIFF 头 ---- */
    put_tag(h, &o, "RIFF");
    put_u32(h, &o, 0);                       /* riff size (偏移 4) */
    put_tag(h, &o, "AVI ");

    /* ---- hdrl LIST ---- */
    put_tag(h, &o, "LIST");
    put_u32(h, &o, 192);                     /* hdrl size */
    put_tag(h, &o, "hdrl");

    /* ---- avih (56B) ---- */
    put_tag(h, &o, "avih");
    put_u32(h, &o, 56);
    put_u32(h, &o, 1000000u / AVI_VIDEO_FPS); /* dwMicroSecPerFrame */
    put_u32(h, &o, 0);                        /* dwMaxBytesPerSec */
    put_u32(h, &o, 0);                        /* dwPaddingGranularity */
    put_u32(h, &o, 0);                        /* dwFlags */
    put_u32(h, &o, 0);                        /* dwTotalFrames (偏移 48) */
    put_u32(h, &o, 0);                        /* dwInitialFrames */
    put_u32(h, &o, 1);                        /* dwStreams = 1 */
    put_u32(h, &o, 0);                        /* dwSuggestedBufferSize */
    put_u32(h, &o, AVI_VIDEO_WIDTH);
    put_u32(h, &o, AVI_VIDEO_HEIGHT);
    put_u32(h, &o, 0); put_u32(h, &o, 0); put_u32(h, &o, 0); put_u32(h, &o, 0);

    /* ---- 视频 strl LIST ---- */
    put_tag(h, &o, "LIST");
    put_u32(h, &o, 116);                     /* strl size */
    put_tag(h, &o, "strl");

    /* strh (56B) */
    put_tag(h, &o, "strh");
    put_u32(h, &o, 56);
    put_tag(h, &o, "vids");
    put_tag(h, &o, "MJPG");
    put_u32(h, &o, 0);                        /* dwFlags */
    put_u16(h, &o, 0);                        /* wPriority */
    put_u16(h, &o, 0);                        /* wLanguage */
    put_u32(h, &o, 0);                        /* dwInitialFrames */
    put_u32(h, &o, 1);                        /* dwScale */
    put_u32(h, &o, AVI_VIDEO_FPS);            /* dwRate */
    put_u32(h, &o, 0);                        /* dwStart */
    put_u32(h, &o, 0);                        /* dwLength (偏移 140) */
    put_u32(h, &o, 0);                        /* dwSuggestedBufferSize */
    put_u32(h, &o, 0xFFFFFFFFu);              /* dwQuality */
    put_u32(h, &o, 0);                        /* dwSampleSize */
    put_u16(h, &o, 0); put_u16(h, &o, 0);     /* rcFrame left/top */
    put_u16(h, &o, AVI_VIDEO_WIDTH); put_u16(h, &o, AVI_VIDEO_HEIGHT); /* right/bottom */

    /* strf BITMAPINFOHEADER (40B) */
    put_tag(h, &o, "strf");
    put_u32(h, &o, 40);                        /* strf chunk size = BITMAPINFOHEADER size */
    put_u32(h, &o, 40);                        /* biSize = 40  (was MISSING: header was 4B short, breaking the MJPG codec ID) */
    put_u32(h, &o, AVI_VIDEO_WIDTH);           /* biWidth */
    put_u32(h, &o, AVI_VIDEO_HEIGHT);          /* biHeight */
    put_u16(h, &o, 1);                        /* biPlanes */
    put_u16(h, &o, 24);                       /* biBitCount */
    put_tag(h, &o, "MJPG");                   /* biCompression */
    put_u32(h, &o, 0);                        /* biSizeImage */
    put_u32(h, &o, 0); put_u32(h, &o, 0);     /* biX/Y PelsPerMeter */
    put_u32(h, &o, 0); put_u32(h, &o, 0);     /* biClrUsed/Important */

    /* ---- movi LIST ---- */
    put_tag(h, &o, "LIST");
    put_u32(h, &o, 0);                        /* movi size (偏移 216) */
    put_tag(h, &o, "movi");

    /* 一次性写入 224 字节头, 然后 fseek 到 224 刷盘 (与 wav_encoder 同款) */
    sd_card_fwrite(g_file, h, AVI_HEADER_SIZE);
    sd_card_fseek(g_file, AVI_HEADER_SIZE);

    printf("[AVI] Header written (224 B)\r\n");
}

void avi_muxer_write_video_frame(void *file, const uint8_t *jpeg_data,
                                 uint32_t jpeg_size, bool is_keyframe)
{
    (void)is_keyframe;                 /* MJPEG 每帧都是关键帧 */
    if (!file || !jpeg_data || jpeg_size == 0) return;

    /* '00dc' chunk: tag + size + data (+ 奇数时 1 字节填充) */
    w_tag("00dc");
    w_u32(jpeg_size);
    sd_card_fwrite(file, jpeg_data, jpeg_size);
    if (jpeg_size & 1) {
        uint8_t pad = 0;
        sd_card_fwrite(file, &pad, 1);
    }

    g_frames++;
    g_movi_data += 8 + jpeg_size + (jpeg_size & 1);
}

/* 音频流已分离到独立 WAV, 此函数保留为 no-op 以兼容旧接口 */
void avi_muxer_write_audio_chunk(void *file, const int16_t *pcm_data,
                                 uint32_t pcm_size)
{
    (void)file; (void)pcm_data; (void)pcm_size;
}

void avi_muxer_finalize(void *file, uint32_t total_frames, uint32_t duration_ms)
{
    (void)total_frames;
    if (!file) return;

    uint32_t frames = g_frames;

    /* 关键: 编码器丢帧, 实际帧数远小于 AVI_VIDEO_FPS×录制时长。若仍按固定
     * 10fps 播放, 视频会"快进" (录 4s 播 <1s)。这里用真实录制时长反推每帧
     * 微秒数, 使回放时长 == 录制时长。 */
    uint32_t us_per_frame;
    if (frames > 0 && duration_ms > 0) {
        us_per_frame = (uint32_t)((uint64_t)duration_ms * 1000u / frames);
    } else {
        us_per_frame = 1000000u / AVI_VIDEO_FPS;   /* 回退 10fps */
    }

    /* strh.dwRate/dwScale 表示真实帧率 (fps = dwRate/1000)。 */
    uint32_t dw_rate = (frames > 0 && duration_ms > 0)
        ? (uint32_t)((uint64_t)frames * 1000000u / duration_ms)   /* fps × 1000 */
        : AVI_VIDEO_FPS * 1000u;

    /* movi LIST size 字段 = "movi"(4) + movi_data */
    uint32_t movi_size = 4 + g_movi_data;
    /* riff size = "AVI "(4) + hdrl LIST(200) + movi LIST(12) + movi_data */
    uint32_t riff_size = 4 + 200 + 12 + g_movi_data;

    printf("[AVI] Finalizing: %lu frames, %lu ms, %lu us/frame (%lu.%03lu fps), %lu movi bytes\r\n",
           (unsigned long)frames, (unsigned long)duration_ms,
           (unsigned long)us_per_frame,
           (unsigned long)(dw_rate / 1000u), (unsigned long)(dw_rate % 1000u),
           (unsigned long)g_movi_data);

    /* 回填 (硬编码偏移) */
    sd_card_fseek(file, AVI_MOVI_SIZE_OFF);
    w_u32(movi_size);

    sd_card_fseek(file, AVI_RIFF_SIZE_OFF);
    w_u32(riff_size);

    sd_card_fseek(file, AVI_AVIH_FRAMES_OFF);
    w_u32(frames);

    sd_card_fseek(file, AVI_STRH_LENGTH_OFF);
    w_u32(frames);

    /* 帧率字段: 按真实时长回填, 修正回放速度 */
    sd_card_fseek(file, AVI_MICROSEC_PER_FRAME_OFF);
    w_u32(us_per_frame);

    sd_card_fseek(file, AVI_STRH_SCALE_OFF);
    w_u32(1000u);

    sd_card_fseek(file, AVI_STRH_RATE_OFF);
    w_u32(dw_rate);

    /* 回到文件末尾 */
    sd_card_fseek(file, AVI_HEADER_SIZE + g_movi_data);

    /* 诊断: 读回头部 12 字节验证 RIFF/AVI 签名与 riff_size 是否回填成功 */
    {
        uint8_t sig[12];
        sd_card_fseek(file, 0);
        sd_card_fread(file, sig, 12);
        sd_card_fseek(file, AVI_HEADER_SIZE + g_movi_data);
        uint32_t riff_read = (uint32_t)sig[4] | ((uint32_t)sig[5] << 8)
                           | ((uint32_t)sig[6] << 16) | ((uint32_t)sig[7] << 24);
        printf("[AVI] hdr=%c%c%c%c riff=%lu %c%c%c%c\r\n",
               sig[0], sig[1], sig[2], sig[3], (unsigned long)riff_read,
               sig[8], sig[9], sig[10], sig[11]);
    }

    printf("[AVI] File finalized (%lu frames)\r\n", (unsigned long)frames);
}
