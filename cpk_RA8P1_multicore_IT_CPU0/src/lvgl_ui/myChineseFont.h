/**
 ******************************************************************************
 * @file    myChineseFont.h
 * @brief   自定义中文字库（数据存于 W25Q256，启动时加载到 SDRAM 供 LVGL 使用）
 *
 * 字库由 Lvgl Font Tool 生成（myChineseFont.c，位于仓库根目录），原始数据过大
 * 无法放进内部 flash，故拆成二进制后烧录到 W25Q256，启动时拷贝到 SDRAM。
 ******************************************************************************
 */

#ifndef MY_CHINESE_FONT_H_
#define MY_CHINESE_FONT_H_

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 中文字库对象（宋体，4bpp，行高 21，约 6859 个常用汉字）。 */
extern const lv_font_t myChineseFont;

/**
 * 从 W25Q256 加载中文字库数据到 SDRAM。
 * 必须在 w25q256_open() 之后、首次使用 myChineseFont 之前调用一次。
 */
void myChineseFont_load(void);

#ifdef __cplusplus
}
#endif

#endif /* MY_CHINESE_FONT_H_ */
