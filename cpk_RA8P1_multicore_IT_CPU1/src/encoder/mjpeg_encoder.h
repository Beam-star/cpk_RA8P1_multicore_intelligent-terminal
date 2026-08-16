/**
 ******************************************************************************
 * @file    mjpeg_encoder.h
 * @brief   MJPEG 软件视频编码器头文件
 *
 * 编码算法: Motion JPEG (逐帧独立 JPEG 压缩)
 * 输入:     RGB565 640×480 原始帧 (614KB)
 * 输出:     JPEG 压缩码流 (~30-80KB/帧, Q50-Q90)
 * 编码耗时: ~50-100ms/帧 @ Cortex-M33 (取决于 Q 因子和分辨率)
 *
 * 优化:
 *   - CMSIS-DSP 定点 DCT (arm_dct4_q15)
 *   - 预存标准霍夫曼表 (Flash)
 *   - 8×8 块流水线处理
 ******************************************************************************
 */

#ifndef MJPEG_ENCODER_H_
#define MJPEG_ENCODER_H_

#include <stdint.h>
#include <stdbool.h>

/* ---- 编码参数 ----
 * 输入是 CPU0 共享的 640×480 RGB565 帧, 内部做 2×2 下采样到 320×240 再编码。
 * 分辨率减半使 8×8 块数减少 4× (7200→1800), DCT 耗时成比例下降, 帧率提升约 4×。 */
#define MJPEG_SRC_WIDTH      640     /* 输入帧宽 (CPU0 共享 SDRAM) */
#define MJPEG_SRC_HEIGHT     480     /* 输入帧高 */
#define MJPEG_DS             2       /* 2×2 下采样因子 */
#define MJPEG_WIDTH          (MJPEG_SRC_WIDTH / MJPEG_DS)    /* 320 编码宽 */
#define MJPEG_HEIGHT         (MJPEG_SRC_HEIGHT / MJPEG_DS)   /* 240 编码高 */
#define MJPEG_QUALITY_MIN    50      /* 最高压缩, 最小文件 */
#define MJPEG_QUALITY_MAX    90      /* 最高质量, 最大文件 */
#define MJPEG_QUALITY_DEFAULT 75     /* 默认平衡 */

/* ---- API ---- */

/**
 * @brief 初始化 MJPEG 编码器
 *
 * 预分配:
 *   - YCbCr 色彩空间转换缓冲区
 *   - DCT 系数处理缓冲区 (8×8 块)
 *   - 霍夫曼编码查找表 (Flash 预存)
 *   - JPEG 码流输出缓冲区 (128KB)
 */
void mjpeg_encoder_init(void);

/**
 * @brief 编码一帧 RGB565 图像为 JPEG
 *
 * @param rgb565_data  输入: 640×480 RGB565 原始帧
 * @param jpeg_buf     输出: JPEG 压缩数据缓冲区
 * @param jpeg_buf_size 输出缓冲区大小 (建议 ≥ 128KB)
 * @param quality      JPEG 质量因子 (50-90)
 * @return JPEG 压缩数据大小 (字节), 0=编码失败
 *
 * 编码流程:
 *   1. RGB565 → YCbCr 4:2:0 色彩空间转换
 *   2. 8×8 块分割
 *   3. 逐块 DCT 变换 (CMSIS-DSP arm_dct4_q15)
 *   4. 量化 (标准 JPEG 量化表, Q 因子缩放)
 *   5. Zig-Zag 扫描
 *   6. 差分 DC + 行程编码 AC
 *   7. 霍夫曼熵编码 → 输出 JPEG 码流
 */
uint32_t mjpeg_encode_frame(const uint16_t *rgb565_data,
                            uint8_t *jpeg_buf,
                            uint32_t jpeg_buf_size,
                            uint8_t quality);

/**
 * @brief 设置编码质量 (运行时可变)
 */
void mjpeg_set_quality(uint8_t quality);

/**
 * @brief 获取当前编码质量
 */
uint8_t mjpeg_get_quality(void);

#endif /* MJPEG_ENCODER_H_ */
