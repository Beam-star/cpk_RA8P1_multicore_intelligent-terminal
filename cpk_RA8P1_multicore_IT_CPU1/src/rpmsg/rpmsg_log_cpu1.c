/*
 * rpmsg_log_cpu1.c - CPU1 日志发送端（通过 rpmsg_core_get_instance 获取实例）
 */

#include "rpmsg_log.h"
#include "rpmsg_core.h"
#include "rpmsg_lite.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

static struct rpmsg_lite_instance *log_rpmsg = NULL;
static struct rpmsg_lite_endpoint *log_ept = NULL;
static volatile int log_initialized = 0;

int rpmsg_log_cpu1_init(void)
{
	log_rpmsg = (struct rpmsg_lite_instance *)rpmsg_core_get_instance();
    if (log_rpmsg == NULL) return -1;

    log_ept = rpmsg_lite_create_ept(log_rpmsg, RPMSG_LOG_EPT_ADDR_CPU1, NULL, NULL);
    if (log_ept == NULL) return -1;

    log_initialized = 1;
    return 0;
}

void rpmsg_log_cpu1_deinit(void)
{
    log_initialized = 0;
    if (log_ept) { rpmsg_lite_destroy_ept(log_rpmsg, log_ept); log_ept = NULL; }
}

int rpmsg_log_cpu1_printf(const char *fmt, ...)
{
    rpmsg_log_msg_t msg;
    va_list args;
    int ret;

    if (!log_initialized || log_rpmsg == NULL || log_ept == NULL || fmt == NULL)
        return -1;

    va_start(args, fmt);
    ret = vsnprintf(msg.data, RPMSG_LOG_MSG_MAX_SIZE, fmt, args);
    va_end(args);

    if (ret < 0) return -1;

    msg.type = 1;
    msg.length = (uint32_t)ret;
    if (msg.length >= RPMSG_LOG_MSG_MAX_SIZE)
        msg.length = RPMSG_LOG_MSG_MAX_SIZE - 1;

    return (rpmsg_lite_send(log_rpmsg, log_ept, RPMSG_LOG_EPT_ADDR_CPU0,
                            (char *)&msg, sizeof(msg), RL_DONT_BLOCK) == 0) ? 0 : -1;
}

int rpmsg_log_cpu1_puts(const char *str)
{
    rpmsg_log_msg_t msg;
    uint32_t len;

    if (!log_initialized || log_rpmsg == NULL || log_ept == NULL || str == NULL)
        return -1;

    len = (uint32_t)strlen(str);
    if (len >= RPMSG_LOG_MSG_MAX_SIZE) len = RPMSG_LOG_MSG_MAX_SIZE - 1;

    msg.type = 1;
    msg.length = len;
    memcpy(msg.data, str, len);
    msg.data[len] = '\0';

    return (rpmsg_lite_send(log_rpmsg, log_ept, RPMSG_LOG_EPT_ADDR_CPU0,
                            (char *)&msg, sizeof(msg), RL_DONT_BLOCK) == 0) ? 0 : -1;
}
