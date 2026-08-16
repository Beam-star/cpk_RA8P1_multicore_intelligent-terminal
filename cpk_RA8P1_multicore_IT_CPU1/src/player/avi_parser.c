/**
 ******************************************************************************
 * @file    avi_parser.c
 * @brief   AVI 容器解析器实现 (见 avi_parser.h)
 ******************************************************************************
 */

#include "avi_parser.h"
#include "driver/sd_card/sdhi_driver.h"
#include <string.h>
#include <stdio.h>

/* 与 avi_muxer.c 的头部布局一致 (biSize 已修复, 头固定 224 字节) */
#define AVI_HEADER_SIZE       224
#define AVI_US_PER_FRAME_OFF  32    /* avih.dwMicroSecPerFrame */
#define AVI_TOTAL_FRAMES_OFF  48    /* avih.dwTotalFrames      */
#define AVI_WIDTH_OFF         64    /* avih.dwWidth            */
#define AVI_HEIGHT_OFF        68    /* avih.dwHeight           */

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool avi_parser_open(avi_parser_t *p, const char *path)
{
    if (!p || !path) return false;
    memset(p, 0, sizeof(*p));

    p->file = sd_card_fopen(path, "r");
    if (!p->file) {
        printf("[AVI] open failed: %s\r\n", path);
        return false;
    }

    uint8_t hdr[AVI_HEADER_SIZE];
    if (sd_card_fread(p->file, hdr, AVI_HEADER_SIZE) != AVI_HEADER_SIZE) {
        printf("[AVI] header read failed\r\n");
        sd_card_fclose(p->file);
        p->file = NULL;
        return false;
    }

    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "AVI ", 4) != 0) {
        printf("[AVI] not an AVI file\r\n");
        sd_card_fclose(p->file);
        p->file = NULL;
        return false;
    }

    p->us_per_frame = rd_u32(hdr + AVI_US_PER_FRAME_OFF);
    p->total_frames = rd_u32(hdr + AVI_TOTAL_FRAMES_OFF);
    p->width        = rd_u32(hdr + AVI_WIDTH_OFF);
    p->height       = rd_u32(hdr + AVI_HEIGHT_OFF);
    p->cur_offset   = AVI_HEADER_SIZE;
    p->frames_read  = 0;

    if (p->us_per_frame == 0) p->us_per_frame = 100000u;   /* 回退 10fps */

    printf("[AVI] %s: %lux%lu, %lu us/frame, %lu frames\r\n",
           path, (unsigned long)p->width, (unsigned long)p->height,
           (unsigned long)p->us_per_frame, (unsigned long)p->total_frames);
    return true;
}

bool avi_parser_read_frame(avi_parser_t *p, uint8_t *buf, uint32_t buf_size,
                           uint32_t *jpeg_size)
{
    if (!p || !p->file || !buf) return false;

    for (;;) {
        uint8_t tag[4], sz[4];
        if (sd_card_fread(p->file, tag, 4) != 4) return false;   /* EOF */
        if (sd_card_fread(p->file, sz, 4) != 4) return false;
        uint32_t size = rd_u32(sz);
        p->cur_offset += 8;

        if (memcmp(tag, "00dc", 4) == 0) {
            /* 视频帧 (JPEG) */
            if (size > buf_size) {
                /* 超缓冲: 跳过该帧 */
                sd_card_fseek(p->file, p->cur_offset + size + (size & 1u));
                p->cur_offset += size + (size & 1u);
                continue;
            }
            if (sd_card_fread(p->file, buf, size) != size) return false;
            if (size & 1u) {
                uint8_t pad;
                sd_card_fread(p->file, &pad, 1);
            }
            p->cur_offset += size + (size & 1u);
            *jpeg_size = size;
            p->frames_read++;
            return true;
        }

        /* 非视频 chunk (LIST/idx1 等) → 跳过 */
        sd_card_fseek(p->file, p->cur_offset + size + (size & 1u));
        p->cur_offset += size + (size & 1u);
    }
}

void avi_parser_close(avi_parser_t *p)
{
    if (p && p->file) {
        sd_card_fclose(p->file);
        p->file = NULL;
    }
}
