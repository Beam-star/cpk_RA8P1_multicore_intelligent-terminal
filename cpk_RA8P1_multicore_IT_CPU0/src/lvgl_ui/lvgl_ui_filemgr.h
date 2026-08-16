/**
 ******************************************************************************
 * @file    lvgl_ui_filemgr.h
 * @brief   LVGL 文件管理页面 — SD卡录制文件浏览/下载
 ******************************************************************************
 */

#ifndef LVGL_UI_FILEMGR_H_
#define LVGL_UI_FILEMGR_H_

#include "lvgl.h"

/**
 * @brief 创建文件管理页面
 *
 * 功能:
 *   - SD卡文件列表 (lv_list, 按时间倒序排列)
 *   - 文件信息显示 (文件名/大小/时长/录制日期)
 *   - 下载按钮 — 触发 TCP 文件传输至 PC/手机
 *   - 删除按钮 — 删除 SD 卡上指定文件
 *   - 传输进度条 (lv_bar)
 */
void lvgl_ui_filemgr_create(void);

/**
 * @brief 刷新文件列表 (重新扫描 SD 卡)
 */
void lvgl_ui_filemgr_refresh(void);

/**
 * @brief 更新文件传输进度
 * @param filename   文件名
 * @param progress   进度百分比 (0~100)
 */
void lvgl_ui_filemgr_update_progress(const char *filename, uint8_t progress);

#endif /* LVGL_UI_FILEMGR_H_ */
