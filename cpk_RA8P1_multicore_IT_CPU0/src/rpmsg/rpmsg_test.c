/*
 * rpmsg_test.c - CPU0 基础通信测试（使用 rpmsg_core 共享实例）
 */

#include "rpmsg_core.h"
#include "rpmsg_lite.h"
#include "rpmsg_queue.h"
#include "rpmsg_log.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

#define LOCAL_EPT_ADDR        (40U)
#define REMOTE_EPT_ADDR       (30U)

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
    if (rpmsg == NULL) { printf("[CPU0] ERROR: RPMsg instance not ready\r\n"); return -1; }

    queue = rpmsg_queue_create(rpmsg);
    if (queue == NULL) { printf("[CPU0] ERROR: queue failed\r\n"); return -1; }

    ept = rpmsg_lite_create_ept(rpmsg, LOCAL_EPT_ADDR, rpmsg_queue_rx_cb, queue);
    if (ept == NULL) { printf("[CPU0] ERROR: endpoint failed\r\n"); return -1; }

    printf("[CPU0] Test initialized (ept=%d)\r\n", LOCAL_EPT_ADDR);
    return 0;
}

static void rpmsg_test_deinit(void)
{
    rpmsg_log_cpu0_deinit();
    if (ept) { rpmsg_lite_destroy_ept(rpmsg, ept); ept = NULL; }
    if (queue) { rpmsg_queue_destroy(rpmsg, queue); queue = NULL; }
}

static int test_ping_pong(void)
{
    test_message_t send_msg, recv_msg;
    uint32_t len, recv_addr;

    printf("\r\n[CPU0] === Test 1: Ping-Pong ===\r\n");
    //rpmsg_log_cpu0_process();

    send_msg.data = 0x12345678;
    send_msg.command = TEST_CMD_PING;

    printf("[CPU0] Sending PING...\r\n");
    int status = rpmsg_lite_send(rpmsg, ept, remote_addr,
                                 (char *)&send_msg, sizeof(send_msg), RL_BLOCK);
    if (status != RL_SUCCESS) { printf("[CPU0] ERROR: send %d\r\n", status); return -1; }

    printf("[CPU0] Waiting for PONG...\r\n");
    status = rpmsg_queue_recv(rpmsg, queue, &recv_addr,
                              (char *)&recv_msg, sizeof(recv_msg), &len, RL_BLOCK);
    //rpmsg_log_cpu0_process();

    if (status != RL_SUCCESS) { printf("[CPU0] ERROR: recv %d\r\n", status); return -1; }

    printf("[CPU0] Received: cmd=0x%02X data=0x%08X\r\n",
           (unsigned)recv_msg.command, (unsigned)recv_msg.data);
    return (recv_msg.command == TEST_CMD_PONG) ? 0 : -1;
}

static int test_data_transfer(void)
{
    test_message_t send_msg, recv_msg;
    uint32_t len, recv_addr;

    printf("\r\n[CPU0] === Test 2: Data Transfer (5 msgs) ===\r\n");
    for (int i = 0; i < 5; i++)
    {
        send_msg.data = 0xA5A50000 | i;
        send_msg.command = TEST_CMD_DATA;
        int status = rpmsg_lite_send(rpmsg, ept, remote_addr,
                                     (char *)&send_msg, sizeof(send_msg), RL_BLOCK);
        if (status != RL_SUCCESS) { printf("[CPU0] ERROR: send %d\r\n", i); return -1; }

        status = rpmsg_queue_recv(rpmsg, queue, &recv_addr,
                                  (char *)&recv_msg, sizeof(recv_msg), &len, RL_BLOCK);
        //rpmsg_log_cpu0_process();

        if (status != RL_SUCCESS || recv_msg.command != TEST_CMD_ACK)
        { printf("[CPU0] ERROR: ACK %d\r\n", i); return -1; }

        printf("[CPU0] ACK %d\r\n", i);
    }
    return 0;
}
/*压力测试会大量占用buffer数量,快速的多次的小数据传输,总的所有批次完成速度会比较慢*/
static int test_stress(void)
{
    test_message_t send_msg, recv_msg;
    uint32_t len, recv_addr;
    int ok = 0;

    printf("\r\n[CPU0] === Test 3: Stress (200 msgs) ===\r\n");
    for (int i = 0; i < 200; i++)
    {
    	    send_msg.data =  i;
        send_msg.command = TEST_CMD_DATA;
        int status = rpmsg_lite_send(rpmsg, ept, remote_addr,
                                     (char *)&send_msg, sizeof(send_msg), RL_BLOCK);
        if (status != RL_SUCCESS) continue;

        status = rpmsg_queue_recv(rpmsg, queue, &recv_addr,
                                  (char *)&recv_msg, sizeof(recv_msg), &len, RL_BLOCK);
        //if ((i % 5) == 0) rpmsg_log_cpu0_process();

        if (status == RL_SUCCESS && recv_msg.command == TEST_CMD_ACK) ok++;
        //vTaskDelay(200);
    }
    printf("[CPU0] %d/200 ACKed\r\n", ok);
    return (ok >= 198) ? 0 : -1;
}

int rpmsg_test_run(void)
{
    int passed = 0, total = 0;

    printf("\r\n=== RPMsg Test Suite (CPU0) ===\r\n\r\n");

    if (rpmsg_test_init() != 0) return -1;

    vTaskDelay(pdMS_TO_TICKS(1000));
    //rpmsg_log_cpu0_process();

    total++; if (test_ping_pong() == 0) { passed++; printf("[CPU0] Test 1 PASSED\r\n"); }
    vTaskDelay(pdMS_TO_TICKS(100)); //rpmsg_log_cpu0_process();
    total++; if (test_data_transfer() == 0) { passed++; printf("[CPU0] Test 2 PASSED\r\n"); }
    vTaskDelay(pdMS_TO_TICKS(100)); //rpmsg_log_cpu0_process();
    total++; if (test_stress() == 0) { passed++; printf("[CPU0] Test 3 PASSED\r\n"); }
    //rpmsg_log_cpu0_process();

    //rpmsg_test_deinit();

    printf("\r\n[CPU0] Result: %d/%d %s\r\n", passed, total,
           passed == total ? "ALL PASSED" : "SOME FAILED");
    return (passed == total) ? 0 : -1;
}
