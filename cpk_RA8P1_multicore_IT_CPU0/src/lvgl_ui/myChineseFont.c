/**
 ******************************************************************************
 * @file    myChineseFont.c
 * @brief   自定义中文字库 —— 数据存 W25Q256，启动加载到 SDRAM 后供 LVGL 使用
 *
 * 原始 myChineseFont.c（Lvgl Font Tool 生成）在仓库根目录，约 10MB 源文件，
 * 编译后数据约 1.3MB，内部 flash 放不下。用 scripts/font_to_bin.py 拆成三个
 * 二进制（位图/unicode 表/字形描述），烧录到 W25Q256，此处从 flash 拷贝到 SDRAM。
 *
 * 注意：Lvgl Font Tool 生成的查询函数是旧版 LVGL API，这里已按 LVGL 9.3 改写。
 ******************************************************************************
 */

#include "myChineseFont.h"
#include "lvgl.h"
#include "driver/w25q256/w25q256.h"
#include <stdio.h>

/* ---- 字体数据在 W25Q256 中的地址与大小（由 scripts/filter_font.py 生成，
 *      只保留 GB2312 一级字库 + ASCII，位图 < 1MB 以避开 20-bit 索引截断） ---- */
#define CN_FONT_FLASH_BITMAP   0x0BE0000UL
#define CN_FONT_FLASH_UNICODE  0x0D11000UL
#define CN_FONT_FLASH_DSC      0x0D15000UL

#define CN_FONT_BITMAP_SIZE    0xA99C5UL
#define CN_FONT_UNICODE_SIZE   0x1E16UL
#define CN_FONT_DSC_SIZE       0x7858UL
#define CN_FONT_GLYPH_COUNT    3851

extern void SCB_CleanDCache_by_Addr(volatile void *addr, int32_t dsize);

/* ---- SDRAM 固定地址（避开 NPU 竞技场/模型/视频帧缓冲等固定区域）。
 *     视频帧缓冲 1 结束于 0x68776000，取 0x68800000 起，安全落在 CPU0 区。 ---- */
#define CN_FONT_SDRAM_BASE     0x68800000UL
#define CN_FONT_SDRAM_BITMAP   (CN_FONT_SDRAM_BASE + 0x000000UL)
#define CN_FONT_SDRAM_UNICODE  (CN_FONT_SDRAM_BASE + 0x131000UL)
#define CN_FONT_SDRAM_DSC      (CN_FONT_SDRAM_BASE + 0x135000UL)

/* ---- 字符映射表（cmap 本身很小，放内部 flash，unicode_list 指向 SDRAM） ---- */
static lv_font_fmt_txt_cmap_t cmaps[] = {{
    .range_start       = 0x0020,
    .range_length      = 0x9fa0,      /* 工具实际存的是最大码点（非长度） */
    .type              = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY,
    .glyph_id_start    = 0,
    .unicode_list      = (const uint16_t *)CN_FONT_SDRAM_UNICODE,
    .glyph_id_ofs_list = NULL,
    .list_length       = CN_FONT_GLYPH_COUNT,
}};

static lv_font_fmt_txt_dsc_t font_dsc = {
    .glyph_bitmap  = (const uint8_t *)CN_FONT_SDRAM_BITMAP,
    .glyph_dsc     = (const lv_font_fmt_txt_glyph_dsc_t *)CN_FONT_SDRAM_DSC,
    .cmaps         = cmaps,
    .kern_dsc      = NULL,
    .kern_scale    = 0,
    .cmap_num      = 1,
    .bpp           = 4,
    .kern_classes  = 0,
    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,
    .stride        = 0,
};

/* ---- 二分查表 ---- */
static int binsearch(const uint16_t *sortedSeq, int seqLength, uint16_t keyData)
{
    int low = 0, mid, high = seqLength - 1;
    while (low <= high) {
        mid = (low + high) >> 1;
        if (keyData < sortedSeq[mid]) {
            high = mid - 1;
        }
        else if (keyData > sortedSeq[mid]) {
            low = mid + 1;
        }
        else {
            return mid;
        }
    }
    return -1;
}

