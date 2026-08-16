#ifndef LVGL_UI_PAGE2_H_
#define LVGL_UI_PAGE2_H_

#include "lvgl.h"
#include <stdint.h>

/** 初始化第二页 (传入第一页对象用于切换) */
void lvgl_ui_page2_init(lv_obj_t *page1);

/** 收到文件列表后刷新 UI */
void lvgl_ui_page2_refresh(void);

/** 手势处理: true=左滑, false=右滑 */
void lvgl_ui_handle_gesture(bool swipe_left);

/** 收到 RPMsg 完整文件列表 (tag: REC_LIST_TAG_AUDIO / REC_LIST_TAG_VIDEO) */
void lvgl_ui_page2_on_list(uint32_t tag, const char names[][32], int count);

/** 播放状态变更 (REC_EVT_PLAY_DONE / REC_EVT_VIDEO_PLAY_DONE 时复位 UI) */
void lvgl_ui_page2_set_playing(bool playing);

/** 是否正在播放 (音频或视频) */
bool lvgl_ui_page2_is_playing(void);

/** 更新播放进度 (elapsed_ms / total_ms 毫秒) */
void lvgl_ui_page2_set_progress(uint32_t elapsed_ms, uint32_t total_ms);

/** 通过路径播放音频（含互斥检查 + 播放状态管理），供按钮与声控共用。
 * @return true 表示已发起播放请求 */
bool lvgl_ui_page2_play_audio_path(const char *path);

/** 返回第二页 screen 对象 (供文本查看器返回时切屏)。 */
lv_obj_t *lvgl_ui_page2_get_screen(void);

#endif
