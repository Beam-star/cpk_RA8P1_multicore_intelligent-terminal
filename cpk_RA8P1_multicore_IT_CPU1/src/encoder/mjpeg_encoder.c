/**
 ******************************************************************************
 * @file    mjpeg_encoder.c
 * @brief   Baseline JPEG 软件编码器 (MJPEG) — 完整实现
 *
 * 标准:     ISO/IEC 10918-1 (ITU-T T.81) baseline sequential DCT
 * 色彩空间: YCbCr 4:2:0 (Y 640×480, Cb/Cr 320×240)
 * 输入:     RGB565 640×480 (row-major)
 * 输出:     标准 baseline JPEG 码流 (PC 播放器 / RA8P1 解码器均可解析)
 *
 * 实现要点:
 *   - 标准 Annex K 量化表 + 标准 Annex K 霍夫曼表 (预存)
 *   - 可分离浮点 DCT (正确性优先; 后续可换定点/Helium 优化)
 *   - 按 MCU (16×16) 逐块转换编码, 无需整帧 YCbCr 缓冲, 省 SDRAM
 *
 * 性能 (Cortex-M33 @ 无 DSP 库):
 *   640×480 单帧 ≈ 28800 个 8×8 块, 浮点 DCT 下约 0.3-0.5s/帧
 *   → 2-3 fps @ Q75。满足"流畅保存"的低帧率要求; 可降分辨率提速。
 ******************************************************************************
 */

#include "mjpeg_encoder.h"
#include <string.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"

/* ======================================================================== */
/*  标准 JPEG 霍夫曼表 (Annex K)                                              */
/* ======================================================================== */

static const uint8_t kDcLumaBits[16]   = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t kDcLumaVal[12]    = {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t kDcChromaBits[16] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t kDcChromaVal[12]  = {0,1,2,3,4,5,6,7,8,9,10,11};

static const uint8_t kAcLumaBits[16] = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const uint8_t kAcLumaVal[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
    0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

static const uint8_t kAcChromaBits[16] = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
static const uint8_t kAcChromaVal[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
    0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
    0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
    0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
    0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
    0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

/* ======================================================================== */
/*  标准量化表 (Annex K) — 亮度 + 色度                                        */
/* ======================================================================== */

static const uint8_t kQuantLuma[64] = {
    16,11,10,16,24,40,51,61,
    12,12,14,19,26,58,60,55,
    14,13,16,24,40,57,69,56,
    14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77,
    24,35,55,64,81,104,113,92,
    49,64,78,87,103,121,120,101,
    72,92,95,98,112,100,103,99
};

static const uint8_t kQuantChroma[64] = {
    17,18,24,47,99,99,99,99,
    18,21,26,66,99,99,99,99,
    24,26,56,99,99,99,99,99,
    47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99
};

/* Zig-Zag 扫描顺序 (JPEG 标准) */
static const uint8_t kZigzag[64] = {
    0, 1, 8,16, 9, 2, 3,10,
   17,24,32,25,18,11, 4, 5,
   12,19,26,33,40,48,41,34,
   27,20,13, 6, 7,14,21,28,
   35,42,49,56,57,50,43,36,
   29,22,15,23,30,37,44,51,
   58,59,52,45,38,31,39,46,
   53,60,61,54,47,55,62,63
};

/* ======================================================================== */
/*  霍夫曼表 (symbol → code / length)                                         */
/* ======================================================================== */

typedef struct {
    uint32_t code[256];
    uint8_t  size[256];
} huff_table_t;

static huff_table_t s_huff_dc_luma, s_huff_dc_chroma;
static huff_table_t s_huff_ac_luma, s_huff_ac_chroma;

static void huff_build(huff_table_t *ht, const uint8_t *bits, const uint8_t *vals)
{
    uint32_t code = 0;
    int k = 0;
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < bits[i]; j++) {
            uint8_t sym = vals[k++];
            ht->code[sym] = code;
            ht->size[sym] = (uint8_t)(i + 1);
            code++;
        }
        code <<= 1;
    }
}

/* ======================================================================== */
/*  位写入器 (JPEG: MSB-first, 0xFF 字节填充)                                 */
/* ======================================================================== */

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t pos;
    uint32_t acc;
    int      nbits;
} bitw_t;

