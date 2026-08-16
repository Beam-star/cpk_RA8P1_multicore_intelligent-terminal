/**
 ******************************************************************************
 * @file    scs_servo.h
 * @brief   FEETECH STS 磁编码串行舵机驱动（FT-SCS 协议）— Renesas FSP 移植
 *
 * 从 FTServo_stm32HAL/SCSLib 移植，去掉 STM32 HAL 依赖，改用 FSP UART。
 * 目标芯片：RA8P1 (CPU0)，使用 UART4 / SCI4（g_uart4，引脚 P414=RX P415=TX，
 *   1Mbps 8N1，已在 e2studio FSP 中配置）。
 *
 * 协议（见 FTServo_stm32HAL/舵机SCS通信协议.md）：
 *   帧格式：FF FF <ID> <LEN> <CMD> [<参数…>] <CHECKSUM>
 *   CHECKSUM = ~(ID + LEN + CMD + 参数…)，STS 为小端（低字节在前）。
 ******************************************************************************
 */
#ifndef SCS_SERVO_H_
#define SCS_SERVO_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 舵机总线参数（按需修改） ---- */
#define SCS_SERVO_ID            1           /* 舵机站号，出厂默认 1 */

/* ---- STS 内存表地址（见 磁编码STS舵机-内存表解析.md） ---- */
#define SCS_STS_ADDR_TORQUE_ENABLE  40     /* 扭矩开关 (0关 1开 2阻尼 128中位校准) */
#define SCS_STS_ADDR_ACC            41     /* 加速度 (8.7 度/s^2) */
#define SCS_STS_ADDR_GOAL_POS_L     42     /* 目标位置 低字节 */
#define SCS_STS_ADDR_GOAL_POS_H     43     /* 目标位置 高字节 */
#define SCS_STS_ADDR_GOAL_TIME_L    44     /* 运行时间 低字节（预留） */
#define SCS_STS_ADDR_GOAL_TIME_H    45     /* 运行时间 高字节（预留） */
#define SCS_STS_ADDR_GOAL_SPEED_L   46     /* 运行速度 低字节 */
#define SCS_STS_ADDR_GOAL_SPEED_H   47     /* 运行速度 高字节 */
#define SCS_STS_ADDR_PRESENT_POS_L  56     /* 当前位置 低字节（只读） */
#define SCS_STS_ADDR_PRESENT_POS_H  57     /* 当前位置 高字节（只读） */
#define SCS_STS_ADDR_MOVING         66     /* 移动标志（只读，1=运动中） */

/* ---- 位置换算 ---- */
#define SCS_STS_POS_CENTER          2048    /* 中位 0°（0..4095 对应 0..360°） */
#define SCS_STS_POS_PER_DEG         (1000.0f / 87.0f)  /* ≈11.494 单位/度 */

/**
 * 初始化舵机：打开 UART4 + 使能扭矩 + 回中位。
 * 只有 UART4 打开失败才返回 false（此时完全无法通信）。
 * 扭矩使能/回中位的写命令是 fire-and-forget，ACK 超时仅打印日志、不影响返回，
 * 保证追踪回路（纯写命令）在舵机偶发无应答时依然能正常驱动。
 */
bool scs_servo_init(void);

/**
 * 写目标位置（绝对位置控制，一次性写入 加速度+位置+预留+速度）。
 * @param pos   目标位置 0..4095（BIT15 方向位由内部处理）
 * @param speed 运行速度（0=最快；单位 0.732RPM，越小越慢）
 * @param acc   加速度（单位 8.7 度/s^2）
 */
bool scs_servo_write_pos(uint16_t pos, uint16_t speed, uint8_t acc);

/**
 * 读当前位置。失败返回 -1。
 */
int scs_servo_read_pos(void);

/**
 * 扭矩开关。@param on true=打开扭矩, false=关闭扭矩。
 */
bool scs_servo_torque(bool on);

/**
 * 查询舵机是否在运动中。失败返回 -1。
 */
int scs_servo_is_moving(void);

/**
 * 测试：打开 UART4 → 读位置 → 使能扭矩 → 左右 ±30° 摆动 3 个来回 → 回中。
 * 分步打印，用于排查舵机不转动的通信/供电问题。
 */
void scs_servo_test_run(void);

#ifdef __cplusplus
}
#endif

#endif /* SCS_SERVO_H_ */
