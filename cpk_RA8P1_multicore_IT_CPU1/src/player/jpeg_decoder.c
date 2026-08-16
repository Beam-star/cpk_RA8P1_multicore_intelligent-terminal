/**
 ******************************************************************************
 * @file    jpeg_decoder.c
 * @brief   Baseline JPEG 软件解码器 (MJPEG 回放)
 *
 * 解码 mjpeg_encoder.c 生成的 baseline JPEG → RGB565。
 *
 * 约定 (与编码器一致):
 *   - 8-bit, 3 分量, YCbCr 4:2:0 (Y 2×2, Cb/Cr 各 1×1, MCU=16×16)
 *   - 标准 Annex K 量化表 + 标准 Annex K 霍夫曼表 (从码流 DQT/DHT 解析)
 *   - 宽高须为 16 的整数倍 (320×240 满足)
 *   - IDCT 用浮点 (与编码器浮点 DCT 精确匹配, CPU1 有 FPU)
 *
 * 性能: 320×240 = 1800 个 8×8 块, 浮点 IDCT 与编码 DCT 同量级,
 *       ~7fps 回放够用。
 ******************************************************************************
 */

#include "jpeg_decoder.h"
#include <string.h>
#include <stdio.h>

/* ---- Zig-Zag 顺序 (与编码器一致, JPEG 标准) ---- */
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

/* ---- cos((2x+1) * u * PI / 16), 与编码器一致 ---- */
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

/* ======================================================================== */
/*  位读取器 (MSB-first, 0xFF 字节填充)                                        */
/* ======================================================================== */

typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t pos;       /* 当前字节位置 */
    uint32_t acc;       /* 位累加器 */
    int      nbits;     /* 累加器中有效位数 */
    bool     eof;
} bitrd_t;

static void bitrd_init(bitrd_t *br, const uint8_t *data, uint32_t size)
{
    br->data = data; br->size = size; br->pos = 0;
    br->acc = 0; br->nbits = 0; br->eof = false;
}

/* 读 1 位, 返回 0/1; 出错/到 EOI 返回 -1 */
static int bitrd_get(bitrd_t *br)
{
    if (br->nbits == 0) {
        if (br->pos >= br->size) { br->eof = true; return -1; }
        uint8_t b = br->data[br->pos++];
        if (b == 0xFF) {
            if (br->pos < br->size) {
                uint8_t n = br->data[br->pos];
                if (n == 0x00) {
                    br->pos++;               /* 填充字节, 0xFF 是数据 */
                } else {
                    br->eof = true;          /* 遇到 marker (EOI 等), 结束 */
                    return -1;
                }
            } else {
                br->eof = true;
                return -1;
            }
        }
        br->acc = b;
        br->nbits = 8;
    }
    int bit = (int)((br->acc >> 7) & 1u);
    br->acc <<= 1;
    br->nbits--;
    return bit;
}

/* 读 n 位 (n≤16), 返回无符号值; 出错返回 -1 */
static int bitrd_bits(bitrd_t *br, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        int b = bitrd_get(br);
        if (b < 0) return -1;
        v = (v << 1) | b;
    }
    return v;
}

/* ======================================================================== */
/*  霍夫曼解码表                                                              */
/* ======================================================================== */

typedef struct {
    int32_t mincode[17];   /* 码长16的规范码可达 65534, 必须 32 位 (int16 会溢出!) */
    int32_t maxcode[18];
    int32_t valptr[17];
    uint8_t huffval[256];
    int     nval;
} huff_tbl_t;

/* 从 DHT 的 bits[16]/vals[n] 构建规范霍夫曼解码表 */
static void huff_build(huff_tbl_t *t, const uint8_t *bits, const uint8_t *vals, int nval)
{
    int code = 0;
    int vi = 0;   /* 消费 vals */
    int hi = 0;   /* 写入 huffval (按 code 升序) */
    for (int l = 1; l <= 16; l++) {
        t->mincode[l] = (int32_t)code;
        t->valptr[l]  = (int32_t)hi;
        for (int i = 0; i < bits[l - 1]; i++) {
            if (vi < nval && hi < 256) t->huffval[hi++] = vals[vi++];
        }
        t->maxcode[l] = (int32_t)(code + bits[l - 1] - 1);
        code = (code + bits[l - 1]) << 1;
    }
    t->maxcode[17] = (int32_t)0x1FFFF;
    t->nval = hi;
}

