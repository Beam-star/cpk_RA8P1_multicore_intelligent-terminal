/**
 ******************************************************************************
 * @file    micarray_driver.h
 * @brief   Sipeed MA-USB8 麦克风阵列 UART1 驱动 — 声源定位
 *
 * 模块通过 UART1 (SCI1, 2,000,000 bps 8N1) 持续输出 16x16 声场热力图帧：
 *   16 字节 0xFF 帧头 + 256 字节数据（行优先，每点 0..255）。
 * 无帧序号/时间戳/CRC，接收端靠 16x0xFF 帧头同步，丢帧后自动重新同步。
 *
 * 模式选择（编译期宏）：
 *   MICARRAY_TEST_MODE = 1  测试模式：模拟旋转声源，验证 UI 粒子效果
 *   MICARRAY_TEST_MODE = 0  正常模式：UART1 接收真实热力图，驱动声源定位
 ******************************************************************************
 */

#ifndef MICARRAY_DRIVER_H_
#define MICARRAY_DRIVER_H_

#include <stdint.h>
#include <stdbool.h>

/* 声源定位模式选择：1=测试(模拟声源)  0=正常(真实 UART 热力图) */
#define MICARRAY_TEST_MODE   0

/** 初始化声源定位（根据 MICARRAY_TEST_MODE 选择测试或真实模式）。 */
void micarray_init(void);

/**
 * 发送单字节串口命令（真实模式下有效）。
 * 常用命令：波束档位 '0'..'9','A','B'；阈值 't'/'T'；LED 'e'/'E'；恢复 'R'。
 */
bool micarray_send_cmd(uint8_t cmd);

#endif /* MICARRAY_DRIVER_H_ */
