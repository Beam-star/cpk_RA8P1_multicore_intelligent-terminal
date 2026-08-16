/**
 ******************************************************************************
 * @file    gt911_test.h
 * @brief   GT911 触摸屏测试程序头文件
 ******************************************************************************
 */

#ifndef GT911_TEST_H_
#define GT911_TEST_H_

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief 启动 GT911 触摸测试任务
 *
 * 创建一个 FreeRTOS 任务, 以 10ms 周期轮询 GT911 触摸状态,
 * 检测到触摸时打印坐标到控制台, 并可选地在 LCD 上绘制触摸点。
 *
 * @note  调用前需确保:
 *         - I2C0 已初始化 (通过 mipi_i2c_init 或 gt911_init)
 *         - RGBCLD 已初始化 (rgblcd_init 已调用)
 *         - GT911 已初始化 (gt911_init 成功返回)
 *
 * @param enable_lcd_draw  true=在 LCD 上画触摸轨迹, false=仅控制台输出
 */
void gt911_test_start(bool enable_lcd_draw);

#endif /* GT911_TEST_H_ */