static void bitw_init(bitw_t *bw, uint8_t *buf, uint32_t cap)
{
    bw->buf = buf; bw->cap = cap; bw->pos = 0; bw->acc = 0; bw->nbits = 0;
}

static void bitw_byte(bitw_t *bw, uint8_t b)
{
    if (bw->pos < bw->cap) bw->buf[bw->pos++] = b;
}

static void bitw_write(bitw_t *bw, uint32_t code, int nbits)
{
    while (nbits > 0) {
        int space = 8 - bw->nbits;
        int take  = (nbits < space) ? nbits : space;
        int shift = nbits - take;
        bw->acc = (bw->acc << take) | ((code >> shift) & ((1u << take) - 1));
        bw->nbits += take;
        nbits -= take;
        if (bw->nbits == 8) {
            uint8_t b = (uint8_t)(bw->acc & 0xFF);
            bitw_byte(bw, b);
            if (b == 0xFF) bitw_byte(bw, 0x00);   /* byte stuffing */
            bw->acc = 0; bw->nbits = 0;
        }
    }
}

static void bitw_flush(bitw_t *bw)
{
    if (bw->nbits > 0) {
        /* 末字节用 1 填充 (JPEG 标准), 后续 EOI 为独立 marker */
        bw->acc = (bw->acc << (8 - bw->nbits)) | ((1u << (8 - bw->nbits)) - 1);
        bitw_byte(bw, (uint8_t)(bw->acc & 0xFF));
        bw->acc = 0; bw->nbits = 0;
    }
}

/* 写入 16-bit 大端 (marker / header) */
static void bitw_u16(bitw_t *bw, uint16_t v)
{
    bitw_byte(bw, (uint8_t)(v >> 8));
    bitw_byte(bw, (uint8_t)(v & 0xFF));
}

/* ======================================================================== */
/*  运行时状态                                                                */
/* ======================================================================== */

static uint8_t g_quality = MJPEG_QUALITY_DEFAULT;
static uint8_t g_quant_luma[64];
static uint8_t g_quant_chroma[64];
static float   g_quant_luma_inv[64];     /* 量化表倒数 (量化除法→乘法) */
static float   g_quant_chroma_inv[64];

void mjpeg_encoder_init(void)
{
    huff_build(&s_huff_dc_luma,   kDcLumaBits,   kDcLumaVal);
    huff_build(&s_huff_dc_chroma, kDcChromaBits, kDcChromaVal);
    huff_build(&s_huff_ac_luma,   kAcLumaBits,   kAcLumaVal);
    huff_build(&s_huff_ac_chroma, kAcChromaBits, kAcChromaVal);
    mjpeg_set_quality(g_quality);
    printf("[MJPEG] Encoder ready (%dx%d, Q%d)\r\n",
           MJPEG_WIDTH, MJPEG_HEIGHT, g_quality);
}

void mjpeg_set_quality(uint8_t quality)
{
    if (quality < MJPEG_QUALITY_MIN) quality = MJPEG_QUALITY_MIN;
    if (quality > MJPEG_QUALITY_MAX) quality = MJPEG_QUALITY_MAX;
    g_quality = quality;

    /* JPEG 标准质量缩放 */
    int scale = (quality < 50) ? (5000 / quality) : (200 - 2 * quality);
    for (int i = 0; i < 64; i++) {
        int q = (kQuantLuma[i] * scale + 50) / 100;
        if (q < 1) q = 1; if (q > 255) q = 255;
        g_quant_luma[i] = (uint8_t)q;
        g_quant_luma_inv[i] = 1.0f / (float)q;

        q = (kQuantChroma[i] * scale + 50) / 100;
        if (q < 1) q = 1; if (q > 255) q = 255;
        g_quant_chroma[i] = (uint8_t)q;
        g_quant_chroma_inv[i] = 1.0f / (float)q;
    }
}

