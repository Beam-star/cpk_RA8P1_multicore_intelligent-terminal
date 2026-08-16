/**
 ******************************************************************************
 * @file    lvgl_ui_page3.h
 * @brief   UI 第三页 — 声源定位 (sound source localization) 粒子波动雷达
 *
 * 页面内容：一个固定大小的圆圈，根据声音的方向 + 强度 + 消散，
 * 用渐变色粒子（由亮到暗）表现声源方位。
 *
 * 数据来源（二选一）：
 *   - lvgl_ui_page3_set_direction(angle, intensity)  直接给方向+强度
 *   - lvgl_ui_page3_set_heatmap(heatmap[16][16])     给 16x16 热力图，内部求峰值
 *
 * 测试模式：lvgl_ui_page3_test_start() 会模拟一个旋转声源，用于在没有
 * 真实声源模块时验证粒子波动效果。
 ******************************************************************************
 */

#ifndef LVGL_UI_PAGE3_H_
#define LVGL_UI_PAGE3_H_

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 创建第三页（声源定位雷达），新建一个独立 screen。 */
void lvgl_ui_page3_init(void);

/** 返回第三页 screen 对象（供翻页导航使用）。 */
lv_obj_t *lvgl_ui_page3_get_screen(void);

/**
 * 直接设置声源方向 + 强度。
 * @param angle_deg  0 = 正前方(屏幕上方)，顺时针 0..360
 * @param intensity  0..255 强度（亮度）
 */
void lvgl_ui_page3_set_direction(int16_t angle_deg, uint8_t intensity);

/**
 * 喂入一帧 16x16 声场热力图（行优先，每点 0..255）。
 * 内部求峰值并映射为方向 + 强度后调用 set_direction。
 */
void lvgl_ui_page3_set_heatmap(const uint8_t heatmap[16 * 16]);

/**
 * 读取当前声源方向 + 强度（供声源追踪引擎用）。
 * @param angle_deg  输出：0..359（0=正前方/屏幕上方，顺时针）
 * @param intensity  输出：0..255
 * @return false 表示当前无声源（方向未定）
 */
bool lvgl_ui_page3_get_direction(int16_t *angle_deg, uint8_t *intensity);

/** 测试模式：模拟一个旋转 + 脉动的声源。 */
void lvgl_ui_page3_test_start(void);
void lvgl_ui_page3_test_stop(void);
bool lvgl_ui_page3_test_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_UI_PAGE3_H_ */
