/**
 ******************************************************************************
 * @file    mipi_camera.h
 * @brief   MIPI CSI Camera driver (OV5645) for RA8P1 CPK board
 *
 * Hardware:
 *   - OV5645 sensor, 2-lane MIPI CSI-2, built-in 24MHz oscillator
 *   - VIN (Video Input) module: YUV422/RGB565 input → RGB565 output
 *   - I2C (IIC0, 400kHz): OV5645 register access (addr 0x3C)
 *   - GPIO: RST/STBY power control
 ******************************************************************************
 */

#ifndef MIPI_CAMERA_H_
#define MIPI_CAMERA_H_

#include "bsp_api.h"
#include <stdint.h>
#include <stdbool.h>

FSP_HEADER

/* ---- 图像参数 (与 VIN 模块配置和 OV5645 输出一致) ---- */
#define CAM_IMAGE_WIDTH         640
#define CAM_IMAGE_HEIGHT        480
#define CAM_IMAGE_BPP           2       /* RGB565 = 2 bytes/pixel */
#define CAM_FRAME_SIZE          (CAM_IMAGE_WIDTH * CAM_IMAGE_HEIGHT * CAM_IMAGE_BPP)  /* 614400 字节 */

/* ---- GPIO 引脚定义 (CPK 板硬件连接) ---- */
#define CAM_RST_PIN             BSP_IO_PORT_11_PIN_00   /* OV5645 复位 PB00 (Active-Low) */
#define CAM_PWDN_PIN            BSP_IO_PORT_07_PIN_10   /* ATK-MCOV5645: P710=H 正常工作, P710=L 掉电
                                                           (注意: 此模组极性与其他 OV5645 相反!) */

/**
 * @brief 初始化 OV5645 摄像头和 VIN 外设
 * @param use_test_mode  true = 输出八色彩条测试图案
 * @return FSP_SUCCESS 成功, 其他值表示失败阶段
 */
fsp_err_t mipi_camera_init(bool use_test_mode);
void mipi_camera_capture_start(void);
bool mipi_camera_wait_frame(uint32_t timeout_ms);
uint8_t *mipi_camera_get_frame(void);

FSP_FOOTER
#endif /* MIPI_CAMERA_H_ */