uint8_t mjpeg_get_quality(void) { return g_quality; }

/* ======================================================================== */
/*  前向 DCT (可分离, 浮点)                                                   */
/* ======================================================================== */

/* cos((2*x+1) * u * PI / 16) */
static const float kCos[8][8] = {
    {1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f},
    {0.9808f, 0.8315f, 0.5556f, 0.1951f,-0.1951f,-0.5556f,-0.8315f,-0.9808f},
    {0.9239f, 0.3827f,-0.3827f,-0.9239f,-0.9239f,-0.3827f, 0.3827f, 0.9239f},
    {0.8315f,-0.1951f,-0.9808f,-0.5556f, 0.5556f, 0.9808f, 0.1951f,-0.8315f},
    {0.7071f,-0.7071f,-0.7071f, 0.7071f, 0.7071f,-0.7071f,-0.7071f, 0.7071f},
    {0.5556f,-0.9808f, 0.1951f, 0.8315f,-0.8315f,-0.1951f, 0.9808f,-0.5556f},
    {0.3827f,-0.9239f, 0.9239f,-0.3827f,-0.3827f, 0.9239f,-0.9239f, 0.3827f},
    {0.1951f,-0.5556f, 0.8315f,-0.9808f, 0.9808f,-0.8315f, 0.5556f,-0.1951f},
};

/* 对一个 8×8 块 (已 level-shift 到 [-128,127] 的 float) 做前向 DCT,
 * 结果写回 block (归一化已含 1/4 + C(u)C(v) 因子, 与量化表直接匹配)。 */
static void fdct_8x8(float block[64])
{
    float tmp[64];

    /* 行 DCT */
    for (int y = 0; y < 8; y++) {
        for (int u = 0; u < 8; u++) {
            float sum = 0.0f;
            for (int x = 0; x < 8; x++) {
                sum += block[y * 8 + x] * kCos[u][x];
            }
            tmp[y * 8 + u] = sum;
        }
    }

    /* 列 DCT + 归一化 */
    for (int v = 0; v < 8; v++) {
        for (int u = 0; u < 8; u++) {
            float sum = 0.0f;
            for (int y = 0; y < 8; y++) {
                sum += tmp[y * 8 + u] * kCos[v][y];
            }
            float cu = (u == 0) ? 0.70710678f : 1.0f;
            float cv = (v == 0) ? 0.70710678f : 1.0f;
            block[v * 8 + u] = sum * 0.25f * cu * cv;
        }
    }
}

/* ======================================================================== */
/*  RGB565 → YCbCr (Rec.601, 全范围 0..255, Y 偏移 -128)                      */
/* ======================================================================== */

/* 2×2 盒式平均后转换: 对 4 个 RGB565 源像素先按通道平均, 再做 5/6/5→8bit + YCbCr。
 * 与"逐像素转换再平均"近似等价, 但省去 3 次冗余的 YCbCr 运算。 */
static void rgb565_avg4_to_ycbcr(uint16_t p00, uint16_t p01, uint16_t p10, uint16_t p11,
                                 int *y, int *cb, int *cr)
{
    int r = ((p00 >> 11) & 0x1F) + ((p01 >> 11) & 0x1F)
          + ((p10 >> 11) & 0x1F) + ((p11 >> 11) & 0x1F);
    int g = ((p00 >> 5) & 0x3F) + ((p01 >> 5) & 0x3F)
          + ((p10 >> 5) & 0x3F) + ((p11 >> 5) & 0x3F);
    int b = (p00 & 0x1F) + (p01 & 0x1F) + (p10 & 0x1F) + (p11 & 0x1F);

    int ra = r >> 2, ga = g >> 2, ba = b >> 2;   /* ÷4 平均 */
    int r8 = (ra << 3) | (ra >> 2);              /* 5-bit → 8-bit */
    int g8 = (ga << 2) | (ga >> 4);              /* 6-bit → 8-bit */
    int b8 = (ba << 3) | (ba >> 2);

    *y  = ( 77 * r8 + 150 * g8 +  29 * b8) >> 8;
    *cb = ((-43 * r8 -  85 * g8 + 128 * b8) >> 8) + 128;
    *cr = ((128 * r8 - 107 * g8 -  21 * b8) >> 8) + 128;
}

