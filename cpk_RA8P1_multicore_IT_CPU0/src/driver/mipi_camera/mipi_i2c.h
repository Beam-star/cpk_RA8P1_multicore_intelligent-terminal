/**
 ******************************************************************************
 * @file    mipi_i2c.h
 * @brief   MIPI Camera I2C 总线封装层 (FSP IIC Master API)
 *
 * 本模块将 FSP 异步 IIC Master API 封装为阻塞式寄存器读写函数,
 * 供 IMX415 / GT911 等设备调用。
 *
 * 硬件依赖:
 *   - FSP IIC Master 实例: g_i2c_master0 (IIC0 通道)
 *   - 时钟模式: Fast Mode (400kHz)
 *   - IMX415 从机地址: 0x1A (7-bit)
 *   - GT911 从机地址: 0x5D / 0x14 (7-bit)
 *   - 寄存器地址: 16-bit (高字节在前)
 *   - 寄存器数据: 8-bit
 *
 * 异步→阻塞转换:
 *   FSP IIC Master 的 write/read 是异步非阻塞的,
 *   本模块使用 FreeRTOS 二进制信号量 + ISR 回调实现阻塞等待。
 ******************************************************************************
 */

#ifndef MIPI_I2C_H_
#define MIPI_I2C_H_

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "semphr.h"

/**
 * @brief 初始化 I2C 总线 (打开 IIC0, 设置从机地址和回调)
 * @return true=成功, false=失败
 * @note  需在 mipi_camera_init() 之前或内部调用
 */
bool mipi_i2c_init(void);

/**
 * @brief 获取 I2C 总线是否已初始化
 * @return true=已初始化, false=未初始化
 */
bool mipi_i2c_is_ready(void);

/* ---- 通用 I2C 总线访问 (供触摸屏等共享 I2C 总线的设备使用) ---- */

/**
 * @brief 通用 I2C 写操作 (阻塞, 自动设置从机地址)
 *
 * 使用共享的 g_i2c_master0 总线, 先调用 slaveAddressSet 切换到目标设备,
 * 再执行写操作。与 OV5640 摄像头共享总线, 但通过从机地址区分设备。
 *
 * @param slave_addr  7-bit I2C 从机地址
 * @param data        待发送数据
 * @param len         数据长度 (字节)
 * @return true=成功, false=超时或失败
 */
bool i2c_bus_write(uint8_t slave_addr, const uint8_t *data, uint32_t len);

/**
 * @brief 通用 I2C 写后读操作 (阻塞, 自动设置从机地址)
 *
 * 先写寄存器地址, 不发送 STOP, 再 RESTART 读数据。
 * 用于 GT911 等 16-bit 寄存器地址的设备。
 *
 * @param slave_addr  7-bit I2C 从机地址
 * @param reg_addr    寄存器地址字节数组
 * @param addr_len    寄存器地址长度 (字节)
 * @param rx_data     读取数据缓冲区
 * @param rx_len      读取数据长度 (字节)
 * @return true=成功, false=超时或失败
 */
bool i2c_bus_write_then_read(uint8_t slave_addr,
                              const uint8_t *reg_addr, uint32_t addr_len,
                              uint8_t *rx_data, uint32_t rx_len);

/**
 * @brief 切换 I2C 从机地址 (用于总线地址扫描和多设备切换)
 * @param slave_addr  7-bit I2C 地址
 * @return true=成功
 */
bool i2c_set_slave(uint8_t slave_addr);

/**
 * @brief 写 8-bit 寄存器 (8-bit 地址) — 用于 SCCB 扩展寄存器
 * @param regID   8-bit 寄存器地址
 * @param regDat  8-bit 写入值
 * @return true=成功
 */
bool wrSensorReg8_8(int regID, int regDat);

/**
 * @brief 写 8-bit 寄存器 (16-bit 地址) — OV5640 标准寄存器访问
 * @param regID   16-bit 寄存器地址 (如 0x3008)
 * @param regDat  8-bit 写入值
 * @return true=成功
 */
bool wrSensorReg16_8(int regID, int regDat);

/**
 * @brief 读 8-bit 寄存器 (16-bit 地址)
 * @param regID   16-bit 寄存器地址
 * @param regDat  [out] 读取到的值
 * @return true=成功
 */
bool rdSensorReg16_8(uint16_t regID, uint8_t *regDat);

/**
 * @brief 连续读取多个字节 (16-bit 起始地址, 地址自动递增)
 * @param regID   起始寄存器地址
 * @param regDat  [out] 读取数据的缓冲区
 * @param len     要读取的字节数
 * @return true=成功
 */
bool rdSensorReg16_Multi(uint16_t regID, uint8_t *regDat, uint32_t len);

#endif /* MIPI_I2C_H_ */
