/**
 ******************************************************************************
 * @file    lvgl_ui_ppt.h
 * @brief   PPT 翻页手势（手掌滑动 → ESP32 翻页命令）
 *
 * 开启 PPT 模式并处于 Hand 检测模式时，检测手掌左右滑动手势：
 *   左 → 右  → 发送 ESP32_CMD_PPT_PREV (0x07, 上一页)
 *   右 → 左  → 发送 ESP32_CMD_PPT_NEXT (0x08, 下一页)
 *
 * 后台手势任务负责：激活态管理 + 滑动状态机 + ESP32 命令发送（0x06~0x09）。
 * UI 只负责切换开关（lvgl_ui_ppt_set_on）。
 ******************************************************************************
 */
#ifndef LVGL_UI_PPT_H_
#define LVGL_UI_PPT_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 创建 PPT 手势后台任务（在 lvgl_ui_init 内调用一次）。 */
void lvgl_ui_ppt_init(void);

/** 切换 PPT 模式开关（UI 按钮回调）。线程安全（单字节 volatile 写）。 */
void lvgl_ui_ppt_set_on(bool on);

/** 查询当前 PPT 模式开关状态。 */
bool lvgl_ui_ppt_is_on(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_UI_PPT_H_ */
