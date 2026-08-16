/*
 * rpmsg_log.h - 日志系统头文件（CPU0/CPU1 共用）
 * 日志代码内部通过 rpmsg_core_get_instance() 获取实例
 */

#ifndef RPMSG_LOG_H_
#define RPMSG_LOG_H_

#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RPMSG_LOG_EPT_ADDR_CPU0    (50U)
#define RPMSG_LOG_EPT_ADDR_CPU1    (51U)
#define RPMSG_LOG_MSG_MAX_SIZE     (240U)

typedef struct {
    uint32_t type;
    uint32_t length;
    char     data[RPMSG_LOG_MSG_MAX_SIZE];
} rpmsg_log_msg_t;

/* CPU0 */
int   rpmsg_log_cpu0_init(void);
void  rpmsg_log_cpu0_deinit(void);
int   rpmsg_log_cpu0_process(void);

/* CPU1 */
int   rpmsg_log_cpu1_init(void);
void  rpmsg_log_cpu1_deinit(void);
int   rpmsg_log_cpu1_printf(const char *fmt, ...);
int   rpmsg_log_cpu1_puts(const char *str);

#ifdef __cplusplus
}
#endif

#endif /* RPMSG_LOG_H_ */