/* ======================================================================== */
/*  系数编码                                                                  */
/* ======================================================================== */

static int category_of(int v)
{
    int abs = (v < 0) ? -v : v;
    int cat = 0;
    while (abs) { cat++; abs >>= 1; }
    return cat;
}

/* 编码一个 8×8 块的 DC 差分 */
static void encode_dc(bitw_t *bw, const huff_table_t *ht, int diff)
{
    int cat = category_of(diff);
    bitw_write(bw, ht->code[cat], ht->size[cat]);
    if (cat > 0) {
        if (diff < 0) diff = diff + (1 << cat) - 1;
        bitw_write(bw, (uint32_t)diff, cat);
    }
}

/* 编码一个 8×8 块 (zigzag 后) 的 AC 系数; 输入已按 zigzag 排列, 跳过 DC(index 0) */
static void encode_ac(bitw_t *bw, const huff_table_t *ht, const int zz[64])
{
    int run = 0;
    for (int i = 1; i < 64; i++) {
        int v = zz[i];
        if (v == 0) { run++; continue; }

        while (run >= 16) {   /* 长零游程 → ZRL (0xF0) */
            bitw_write(bw, ht->code[0xF0], ht->size[0xF0]);
            run -= 16;
        }

        int cat = category_of(v);
        int sym = (run << 4) | cat;
        bitw_write(bw, ht->code[sym], ht->size[sym]);
        if (v < 0) v = v + (1 << cat) - 1;
        bitw_write(bw, (uint32_t)v, cat);
        run = 0;
    }
    if (run > 0) {   /* 尾部零 → EOB (0x00) */
        bitw_write(bw, ht->code[0x00], ht->size[0x00]);
    }
}

/* 量化 + zigzag 一个 DCT 块, 结果存 zz[64] (含 DC) */
static void quantize_zigzag(const float block[64], const float inv_quant[64], int zz[64])
{
    int natural[64];
    for (int i = 0; i < 64; i++) {
        float q = block[i] * inv_quant[i];            /* 乘法代替除法 (除法 ~14× 慢) */
        int v = (int)(q + (q >= 0 ? 0.5f : -0.5f));   /* round */
        natural[i] = v;
    }
    for (int i = 0; i < 64; i++) {
        zz[i] = natural[kZigzag[i]];
    }
}

/* ======================================================================== */
/*  JPEG 头部                                                                 */
/* ======================================================================== */

static void write_marker(bitw_t *bw, uint16_t marker) { bitw_u16(bw, marker); }

static void write_dqt(bitw_t *bw)
{
    write_marker(bw, 0xFFDB);
    bitw_u16(bw, 2 + 1 + 64 + 1 + 64);   /* length */
    bitw_byte(bw, 0x00);                  /* table 0, 8-bit precision */
    /* JPEG DQT 规范要求按 zigzag 顺序存 64 个量化值; kQuantLuma 是自然序。 */
    for (int i = 0; i < 64; i++) bitw_byte(bw, g_quant_luma[kZigzag[i]]);
    bitw_byte(bw, 0x01);                  /* table 1, 8-bit */
    for (int i = 0; i < 64; i++) bitw_byte(bw, g_quant_chroma[kZigzag[i]]);
}

