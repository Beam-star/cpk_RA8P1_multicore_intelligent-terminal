/*
 * rpmsg_log_cpu0.c - CPU0 日志接收端（通过 rpmsg_core_get_instance 获取实例）
 */

#include "rpmsg_log.h"
#include "rpmsg_core.h"
#include "rpmsg_lite.h"
#include "rpmsg_queue.h"
#include <stdio.h>
#include <string.h>

static struct rpmsg_lite_instance *log_rpmsg = NULL;
static rpmsg_queue_handle log_queue = NULL;
static struct rpmsg_lite_endpoint *log_ept = NULL;
static volatile int log_initialized = 0;

int rpmsg_log_cpu0_init(void)
{
	log_rpmsg = (struct rpmsg_lite_instance *)rpmsg_core_get_instance();
    if (log_rpmsg == NULL) return -1;

    log_queue = rpmsg_queue_create(log_rpmsg);
    if (log_queue == NULL) return -1;

    log_ept = rpmsg_lite_create_ept(log_rpmsg, RPMSG_LOG_EPT_ADDR_CPU0,
                                     rpmsg_queue_rx_cb, log_queue);
    if (log_ept == NULL) return -1;

    log_initialized = 1;
    printf("[LOG] CPU0 log initialized (EP %d)\r\n", RPMSG_LOG_EPT_ADDR_CPU0);
    return 0;
}

void rpmsg_log_cpu0_deinit(void)
{
    log_initialized = 0;
    if (log_ept) { rpmsg_lite_destroy_ept(log_rpmsg, log_ept); log_ept = NULL; }
    if (log_queue) { rpmsg_queue_destroy(log_rpmsg, log_queue); log_queue = NULL; }
}

int rpmsg_log_cpu0_process(void)
{
    rpmsg_log_msg_t msg;
    uint32_t len, src;
    int count = 0;

    if (!log_initialized || log_rpmsg == NULL || log_queue == NULL) return 0;

    while (rpmsg_queue_recv(log_rpmsg, log_queue, &src,
                            (char *)&msg, sizeof(msg), &len,
                            RL_DONT_BLOCK) == RL_SUCCESS)
    {
        if (msg.length < RPMSG_LOG_MSG_MAX_SIZE)
            msg.data[msg.length] = '\0';
        else
            msg.data[RPMSG_LOG_MSG_MAX_SIZE - 1] = '\0';

        printf("[CPU1] %s", msg.data);
        count++;
    }
    return count;
}
