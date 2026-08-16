/**
 ******************************************************************************
 * @file    mipi_camera_lcd.h
 * @brief   Unified camera capture + LCD display module (CPU0)
 *
 * Merges camera capture (VIN DMA from OV5640) and LCD display
 * (rotation + framebuffer write + GLCDC buffer switch) into a
 * single FreeRTOS task that processes each frame atomically.
 *
 * Data flow:
 *   OV5640 → MIPI CSI → VIN DMA → SDRAM → rotate 90°CW
 *   → fb_background[] → GLCDC (layer 1) → LCD (1024×600)
 *
 * Dependencies:
 *   - The GLCDC display must already be running (either via rgblcd_init()
 *     in legacy single-layer mode, or via RM_LVGL_PORT_Open() in dual-layer
 *     mode).  Backlight should be on.
 *   - In dual-layer mode, LVGL owns GLCDC layer 2; the camera task owns
 *     layer 1.  Both layers are driven by the same line-detect interrupt.
 ******************************************************************************
 */

#ifndef MIPI_CAMERA_LCD_H_
#define MIPI_CAMERA_LCD_H_

#include <stdint.h>
#include <stdbool.h>
#include "rm_lvgl_port.h"

/**
 * @brief Start the unified camera capture + LCD display task.
 *
 * Creates a SINGLE FreeRTOS task that handles the complete pipeline:
 *   camera init → VIN capture wait → D-Cache ops → vsync wait
 *   → rotation → framebuffer write → GLCDC buffer switch
 *
 * This replaces the previous two-task design (separate camera task
 * and LCD task) which had race conditions on shared VIN buffer addresses.
 *
 * @param use_test_pattern      true = OV5640 color bars, false = normal camera
 * @param enable_face_detection true = also run face detection AI pipeline
 *
 * Call after the GLCDC display is running (rgblcd_init or RM_LVGL_PORT_Open).
 */
void mipi_camera_lcd_start(bool use_test_pattern, bool enable_face_detection);

/**
 * @brief LVGL port user VPOS callback — increments g_frame_count so the
 *        camera task's vsync-poll continues to work when RM_LVGL_PORT owns
 *        the GLCDC display controller.
 *
 * Register this as the .p_callback in the rm_lvgl_port_cfg_t passed to
 * RM_LVGL_PORT_Open() (runtime override of the FSP-generated config).
 */
void lvgl_port_vpos_cb(rm_lvgl_port_callback_args_t *p_args);

/**
 * @brief 开/关视频录制帧抓取 (把带人脸框的画面写入共享 SDRAM 供 CPU1 编码)。
 *
 * 由 rpmsg_record_cpu0.c 在收到 REC_EVT_STARTED / STOPPED 时调用。
 * 摄像头任务在录制期间按帧率节流地复制带框画面到 VIDEO_FRAME 双缓冲并递增
 * frame_id, CPU1 轮询后编码写入 AVI。
 *
 * @param enabled  true=开始抓取视频帧, false=停止
 */
void mipi_camera_lcd_set_video_record(bool enabled);

/**
 * @brief 开/关视频回放显示模式。
 *
 * 播放期间 (active=true) 摄像头任务暂停写 GLCDC layer1 帧缓冲, 改由
 * video_play_display 显示任务接管 layer1 显示解码帧; 播放结束恢复摄像头。
 *
 * @param active  true=进入回放显示, false=恢复摄像头显示
 */
void mipi_camera_lcd_set_playback(bool active);

#endif /* MIPI_CAMERA_LCD_H_ */