static void write_sof0(bitw_t *bw)
{
    write_marker(bw, 0xFFC0);
    bitw_u16(bw, 17);                     /* length */
    bitw_byte(bw, 8);                     /* precision */
    bitw_u16(bw, MJPEG_HEIGHT);           /* height */
    bitw_u16(bw, MJPEG_WIDTH);            /* width */
    bitw_byte(bw, 3);                     /* components */

    /* Y */
    bitw_byte(bw, 1);                     /* component id */
    bitw_byte(bw, 0x22);                  /* h=2, v=2 */
    bitw_byte(bw, 0);                     /* quant table 0 */
    /* Cb */
    bitw_byte(bw, 2);
    bitw_byte(bw, 0x11);                  /* h=1, v=1 */
    bitw_byte(bw, 1);
    /* Cr */
    bitw_byte(bw, 3);
    bitw_byte(bw, 0x11);
    bitw_byte(bw, 1);
}

static void write_dht(bitw_t *bw)
{
    /* 4 张表: DC luma, DC chroma, AC luma, AC chroma */
    const uint8_t *bits[4] = { kDcLumaBits, kDcChromaBits, kAcLumaBits, kAcChromaBits };
    const uint8_t *vals[4] = { kDcLumaVal, kDcChromaVal, kAcLumaVal, kAcChromaVal };
    const uint8_t  cls[4]  = { 0, 0, 1, 1 };   /* 0=DC, 1=AC */
    const uint8_t  ids[4]  = { 0, 1, 0, 1 };   /* luma=0, chroma=1 */

    for (int t = 0; t < 4; t++) {
        int nsym = 0;
        for (int i = 0; i < 16; i++) nsym += bits[t][i];

        write_marker(bw, 0xFFC4);
        bitw_u16(bw, (uint16_t)(3 + 16 + nsym));   /* length */
        bitw_byte(bw, (uint8_t)((cls[t] << 4) | ids[t]));
        for (int i = 0; i < 16; i++) bitw_byte(bw, bits[t][i]);
        for (int i = 0; i < nsym; i++) bitw_byte(bw, vals[t][i]);
    }
}

static void write_sos(bitw_t *bw)
{
    write_marker(bw, 0xFFDA);
    bitw_u16(bw, 12);                     /* length */
    bitw_byte(bw, 3);                     /* components */

    bitw_byte(bw, 1);                     /* Y: dc=0, ac=0 */
    bitw_byte(bw, 0x00);
    bitw_byte(bw, 2);                     /* Cb: dc=1, ac=1 */
    bitw_byte(bw, 0x11);
    bitw_byte(bw, 3);                     /* Cr: dc=1, ac=1 */
    bitw_byte(bw, 0x11);

    bitw_byte(bw, 0);                     /* Ss */
    bitw_byte(bw, 63);                    /* Se */
    bitw_byte(bw, 0);                     /* AhAl */
}

/* ======================================================================== */
/*  公共编码入口                                                              */
/* ======================================================================== */

