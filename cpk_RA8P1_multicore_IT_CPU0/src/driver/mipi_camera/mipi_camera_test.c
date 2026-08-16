/**
 ******************************************************************************
 * @file    mipi_camera_test.c
 * @brief   MIPI Camera test entry point — delegates to unified cam-lcd task
 *
 * The actual camera capture + LCD display pipeline is now in
 * mipi_camera_lcd.c (single merged task, no inter-task races).
 ******************************************************************************
 */

#include "mipi_camera_test.h"
#include "mipi_camera_lcd.h"

void mipi_camera_test_start(bool use_test_pattern)
{
    mipi_camera_lcd_start(use_test_pattern, true);
}

void mipi_camera_test_start_ex(bool use_test_pattern, bool enable_face_detection)
{
    mipi_camera_lcd_start(use_test_pattern, enable_face_detection);
}
