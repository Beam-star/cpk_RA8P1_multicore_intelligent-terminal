/*
 * rpmsg_bulk_test.c - CPU0 共享内存直通测试（使用 rpmsg_core 共享实例）
 */

#include "rpmsg_core.h"
#include "rpmsg_lite.h"
#include "rpmsg_queue.h"
#include "rpmsg_log.h"
#include "rpmsg_bulk.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

#define LOCAL_EPT_ADDR        (40U)
#define REMOTE_EPT_ADDR       (30U)

static struct rpmsg_lite_instance *rpmsg = NULL;
static rpmsg_queue_handle queue = NULL;
static struct rpmsg_lite_endpoint *ept = NULL;
static volatile uint32_t remote_addr = REMOTE_EPT_ADDR;

static int bulk_test_init(void)
{
    rpmsg = (struct rpmsg_lite_instance *)rpmsg_core_get_instance();
    if (rpmsg == NULL) return -1;

    queue = rpmsg_queue_create(rpmsg);
    if (queue == NULL) return -1;

    ept = rpmsg_lite_create_ept(rpmsg, LOCAL_EPT_ADDR, rpmsg_queue_rx_cb, queue);
    if (ept == NULL) return -1;

    printf("[CPU0] Bulk test ready (data area 0x%08X +%dKB)\r\n",
           (unsigned)BULK_SHMEM_ADDR, BULK_SHMEM_SIZE / 1024);
    return 0;
}

static void bulk_test_deinit(void)
{
    if (ept) { rpmsg_lite_destroy_ept(rpmsg, ept); ept = NULL; }
    if (queue) { rpmsg_queue_destroy(rpmsg, queue); queue = NULL; }
}

static int bulk_notify_and_wait(uint32_t offset, uint32_t size, uint32_t checksum)
{
    bulk_notify_t notify = { BULK_CMD_PUT, offset, size, checksum };
    int status = rpmsg_lite_send(rpmsg, ept, remote_addr,
                                  (char *)&notify, sizeof(notify), RL_BLOCK);
    if (status != RL_SUCCESS) { printf("[CPU0] ERROR: notify\r\n"); return -1; }

    bulk_notify_t ack;
    uint32_t len, src;
    status = rpmsg_queue_recv(rpmsg, queue, &src,
                               (char *)&ack, sizeof(ack), &len, RL_BLOCK);
    if (status != RL_SUCCESS || ack.command != BULK_CMD_ACK)
    { printf("[CPU0] ERROR: no ACK\r\n"); return -1; }

    printf("[CPU0] ACK: offset=%u size=%u\r\n", (unsigned)ack.offset, (unsigned)ack.size);
    return 0;
}

int rpmsg_bulk_test_run(void)
{
    printf("\r\n=== Bulk Transfer Test (CPU0) ===\r\n\r\n");
    if (bulk_test_init() != 0) return -1;

    vTaskDelay(pdMS_TO_TICKS(1000));
    //rpmsg_log_cpu0_process();

    int passed = 0, total = 0;

    total++;
    printf("[CPU0] Test 1: 256 bytes\r\n");
    { uint32_t ck = bulk_shmem_fill(0, 256, 0xAA); if (bulk_notify_and_wait(0, 256, ck) == 0) passed++; }
    vTaskDelay(pdMS_TO_TICKS(50));
    //rpmsg_log_cpu0_process();

    total++;
    printf("[CPU0] Test 2: 1024 bytes\r\n");
    { uint32_t ck = bulk_shmem_fill(0, 1024, 0x55); if (bulk_notify_and_wait(0, 1024, ck) == 0) passed++; }
    vTaskDelay(pdMS_TO_TICKS(50));
    //rpmsg_log_cpu0_process();

    total++;
    printf("[CPU0] Test 3: 4096 bytes\r\n");
    { uint32_t ck = bulk_shmem_fill(0, 4096, 0x11); if (bulk_notify_and_wait(0, 4096, ck) == 0) passed++; }
    vTaskDelay(pdMS_TO_TICKS(50));
    //rpmsg_log_cpu0_process();

    total++;
    printf("[CPU0] Test 4: 8192 bytes (full)\r\n");
    { uint32_t ck = bulk_shmem_fill(0, 8192, 0x33); if (bulk_notify_and_wait(0, 8192, ck) == 0) passed++; }
    vTaskDelay(pdMS_TO_TICKS(50));
    //rpmsg_log_cpu0_process();

    total++;
    printf("[CPU0] Test 5: Head/tail verify\r\n");
    {
        uint8_t head, tail;
        bulk_shmem_read(0, &head, 1);
        bulk_shmem_read(8191, &tail, 1);
        if (head == 0x33 && tail == (uint8_t)(0x33 + 8191)) { printf("[CPU0] OK\r\n"); passed++; }
        else printf("[CPU0] FAIL: head=0x%02X tail=0x%02X\r\n", head, tail);
    }

    //bulk_test_deinit();
    printf("\r\n[CPU0] Bulk: %d/%d %s\r\n", passed, total,
           passed == total ? "ALL PASSED" : "SOME FAILED");
    return (passed == total) ? 0 : -1;
}
