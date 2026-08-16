/**
 ******************************************************************************
 * @file    jpeg_decoder.h
 * @brief   Baseline JPEG 软件解码器 (MJPEG 回放) — 头文件
 *
 * 解码 mjpeg_encoder.c 生成的 baseline JPEG (YCbCr 4:2:0, 8-bit, 3 分量),
 * 输出 RGB565。与编码器共享同一套 Annex K 量化表 / 霍夫曼表约定, 输入
 * 里带 DQT/DHT, 解码器从码流解析 (对标准 JPEG 也兼容)。
 ******************************************************************************
 */

#ifndef JPEG_DECODER_H_
#define JPEG_DECODER_H_

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief 解码一张 baseline JPEG 到 RGB565。
 *
 * @param jpeg         JPEG 码流 (含 SOI...EOI)
 * @param jpeg_size    码流字节数
 * @param rgb565_out   输出 RGB565 缓冲 (宽×高, 调用者分配 ≥ w*h*2 字节)
 * @param out_width    [out] 解码图像宽
 * @param out_height   [out] 解码图像高
 * @return true=成功, false=码流损坏或不支持
 */
bool jpeg_decode_rgb565(const uint8_t *jpeg, uint32_t jpeg_size,
                        uint16_t *rgb565_out,
                        uint32_t *out_width, uint32_t *out_height);

#endif /* JPEG_DECODER_H_ */
