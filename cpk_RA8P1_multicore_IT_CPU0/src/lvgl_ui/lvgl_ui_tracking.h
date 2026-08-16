/**
 ******************************************************************************
 * @file    lvgl_ui_tracking.h
 * @brief   舵机追踪引擎 — 人脸追踪（PID）+ 声源追踪（比例/EMA）
 *
 * 单自由度水平舵机（pan），角度限定左右各 30°（mipi 摄像头 FPC 线短）。
 *
 * 两种追踪模式互斥：
 *   - 人脸追踪：需处于 Face 检测模式；PID 以人脸框中心 X 坐标为反馈。
 *     person1/person2/person3 由语音指令选择，默认 person1。
 *   - 声源追踪：Face/Hand 模式均可用；用声源定位模块的方向角驱动舵机。
 *
 * 引擎运行在低优先级后台任务（prio 1），仅当对应开关 ON 且模式匹配时才
 * 驱动舵机；关闭后舵机缓慢回中位。
 ******************************************************************************
 */
#ifndef LVGL_UI_TRACKING_H_
#define LVGL_UI_TRACKING_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 创建追踪后台任务（在 lvgl_ui_init 内调用一次）。任务内完成舵机初始化。 */
void lvgl_ui_tracking_init(void);

/** 人脸追踪开关（UI 按钮 / 语音命令）。 */
void lvgl_ui_tracking_face_set_on(bool on);
bool lvgl_ui_tracking_face_is_on(void);

/** 声源追踪开关（UI 按钮）。 */
void lvgl_ui_tracking_sound_set_on(bool on);
bool lvgl_ui_tracking_sound_is_on(void);

/** 选择追踪的人脸编号：0=person1, 1=person2, 2=person3。 */
void lvgl_ui_tracking_set_target_person(uint8_t idx);
uint8_t lvgl_ui_tracking_get_target_person(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_UI_TRACKING_H_ */
