/**
 ******************************************************************************
 * @file    zw111_fingerprint.h
 * @brief   ZW111 半导体指纹模块驱动（UART6 / SCI6, P909=RX P908=TX, 57600 8N1）
 *
 * 协议（见 指纹模块ZW111/指纹模组产品用户手册_V1.5.1.pdf）：
 *   命令包：EF 01 + 设备地址(4B, 默认 0xFFFFFFFF) + 包标识(01) + 包长度(2B)
 *           + 指令(1B) + 参数 + 校验和(2B)
 *   应答包：EF 01 + 地址(4B) + 包标识(07) + 包长度(2B) + 确认码(1B) + 参数 + 校验和
 *   多字节一律大端（高字节在前）；校验和 = 包标识~校验和之间所有字节之和
 *   （16 位，超出 2 字节的进位忽略）。
 *
 * 模型：异步请求 + worker 任务。录入 / 打卡会阻塞数秒（等待手指按压），不能
 * 放在 LVGL 任务里执行。UI 调用 enroll()/identify() 只投递请求，结果经 done
 * 回调返回（worker 任务上下文），UI 再自行 defer 到 LVGL 任务。
 ******************************************************************************
 */
#ifndef ZW111_FINGERPRINT_H_
#define ZW111_FINGERPRINT_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 传输层错误（区别于模块返回的确认码，后者 > 0） */
#define ZW111_ERR_TRANSPORT   (-1)   /* 串口无应答 / 帧校验错 / 超时 */
#define ZW111_ERR_CANCELLED   (-2)   /* 用户取消（PS_Cancel 终止录入/打卡） */

/* ---- 操作类型 ---- */
typedef enum {
    ZW111_OP_ENROLL      = 0,   /* 录入指纹 */
    ZW111_OP_IDENTIFY    = 1,   /* 指纹打卡（1:N 搜索） */
    ZW111_OP_CLEAR       = 2,   /* 清空指纹库 */
    ZW111_OP_FLUSH_NAMES = 3,   /* 把名字库 RAM 副本写回 W25Q256（内部用） */
} zw111_op_t;

/* ---- 操作结果 ---- */
typedef struct {
    int      status;    /* 0=成功；<0=传输错误；>0=模块确认码(失败) */
    uint16_t page_id;   /* 打卡成功时：匹配到的指纹库 ID */
    uint16_t score;     /* 打卡成功时：匹配得分 */
} zw111_result_t;

/** 完成回调（worker 任务上下文调用；回调内部需自行 defer 到 UI 任务）。 */
typedef void (*zw111_done_cb_t)(zw111_op_t op, const zw111_result_t *result);

/** 初始化 UART6 + 启动 worker 任务；并做一次握手探测 + 恢复录入 ID 计数。 */
void zw111_fingerprint_init(void);

/** 注册「录入/打卡完成」回调。 */
void zw111_fingerprint_set_done_cb(zw111_done_cb_t cb);

/** 请求录入（异步，结果经回调返回）。返回 false = 尚未初始化或忙碌。 */
bool zw111_fingerprint_enroll(void);

/** 请求打卡（异步，1:N 搜索，结果经回调返回）。返回 false = 尚未初始化或忙碌。 */
bool zw111_fingerprint_identify(void);

/** 取消正在进行的录入/打卡（发送 PS_Cancel 0x30 终止模组操作）。
 *  无操作进行时调用无副作用。 */
void zw111_fingerprint_cancel(void);

/** 请求清空指纹库（异步，结果经回调返回）。返回 false = 尚未初始化或忙碌。 */
bool zw111_fingerprint_clear(void);

/** 请求把名字库 RAM 副本写回 W25Q256（异步，无回调）。返回 false = 忙碌。 */
bool zw111_fingerprint_flush_names(void);

/** 是否正在执行录入/打卡（供 UI 互斥判断）。 */
bool zw111_fingerprint_is_busy(void);

/** 模块确认码 → 可读字符串（英文，供日志显示）。 */
const char *zw111_confirm_str(int confirm);

#ifdef __cplusplus
}
#endif

#endif /* ZW111_FINGERPRINT_H_ */
