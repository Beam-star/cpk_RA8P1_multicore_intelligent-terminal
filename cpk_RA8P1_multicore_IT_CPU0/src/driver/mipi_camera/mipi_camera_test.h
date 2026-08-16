/**
 ******************************************************************************
 * @file    mipi_camera_test.h
 * @brief   CPU0 摄像头采集任务 — 连续采集并通过共享内存同步给 CPU1
 *
 * 功能:
 *   创建 FreeRTOS 任务, 循环执行:
 *     1. 启动 VIN DMA 采集 (三缓冲轮询)
 *     2. 等待帧完成 (信号量同步)
 *     3. 无效化 D-Cache (确保 SDRAM 数据最新)
 *     4. 更新 cam_shmem_t 共享内存 (地址+就绪标志)
 *
 * CPU1 端的 mipi_camera_lcd 任务会轮询共享内存并显示画面。
 ******************************************************************************
 */

#ifndef MIPI_CAMERA_TEST_H_
#define MIPI_CAMERA_TEST_H_

#include <stdbool.h>

/**
 * @brief 启动摄像头采集任务 (默认启用 face detection)
 */
void mipi_camera_test_start(bool use_test_pattern);

/**
 * @brief 启动摄像头采集任务 (可选 face detection)
 *
 * @param use_test_pattern     true = OV5640 测试彩条, false = 正常摄像头
 * @param enable_face_detection true = 启动人脸检测, false = 仅摄像头显示
 */
void mipi_camera_test_start_ex(bool use_test_pattern, bool enable_face_detection);

#endif /* MIPI_CAMERA_TEST_H_ */