/* 解码一个霍夫曼符号; 出错返回 -1 */
static int huff_decode(const huff_tbl_t *t, bitrd_t *br)
{
    int b = bitrd_get(br);
    if (b < 0) return -1;
    int code = b;
    int l = 1;
    while (code > t->maxcode[l]) {
        b = bitrd_get(br);
        if (b < 0) return -1;
        code = (code << 1) | b;
        l++;
        if (l > 16) return -1;
    }
    int idx = t->valptr[l] + (code - t->mincode[l]);
    if (idx < 0 || idx >= t->nval) return -1;
    return (int)t->huffval[idx];
}

/* 解码一个 DC 差分系数 (使用 DC 表); 结果可为负, 通过 out_diff 返回。
 * 注意: 不能以负返回值当错误标志, DC 差分本身合法就可能是负值。 */
static bool decode_dc_diff(const huff_tbl_t *dc, bitrd_t *br, int *out_diff)
{
    int cat = huff_decode(dc, br);
    if (cat < 0) return false;
    if (cat == 0) { *out_diff = 0; return true; }
    int bits = bitrd_bits(br, cat);
    if (bits < 0) return false;
    /* 若最高位为 0 则为负, 减去 (1<<cat)-1 还原 */
    if ((bits & (1 << (cat - 1))) == 0) {
        bits -= (1 << cat) - 1;
    }
    *out_diff = bits;
    return true;
}

/* ======================================================================== */
/*  反 DCT (标准 2D IDCT, 与编码器浮点 DCT 互为逆)                            */
/* ======================================================================== */

static void idct_8x8(const float coeff[64], float block[64])
{
    float tmp[64];

    /* 列 IDCT (v → y), 每列 u */
    for (int u = 0; u < 8; u++) {
        for (int y = 0; y < 8; y++) {
            float sum = 0.0f;
            for (int v = 0; v < 8; v++) {
                float cv = (v == 0) ? 0.70710678f : 1.0f;
                sum += cv * coeff[v * 8 + u] * kCos[v][y];
            }
            tmp[y * 8 + u] = sum * 0.5f;
        }
    }

    /* 行 IDCT (u → x), 每行 y */
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            float sum = 0.0f;
            for (int u = 0; u < 8; u++) {
                float cu = (u == 0) ? 0.70710678f : 1.0f;
                sum += cu * tmp[y * 8 + u] * kCos[u][x];
            }
            block[y * 8 + x] = sum * 0.5f;
        }
    }
}

/* ======================================================================== */
/*  YCbCr → RGB565 (Rec.601 全范围逆变换)                                     */
/* ======================================================================== */