uint32_t mjpeg_encode_frame(const uint16_t *rgb565_data,
                            uint8_t *jpeg_buf,
                            uint32_t jpeg_buf_size,
                            uint8_t quality)
{
    if (!rgb565_data || !jpeg_buf || jpeg_buf_size < 128 * 1024) return 0;
    if (quality != g_quality) mjpeg_set_quality(quality);

    bitw_t bw;
    bitw_init(&bw, jpeg_buf, jpeg_buf_size);

    /* ---- JPEG 头部 ---- */
    write_marker(&bw, 0xFFD8);   /* SOI */
    write_dqt(&bw);
    write_sof0(&bw);
    write_dht(&bw);
    write_sos(&bw);

    /* ---- 熵编码: 逐 MCU (16×16) ---- */
    int prev_dc_y = 0, prev_dc_cb = 0, prev_dc_cr = 0;
    int zz_y[64], zz_cb[64], zz_cr[64];
    float blk[64];

    for (int my = 0; my < MJPEG_HEIGHT; my += 16) {
        for (int mx = 0; mx < MJPEG_WIDTH; mx += 16) {
            /* ---- 收集 16×16 RGB565 → 4:2:0 ---- */
            int8_t y_vals[4][64];    /* 4 个 8×8 Y 块 */
            int8_t cb_vals[64];      /* 8×8 Cb 块 */
            int8_t cr_vals[64];      /* 8×8 Cr 块 */

            for (int by = 0; by < 16; by++) {
                int sy = (my + by) * MJPEG_DS;              /* 源 y (2× 下采样) */
                if (sy >= MJPEG_SRC_HEIGHT) sy = MJPEG_SRC_HEIGHT - MJPEG_DS;
                for (int bx = 0; bx < 16; bx++) {
                    int sx = (mx + bx) * MJPEG_DS;          /* 源 x */
                    if (sx >= MJPEG_SRC_WIDTH) sx = MJPEG_SRC_WIDTH - MJPEG_DS;

                    const uint16_t *sp = &rgb565_data[sy * MJPEG_SRC_WIDTH + sx];
                    int y, cb, cr;
                    rgb565_avg4_to_ycbcr(sp[0], sp[1],
                                         sp[MJPEG_SRC_WIDTH], sp[MJPEG_SRC_WIDTH + 1],
                                         &y, &cb, &cr);

                    int yb = (by >> 3) * 2 + (bx >> 3);   /* 0..3 */
                    int yi = (by & 7) * 8 + (bx & 7);
                    y_vals[yb][yi] = (int8_t)(y - 128);   /* level shift */

                    if ((by & 1) == 0 && (bx & 1) == 0) { /* 4:2:0 采样 */
                        int ci = (by >> 1) * 8 + (bx >> 1);
                        cb_vals[ci] = (int8_t)(cb - 128);
                        cr_vals[ci] = (int8_t)(cr - 128);
                    }
                }
            }

            /* ---- 编码 4 个 Y 块 ---- */
            for (int b = 0; b < 4; b++) {
                for (int i = 0; i < 64; i++) blk[i] = (float)y_vals[b][i];
                fdct_8x8(blk);
                quantize_zigzag(blk, g_quant_luma_inv, zz_y);
                encode_dc(&bw, &s_huff_dc_luma, zz_y[0] - prev_dc_y);
                prev_dc_y = zz_y[0];
                encode_ac(&bw, &s_huff_ac_luma, zz_y);
            }

            /* ---- 编码 Cb 块 ---- */
            for (int i = 0; i < 64; i++) blk[i] = (float)cb_vals[i];
            fdct_8x8(blk);
            quantize_zigzag(blk, g_quant_chroma_inv, zz_cb);
            encode_dc(&bw, &s_huff_dc_chroma, zz_cb[0] - prev_dc_cb);
            prev_dc_cb = zz_cb[0];
            encode_ac(&bw, &s_huff_ac_chroma, zz_cb);

            /* ---- 编码 Cr 块 ---- */
            for (int i = 0; i < 64; i++) blk[i] = (float)cr_vals[i];
            fdct_8x8(blk);
            quantize_zigzag(blk, g_quant_chroma_inv, zz_cr);
            encode_dc(&bw, &s_huff_dc_chroma, zz_cr[0] - prev_dc_cr);
            prev_dc_cr = zz_cr[0];
            encode_ac(&bw, &s_huff_ac_chroma, zz_cr);
        }
        taskYIELD();   /* 每行 MCU (~5-16ms) 让出 CPU: 否则 32ms/帧 的 PDM 音频在编码期间被饿死 */
    }

    /* ---- 结束 ---- */
    bitw_flush(&bw);
    write_marker(&bw, 0xFFD9);   /* EOI */

    /* 诊断: 首帧 JPEG 头 32 字节 (验证 SOI/DQT/SOF0/DHT/SOS 结构) */
    {
        static int dumped = 0;
        if (!dumped) {
            dumped = 1;
            printf("[MJPEG] first JPEG[0..31]:");
            for (uint32_t i = 0; i < 32 && i < bw.pos; i++) {
                printf(" %02X", jpeg_buf[i]);
            }
            printf("\r\n");
        }
    }

    return bw.pos;
}
