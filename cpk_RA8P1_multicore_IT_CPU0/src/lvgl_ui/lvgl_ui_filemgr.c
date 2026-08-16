/**
 ******************************************************************************
 * @file    lvgl_ui_filemgr.c
 * @brief   LVGL 文件管理页面实现
 ******************************************************************************
 */

#include "lvgl_ui_filemgr.h"
#include "lvgl.h"
#include <stdio.h>

static lv_obj_t *g_file_list = NULL;
static lv_obj_t *g_progress_bar = NULL;

void lvgl_ui_filemgr_create(void)
{
    printf("[LVGL UI] Creating file manager page...\r\n");

    lv_obj_t *scr = lv_obj_create(NULL);

    /* 标题 */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Meeting Records (SD Card)");
    lv_obj_set_pos(title, 20, 10);

    /* 文件列表 */
    g_file_list = lv_list_create(scr);
    lv_obj_set_pos(g_file_list, 10, 50);
    lv_obj_set_size(g_file_list, 800, 400);

    /* 传输进度条 */
    g_progress_bar = lv_bar_create(scr);
    lv_obj_set_pos(g_progress_bar, 10, 470);
    lv_obj_set_size(g_progress_bar, 800, 20);
    lv_bar_set_value(g_progress_bar, 0, LV_ANIM_OFF);

    /* TODO: 底部按钮 — 刷新列表/返回主界面 */

    printf("[LVGL UI] File manager page created\r\n");
}

void lvgl_ui_filemgr_refresh(void)
{
    /* TODO: 通过 RPMsg 请求 CPU1 扫描 SD 卡 /MEETING/ 目录,
     * 获取 JSON 文件列表, 更新 lv_list */
}

void lvgl_ui_filemgr_update_progress(const char *filename, uint8_t progress)
{
    /* TODO: 更新进度条和文件名标签 */
    (void)filename;
    lv_bar_set_value(g_progress_bar, progress, LV_ANIM_ON);
}