static uint16_t ycbcr_to_rgb565(int y, int cb, int cr)
{
    int r = y + ((359 * (cr - 128)) >> 8);                       /* 1.402 */
    int g = y - ((88 * (cb - 128)) >> 8) - ((183 * (cr - 128)) >> 8); /* 0.344/0.714 */
    int b = y + ((454 * (cb - 128)) >> 8);                       /* 1.772 */

    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;

    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* ======================================================================== */
/*  解码器状态                                                                */
/* ======================================================================== */

typedef struct {
    uint8_t  quant[2][64];      /* 量化表 (zigzag 序): [0]=luma [1]=chroma */
    huff_tbl_t dc[2];           /* DC 表: [0]=luma [1]=chroma */
    huff_tbl_t ac[2];           /* AC 表: [0]=luma [1]=chroma */

    uint32_t width, height;
    int      comp_qt[3];        /* 每分量量化表 id */
    int      comp_dc[3];        /* 每分量 DC 表 id */
    int      comp_ac[3];        /* 每分量 AC 表 id */
    int      ncomp;
} jd_t;

/* 解析一个 DQT 段 (data 指向表信息字节) */
static bool parse_dqt(jd_t *jd, const uint8_t *data, uint32_t len)
{
    uint32_t off = 0;
    while (off < len) {
        uint8_t info = data[off++];
        int id = info & 0x0F;
        if (id > 1) return false;
        if (off + 64 > len) return false;
        /* 8-bit 精度 (高 4 位 == 0) */
        for (int i = 0; i < 64; i++) jd->quant[id][i] = data[off++];
    }
    return true;
}

/* 解析 SOF0 (data 指向精度字节) */
static bool parse_sof0(jd_t *jd, const uint8_t *data, uint32_t len)
{
    if (len < 6) return false;
    uint8_t precision = data[0];
    if (precision != 8) return false;
    jd->height = ((uint32_t)data[1] << 8) | data[2];
    jd->width  = ((uint32_t)data[3] << 8) | data[4];
    int ncomp = data[5];
    if (ncomp != 3 || len < 6u + (uint32_t)ncomp * 3u) return false;
    jd->ncomp = ncomp;
    for (int i = 0; i < ncomp; i++) {
        int cid  = data[6 + i * 3];
        int samp = data[7 + i * 3];
        int qt   = data[8 + i * 3];
        (void)cid; (void)samp;
        jd->comp_qt[i] = qt;
    }
    return true;
}

/* 解析一个 DHT 段 (data 指向表信息字节) */
static bool parse_dht(jd_t *jd, const uint8_t *data, uint32_t len)
{
    uint32_t off = 0;
    while (off < len) {
        uint8_t info = data[off++];
        int cls = (info >> 4) & 0x0F;   /* 0=DC 1=AC */
        int id  = info & 0x0F;
        if (id > 1 || cls > 1) return false;
        if (off + 16 > len) return false;
        uint8_t bits[16];
        int nsym = 0;
        for (int i = 0; i < 16; i++) { bits[i] = data[off++]; nsym += bits[i]; }
        if (off + (uint32_t)nsym > len) return false;
        if (cls == 0) huff_build(&jd->dc[id], bits, &data[off], nsym);
        else          huff_build(&jd->ac[id], bits, &data[off], nsym);
        off += (uint32_t)nsym;
    }
    return true;
}

/* 解析 SOS (data 指向分量数字节), 返回熵数据起始偏移 */
static bool parse_sos(jd_t *jd, const uint8_t *data, uint32_t len)
{
    if (len < 1) return false;
    int ncomp = data[0];
    if (ncomp != jd->ncomp || len < 1u + (uint32_t)ncomp * 2u + 3u) return false;
    for (int i = 0; i < ncomp; i++) {
        int cid = data[1 + i * 2];
        int sel = data[2 + i * 2];
        int idx = cid - 1;                 /* 分量 id 1/2/3 → 0/1/2 */
        if (idx < 0 || idx >= 3) return false;
        jd->comp_dc[idx] = (sel >> 4) & 0x0F;
        jd->comp_ac[idx] = sel & 0x0F;
    }
    return true;
}

/* 解码一个 8×8 块: 熵解码 → 反量化 → IDCT → 输出 sample (0..255, +128 电平) */
static bool decode_block(const jd_t *jd, int qt, int dc_id, int ac_id,
                         bitrd_t *br, int *prev_dc, int8_t *samples)
{
    int diff;
    if (!decode_dc_diff(&jd->dc[dc_id], br, &diff)) return false;
    *prev_dc += diff;

    int zz[64] = {0};
    zz[0] = *prev_dc;
    for (int i = 1; i < 64; i++) {
        /* 解码 (run,size) 符号 */
        int sym = huff_decode(&jd->ac[ac_id], br);
        if (sym < 0) return false;
        if (sym == 0x00) {                       /* EOB */
            while (i < 64) zz[i++] = 0;
            break;
        }
        if (sym == 0xF0) {                       /* ZRL: 16 个零 */
            for (int k = 0; k < 16 && i < 64; k++) zz[i++] = 0;
            i--;                                  /* 抵消外层 i++ */
            continue;
        }
        int run = sym >> 4;
        int size = sym & 0x0F;
        i += run;                                 /* 跳过 run 个零 */
        if (i >= 64) return false;
        if (size == 0) {                          /* 该位置系数为 0 (一般不会出现) */
            zz[i] = 0;
            continue;
        }
        int bits = bitrd_bits(br, size);
        if (bits < 0) return false;
        if ((bits & (1 << (size - 1))) == 0) bits -= (1 << size) - 1;
        zz[i] = bits;
    }

    /* 反量化 + 逆 zigzag + IDCT */
    float coeff[64];
    for (int j = 0; j < 64; j++) {
        coeff[kZigzag[j]] = (float)zz[j] * (float)jd->quant[qt][j];
    }
    float block[64];
    idct_8x8(coeff, block);

    for (int i = 0; i < 64; i++) {
        int v = (int)(block[i] + (block[i] >= 0 ? 0.5f : -0.5f)) + 128;
        if (v < 0) v = 0; else if (v > 255) v = 255;
        samples[i] = (int8_t)(v - 128);
    }
    return true;
}

/* ======================================================================== */
/*  公共入口                                                                  */
/* ======================================================================== */

bool jpeg_decode_rgb565(const uint8_t *jpeg, uint32_t jpeg_size,
                        uint16_t *rgb565_out,
                        uint32_t *out_width, uint32_t *out_height)
{
    if (!jpeg || !rgb565_out || jpeg_size < 4) return false;

    jd_t jd;
    memset(&jd, 0, sizeof(jd));

    /* ---- 逐 marker 解析 ---- */
    uint32_t p = 0;
    const uint8_t *scan_data = NULL;
    uint32_t scan_size = 0;

    if (jpeg[0] != 0xFF || jpeg[1] != 0xD8) return false;   /* 无 SOI */
    p = 2;

    while (p + 1 < jpeg_size) {
        if (jpeg[p] != 0xFF) { p++; continue; }             /* 填充 */
        uint8_t marker = jpeg[p + 1];
        p += 2;

        if (marker == 0xD9) break;                          /* EOI */
        if (marker == 0xD8 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7)) continue;   /* 无载荷 */

        if (marker == 0xDA) {                               /* SOS */
            if (p + 2 > jpeg_size) return false;
            uint32_t seglen = ((uint32_t)jpeg[p] << 8) | jpeg[p + 1];
            const uint8_t *sos = &jpeg[p + 2];
            uint32_t soslen = (seglen >= 2) ? seglen - 2 : 0;
            if (!parse_sos(&jd, sos, soslen)) return false;
            scan_data = &jpeg[p + 2 + soslen];
            scan_size = jpeg_size - (p + 2 + soslen);
            break;                                          /* 熵数据在其后 */
        }

        /* 带长度段的 marker (DQT/SOF0/DHT/其它) */
        if (p + 2 > jpeg_size) return false;
        uint32_t seglen = ((uint32_t)jpeg[p] << 8) | jpeg[p + 1];
        if (seglen < 2) return false;
        const uint8_t *body = &jpeg[p + 2];
        uint32_t bodylen = seglen - 2;
        if (p + 2 + bodylen > jpeg_size) return false;

        switch (marker) {
        case 0xDB: if (!parse_dqt(&jd, body, bodylen)) return false; break;
        case 0xC0: if (!parse_sof0(&jd, body, bodylen)) return false; break;
        case 0xC4: if (!parse_dht(&jd, body, bodylen)) return false; break;
        default: break;   /* 忽略未知段 */
        }
        p += 2 + bodylen;
    }

    if (!scan_data || jd.width == 0 || jd.height == 0 || jd.ncomp != 3) {
        return false;
    }
    if (jd.width > 4096 || jd.height > 4096) return false;

    *out_width  = jd.width;
    *out_height = jd.height;

    /* 假定 4:2:0: Y 2×2, Cb/Cr 1×1, MCU = 16×16 */
    int mcu_w = (int)(jd.width  / 16);
    int mcu_h = (int)(jd.height / 16);
    if (mcu_w <= 0 || mcu_h <= 0) return false;

    bitrd_t br;
    bitrd_init(&br, scan_data, scan_size);

    int prev_dc[3] = {0, 0, 0};
    int8_t y_blk[4][64], cb_blk[64], cr_blk[64];

    for (int my = 0; my < mcu_h; my++) {
        for (int mx = 0; mx < mcu_w; mx++) {
            /* 4 个 Y 块 */
            for (int b = 0; b < 4; b++) {
                if (!decode_block(&jd, jd.comp_qt[0], jd.comp_dc[0], jd.comp_ac[0],
                                  &br, &prev_dc[0], y_blk[b])) {
                    return false;
                }
            }
            /* Cb / Cr 块 */
            if (!decode_block(&jd, jd.comp_qt[1], jd.comp_dc[1], jd.comp_ac[1],
                              &br, &prev_dc[1], cb_blk)) return false;
            if (!decode_block(&jd, jd.comp_qt[2], jd.comp_dc[2], jd.comp_ac[2],
                              &br, &prev_dc[2], cr_blk)) return false;

            /* YCbCr 4:2:0 → RGB565 (16×16 MCU) */
            for (int by = 0; by < 16; by++) {
                int yb = (by >> 3) * 2;                 /* 4 个 Y 块的行组 */
                int yi = (by & 7) * 8;
                int ci = (by >> 1) * 8;                 /* Cb/Cr 行 */
                for (int bx = 0; bx < 16; bx++) {
                    int xb = yb + (bx >> 3);            /* Y 块索引 0..3 */
                    int xi = (bx & 7);
                    int y  = (int)y_blk[xb][yi + xi] + 128;
                    int cb = (int)cb_blk[ci + (bx >> 1)] + 128;
                    int cr = (int)cr_blk[ci + (bx >> 1)] + 128;

                    uint32_t px = (uint32_t)(mx * 16 + bx);
                    uint32_t py = (uint32_t)(my * 16 + by);
                    rgb565_out[py * jd.width + px] = ycbcr_to_rgb565(y, cb, cr);
                }
            }
        }
    }

    return true;
}
