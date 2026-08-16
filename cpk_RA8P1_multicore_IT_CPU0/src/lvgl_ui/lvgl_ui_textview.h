/**
 ******************************************************************************
 * @file    lvgl_ui_textview.h
 * @brief   会议记录文本查看器 (打开 /meeting/notes/record_XX.txt 显示中文内容)
 *
 * 一个独立的 LVGL screen: 标题栏(文件名 + 返回按钮) + 可滚动中文正文。
 * 由 rpmsg_record_cpu0.c 在收到 REC_EVT_NOTES_END 后经 lv_async_call 打开。
 * 打开期间屏蔽左右滑动手势, 返回按钮切回文件浏览页(page2)。
 ******************************************************************************
 */

#ifndef LVGL_UI_TEXTVIEW_H_
#define LVGL_UI_TEXTVIEW_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 打开文本查看器并显示内容 (必须在 LVGL 任务上下文调用)。 */
void lvgl_ui_textview_open(const char *filename, const char *text);

/** 关闭查看器, 切回文件浏览页 (LVGL 任务上下文)。 */
void lvgl_ui_textview_close(void);

/** 查看器是否正在显示 (用于屏蔽翻页手势)。 */
bool lvgl_ui_textview_is_open(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_UI_TEXTVIEW_H_ */