/* ---- LVGL 9.3 get_glyph_dsc 回调 ---- */
static bool __user_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc_out,
                                      uint32_t letter, uint32_t letter_next)
{
    (void)letter_next;
    lv_font_fmt_txt_dsc_t *fdsc = (lv_font_fmt_txt_dsc_t *)font->dsc;

    if (letter < fdsc->cmaps[0].range_start || letter > fdsc->cmaps[0].range_length) {
        return false;
    }

    int i = binsearch(fdsc->cmaps[0].unicode_list, fdsc->cmaps[0].list_length, (uint16_t)letter);
    if (i == -1) {
        return false;
    }

    const lv_font_fmt_txt_glyph_dsc_t *gdsc = &fdsc->glyph_dsc[i];

    dsc_out->resolved_font = font;
    dsc_out->adv_w  = gdsc->adv_w;      /* 工具存纯像素(21/11)，非 8.4 格式 */
    dsc_out->box_w  = gdsc->box_w;
    dsc_out->box_h  = gdsc->box_h;
    dsc_out->ofs_x  = gdsc->ofs_x;
    dsc_out->ofs_y  = gdsc->ofs_y;
    dsc_out->stride = 0;                /* 4bpp 打包，无 padding */
    dsc_out->format = LV_FONT_GLYPH_FORMAT_A4;
    dsc_out->is_placeholder = 0;
    dsc_out->gid.index = (uint32_t)i;
    return true;
}

/* ---- LVGL 9.3 get_glyph_bitmap 回调 ---- */
static const void *__user_font_get_bitmap(lv_font_glyph_dsc_t *g_dsc, lv_draw_buf_t *draw_buf)
{
    const lv_font_t *font = g_dsc->resolved_font;
    lv_font_fmt_txt_dsc_t *fdsc = (lv_font_fmt_txt_dsc_t *)font->dsc;
    uint32_t gid = g_dsc->gid.index;
    const lv_font_fmt_txt_glyph_dsc_t *gdsc = &fdsc->glyph_dsc[gid];

    /* 渲染器请求原始位图：直接返回 4bpp 数据 */
    if (g_dsc->req_raw_bitmap) {
        return &fdsc->glyph_bitmap[gdsc->bitmap_index];
    }

    /* 否则把 4bpp 转成 A8 到 draw_buf */
    uint8_t *out = draw_buf->data;
    const uint8_t *in = &fdsc->glyph_bitmap[gdsc->bitmap_index];
    uint16_t w = gdsc->box_w, h = gdsc->box_h;
    uint16_t in_stride = (w + 1) / 2;   /* 4bpp 每行字节数 */
    uint32_t out_stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_A8);

    for (uint16_t y = 0; y < h; y++) {
        for (uint16_t x = 0; x < w; x++) {
            uint8_t b = in[y * in_stride + (x >> 1)];
            uint8_t nib = (x & 1) ? (b & 0x0F) : (b >> 4);
            out[y * out_stride + x] = (uint8_t)((nib << 4) | nib);
        }
    }

    /* 关键：和标准字库一样，返回整个 draw_buf 结构（而非 draw_buf->data），
     * 并 flush 缓存，否则 D/AVE 2D 渲染器会把 A8 位图指针当成 lv_draw_buf_t
     * 去读 data_size，读到垃圾值后卡死在 d1_cacheblockflush。 */
    lv_draw_buf_flush_cache(draw_buf, NULL);
    return draw_buf;
}

/* ---- 字体对象 ---- */
const lv_font_t myChineseFont = {
    .get_glyph_dsc    = __user_font_get_glyph_dsc,
    .get_glyph_bitmap = __user_font_get_bitmap,
    .release_glyph    = NULL,
    .line_height      = 21,
    .base_line        = 0,
    .static_bitmap    = 1,      /* 位图是静态的，渲染器直接用原始 4bpp */
    .dsc              = &font_dsc,
    .fallback         = NULL,
};

/* ======================================================================== */
/*  加载                                                                     */
/* ======================================================================== */

void myChineseFont_load(void)
{
    w25q256_read(CN_FONT_FLASH_BITMAP,  (uint8_t *)CN_FONT_SDRAM_BITMAP,  CN_FONT_BITMAP_SIZE);
    w25q256_read(CN_FONT_FLASH_UNICODE, (uint8_t *)CN_FONT_SDRAM_UNICODE, CN_FONT_UNICODE_SIZE);
    w25q256_read(CN_FONT_FLASH_DSC,     (uint8_t *)CN_FONT_SDRAM_DSC,     CN_FONT_DSC_SIZE);

    /* D-Cache clean：确保其它总线主设备能看到已加载的字体数据 */
    SCB_CleanDCache_by_Addr((void *)CN_FONT_SDRAM_BITMAP,  (int32_t)CN_FONT_BITMAP_SIZE);
    SCB_CleanDCache_by_Addr((void *)CN_FONT_SDRAM_UNICODE, (int32_t)CN_FONT_UNICODE_SIZE);
    SCB_CleanDCache_by_Addr((void *)CN_FONT_SDRAM_DSC,     (int32_t)CN_FONT_DSC_SIZE);
}
