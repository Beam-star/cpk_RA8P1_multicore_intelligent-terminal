/*
 * rpmsg_test.c - CPU1 基础通信测试（使用 rpmsg_core 共享实例）
 */

#include "rpmsg_core.h"
#include "rpmsg_lite.h"
#include "rpmsg_queue.h"
#include "rpmsg_log.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

#define LOCAL_EPT_ADDR        (30U)
#define REMOTE_EPT_ADDR       (40U)

typedef struct { uint32_t data; uint32_t command; } test_message_t;
#define TEST_CMD_PING  (0x01)
#define TEST_CMD_PONG  (0x02)
#define TEST_CMD_DATA  (0x03)
#define TEST_CMD_ACK   (0x04)

static struct rpmsg_lite_instance *rpmsg = NULL;
static rpmsg_queue_handle queue = NULL;
static struct rpmsg_lite_endpoint *ept = NULL;
static volatile uint32_t remote_addr = REMOTE_EPT_ADDR;

static int rpmsg_test_init(void)
{
    rpmsg = (struct rpmsg_lite_instance *)rpmsg_core_get_instance();
    if (rpmsg == NULL) return -1;

    queue = rpmsg_queue_create(rpmsg);
    if (queue == NULL) return -1;

    ept = rpmsg_lite_create_ept(rpmsg, LOCAL_EPT_ADDR, rpmsg_queue_rx_cb, queue);
    if (ept == NULL) return -1;

    rpmsg_log_cpu1_printf("[CPU1] Test initialized (ept=%d)\r\n", LOCAL_EPT_ADDR);
    return 0;
}

static void rpmsg_test_deinit(void)
{
    rpmsg_log_cpu1_deinit();
    if (ept) { rpmsg_lite_destroy_ept(rpmsg, ept); ept = NULL; }
    if (queue) { rpmsg_queue_destroy(rpmsg, queue); queue = NULL; }
}

static int handle_message(void)
{
    test_message_t recv_msg, send_msg;
    uint32_t len, recv_addr;
    int status;

    status = rpmsg_queue_recv(rpmsg, queue, &recv_addr,
                              (char *)&recv_msg, sizeof(recv_msg), &len, RL_BLOCK);
    if (status != RL_SUCCESS) return -1;

    remote_addr = recv_addr;
    rpmsg_log_cpu1_printf("Received: cmd=0x%02X data=0x%08X\r\n",
                           (unsigned)recv_msg.command, (unsigned)recv_msg.data);
    switch (recv_msg.command)
    {
        case TEST_CMD_PING:
            rpmsg_log_cpu1_printf("PING -> PONG\r\n");
            send_msg.data = recv_msg.data;
            send_msg.command = TEST_CMD_PONG;
            rpmsg_lite_send(rpmsg, ept, remote_addr, (char *)&send_msg, sizeof(send_msg), RL_BLOCK);
            break;
        case TEST_CMD_DATA:
            rpmsg_log_cpu1_printf("DATA -> ACK\r\n");
            send_msg.data = recv_msg.data;
            send_msg.command = TEST_CMD_ACK;
            rpmsg_lite_send(rpmsg, ept, remote_addr, (char *)&send_msg, sizeof(send_msg), RL_BLOCK);
            break;
        default:
            /* NS announcements or other internal messages - ignore */
            return 0;
    }
    return 1;  /* Valid test message processed */
}

int rpmsg_test_run(void)
{
    rpmsg_log_cpu1_printf("\r\n=== RPMsg Test (CPU1) ===\r\n\r\n");

    if (rpmsg_test_init() != 0) return -1;

    rpmsg_log_cpu1_printf("Waiting for messages...\r\n");

    int count = 0;
    while (count < 206)
    {
        int ret = handle_message();
        if (ret < 0) break;   /* Error */
        if (ret == 1) count++; /* Valid test message processed */
        /* ret == 0: internal message (NS, etc.), skip */
    }

    rpmsg_log_cpu1_printf("Processed %d messages\r\n", count);
    //rpmsg_test_deinit();
    return 0;
}
