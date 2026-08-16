/**
 ******************************************************************************
 * @file    avi_parser.h
 * @brief   AVI 容器解析器 (回放) — 提取 MJPEG '00dc' 帧
 *
 * 解析 avi_muxer.c 生成的 video-only AVI (单 MJPEG 流, 无音频无 idx1)。
 * 读取头部帧率字段 (dwMicroSecPerFrame) 供播放器按正确速度回放。
 ******************************************************************************
 */

#ifndef AVI_PARSER_H_
#define AVI_PARSER_H_

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    void    *file;
    uint32_t cur_offset;      /* 当前文件偏移 (手动维护, 不依赖 ftell) */
    uint32_t us_per_frame;    /* 每帧微秒 (回放节拍) */
    uint32_t width, height;
    uint32_t total_frames;
    uint32_t frames_read;
} avi_parser_t;

/** 打开 AVI 并解析头部 */
bool avi_parser_open(avi_parser_t *p, const char *path);

/**
 * @brief 读取下一帧 JPEG 数据
 * @param buf/jpeg_size  输出 JPEG 码流与长度
 * @return true=读得一帧, false=EOF/错误
 */
bool avi_parser_read_frame(avi_parser_t *p, uint8_t *buf, uint32_t buf_size,
                           uint32_t *jpeg_size);

/** 关闭并释放 */
void avi_parser_close(avi_parser_t *p);

#endif /* AVI_PARSER_H_ */
