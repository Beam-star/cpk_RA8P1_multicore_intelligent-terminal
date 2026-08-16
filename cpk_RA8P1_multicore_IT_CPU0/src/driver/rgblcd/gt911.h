/**
 ******************************************************************************
 * @file    gt911.h
 * @brief   GT911 电容触摸屏驱动头文件 (Goodix GT911)
 *
 * 硬件连接 (Titan-mini RA8P1):
 *   - I2C:   共享 I2C0 (g_i2c_master0)，与 OV5640 摄像头同总线
 *            从机地址: 0x14 (7-bit, INT=low 时选择)
 *   - INT:   P007 (BSP_IO_PORT_00_PIN_07) — 输入, 上拉, IRQ 使能
 *            e2studio FSP 配置: ICU IRQ28 → vector index 20 → r_icu_isr
 *   - RST:   P412 (BSP_IO_PORT_04_PIN_12) — 输出
 *
 * 中断模式:
 *   GT911 检测到触摸时将 INT 拉低 → ICU IRQ7 检测下降沿
 *   → FSP vector[20] 触发 r_icu_isr() → ISR 设置 g_gt911_touch_flag = 1
 *   → gt911_scan() 读取坐标并清除标志
 *
 * 支持的触摸点: 最多 5 点
 *
 * 参考:
 *   - bsp_gt9147.c/h (IMX6ULL 裸机驱动, GT9147)
 *   - gt9xxx.c/h (ESP32-S3 FreeRTOS 驱动, GT911)
 ******************************************************************************
 */

#ifndef GT911_H_
#define GT911_H_

#include <stdint.h>
#include <stdbool.h>
#include "hal_data.h"

/* ======================================================================== */
/*  I2C 地址                                                                  */
/* ======================================================================== */

/** GT911 7-bit I2C 备选地址 (INT 引脚拉低时选择) */
#define GT911_I2C_ADDR_ALT      0x14U
/** GT911 7-bit I2C 默认地址 (INT 引脚浮空/拉高时选择) */
#define GT911_I2C_ADDR_DEFAULT  0x5DU

/* ======================================================================== */
/*  GT911 寄存器地址 (16-bit, 大端)                                            */
/* ======================================================================== */

#define GT911_CTRL_REG          0x8040U     /* 控制寄存器         */
#define GT911_CFGS_REG          0x8047U     /* 配置起始地址寄存器  */
#define GT911_CHECK_REG         0x80FFU     /* 校验和寄存器       */
#define GT911_PID_REG           0x8140U     /* 产品 ID 寄存器     */
#define GT911_GSTID_REG         0x814EU     /* 触摸状态寄存器     */
#define GT911_TP1_REG           0x8150U     /* 第 1 个触摸点数据  */
#define GT911_TP2_REG           0x8158U     /* 第 2 个触摸点数据  */
#define GT911_TP3_REG           0x8160U     /* 第 3 个触摸点数据  */
#define GT911_TP4_REG           0x8168U     /* 第 4 个触摸点数据  */
#define GT911_TP5_REG           0x8170U     /* 第 5 个触摸点数据  */

/* ======================================================================== */
/*  常量                                                                      */
/* ======================================================================== */

#define GT911_MAX_TOUCH_POINTS  5           /* 最多支持的触摸点数 */

/** 触摸坐标寄存器查找表 (在 gt911.c 中定义) */
extern const uint16_t GT911_TPX_TBL[GT911_MAX_TOUCH_POINTS];

/* ======================================================================== */
/*  数据结构                                                                  */
/* ======================================================================== */

/** 单个触摸点信息 */
typedef struct {
    uint16_t x;             /* X 坐标 (LCD 物理像素) */
    uint16_t y;             /* Y 坐标 (LCD 物理像素) */
    uint8_t  id;            /* 触摸点 ID (用于多点追踪) */
    bool     pressed;       /* true=按下, false=释放 */
} gt911_touch_point_t;

/** 触摸数据帧 (一次扫描的所有触摸点) */
typedef struct {
    uint8_t             point_num;                          /* 当前触摸点数量 (0~5) */
    gt911_touch_point_t points[GT911_MAX_TOUCH_POINTS];     /* 触摸点数组 */
    bool                data_ready;                         /* 是否有新数据就绪 */
} gt911_touch_data_t;

/* ======================================================================== */
/*  全局变量 (外部可见)                                                         */
/* ======================================================================== */

/** 最近一次扫描的触摸数据 */
extern gt911_touch_data_t g_gt911_touch;

/** 触摸屏是否初始化成功 */
extern bool g_gt911_init_ok;

/** 触摸中断标志 (ISR 置 1, gt911_scan 清 0) — 用于 FreeRTOS 任务间同步 */
extern volatile uint8_t g_gt911_touch_flag;

/* ======================================================================== */
/*  API 函数                                                                  */
/* ======================================================================== */

/**
 * @brief 初始化 GT911 触摸屏
 *
 * 初始化流程:
 *   1. 打开 I2C0 (如已由摄像头打开则跳过)
 *   2. 硬件复位时序: RST→INT 拉低 → RST 释放 → INT 释放
 *   3. 读取 PID 寄存器验证芯片 (期望 "911")
 *   4. 配置 ICU IRQ7 下降沿中断 → 链接到 NVIC IRQ28 → 安装 ISR → 使能
 *   5. 发送初始化完成命令 (CTRL_REG: 0x02 → 0x00)
 *
 * @return true=初始化成功, false=初始化失败
 */
bool gt911_init(void);

/**
 * @brief 扫描触摸屏 (检查硬件中断标志)
 *
 * 检查 g_gt911_touch_flag:
 *   - 如果为 0 (无中断): 立即返回 false, 触摸数据未更新
 *   - 如果为 1 (ISR 已触发): 通过 I2C 读取触摸坐标, 清除标志, 返回 true
 *
 * @return true=有新触摸数据, false=无新数据
 *
 * @note  本函数在 I2C 传输期间阻塞, 但不会忙等。
 *        建议在 FreeRTOS 任务中以 10-20ms 周期调用。
 *        ISR 极轻量 (仅设置标志), 所有 I2C 操作在任务上下文中完成。
 */
bool gt911_scan(void);

/**
 * @brief 获取触摸屏分辨率
 *
 * @return X 分辨率 (像素)
 */
uint16_t gt911_get_width(void);

/**
 * @brief 获取触摸屏分辨率
 *
 * @return Y 分辨率 (像素)
 */
uint16_t gt911_get_height(void);

#endif /* GT911_H_ */
