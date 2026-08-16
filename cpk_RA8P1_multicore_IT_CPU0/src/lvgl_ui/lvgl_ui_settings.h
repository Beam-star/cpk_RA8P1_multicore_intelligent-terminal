/**
 ******************************************************************************
 * @file    lvgl_ui_settings.h
 * @brief   LVGL 设置页面
 *
 * 设置项:
 *   - 摄像头参数: 亮度/对比度滑块
 *   - 录制质量: MJPEG Q因子 (50~90), 帧率 (5/10fps)
 *   - 网络配置: IP模式 (静态/DHCP), IP地址/网关/DNS
 *   - WiFi 配置: W800 SSID/密码
 *   - 系统信息: 固件版本/SD卡剩余容量
 ******************************************************************************
 */

#ifndef LVGL_UI_SETTINGS_H_
#define LVGL_UI_SETTINGS_H_

#include "lvgl.h"

/**
 * @brief 创建设置页面
 *
 * 页面元素:
 *   - 摄像头亮度滑块 (lv_slider, -3 ~ +3)
 *   - 摄像头对比度滑块 (lv_slider, -3 ~ +3)
 *   - MJPEG 质量滑块 (lv_slider, 50~90)
 *   - 录制帧率下拉 (lv_dropdown, 5fps/10fps)
 *   - 网络模式开关 (lv_switch, 静态IP/DHCP)
 *   - IP/网关/DNS 输入框 (lv_textarea × 4)
 *   - WiFi SSID/密码输入框 (lv_textarea × 2)
 *   - 保存按钮 (lv_btn)
 *   - 返回按钮 (lv_btn)
 */
void lvgl_ui_settings_create(void);

#endif /* LVGL_UI_SETTINGS_H_ */
