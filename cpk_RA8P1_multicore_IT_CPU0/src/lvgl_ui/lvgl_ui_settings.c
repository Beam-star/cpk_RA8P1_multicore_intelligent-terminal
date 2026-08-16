/**
 ******************************************************************************
 * @file    lvgl_ui_settings.c
 * @brief   LVGL 设置页面实现
 ******************************************************************************
 */

#include "lvgl_ui_settings.h"
#include "lvgl.h"
#include <stdio.h>

void lvgl_ui_settings_create(void)
{
    printf("[LVGL UI] Creating settings page...\r\n");

    /* 创建设置页面 (新屏幕) */
    lv_obj_t *settings_scr = lv_obj_create(NULL);

    /* 标题 */
    lv_obj_t *title = lv_label_create(settings_scr);
    lv_label_set_text(title, "Settings");
    lv_obj_set_pos(title, 20, 10);

    /* TODO: 添加各设置控件
     * - 亮度/对比度滑块 (lv_slider)
     * - MJPEG 质量滑块 (50-90)
     * - 录制帧率下拉 (5/10fps)
     * - 网络模式开关 (静态IP/DHCP)
     * - IP/网关文本输入框
     * - WiFi SSID/密码输入框
     * - 保存&返回按钮 */

    (void)settings_scr;
    printf("[LVGL UI] Settings page created\r\n");
}
