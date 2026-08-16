/*
 * rpmsg_bulk_test.c - CPU1 共享内存直通测试（使用 rpmsg_core 共享实例）
 */

#include "rpmsg_core.h"
#include "rpmsg_lite.h"
#include "rpmsg_queue.h"
#include "rpmsg_log.h"
#include "rpmsg_bulk.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

#define LOCAL_EPT_ADDR        (30U)
#define REMOTE_EPT_ADDR       (40U)

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

    rpmsg_log_cpu1_printf("[CPU1] Bulk test ready\r\n");
    return 0;
}

static void bulk_test_deinit(void)
{
    if (ept) { rpmsg_lite_destroy_ept(rpmsg, ept); ept = NULL; }
    if (queue) { rpmsg_queue_destroy(rpmsg, queue); queue = NULL; }
}

static int handle_notify(void)
{
    bulk_notify_t notify;
    uint32_t len, src;
    int status;

    status = rpmsg_queue_recv(rpmsg, queue, &src,
                               (char *)&notify, sizeof(notify), &len, RL_BLOCK);
    if (status != RL_SUCCESS) return -1;

    remote_addr = src;

    if (notify.command == BULK_CMD_PUT)
    {
        rpmsg_log_cpu1_printf("BULK: offset=%u size=%u\r\n",
                               (unsigned)notify.offset, (unsigned)notify.size);

        /* Verify data */
        if (notify.size > 0)
        {
            uint8_t first;
            bulk_shmem_read(notify.offset, &first, 1);
            int ok = 1;
            for (uint32_t i = 0; ok && i < notify.size; i++)
            {
                uint8_t val;
                bulk_shmem_read(notify.offset + i, &val, 1);
                if (val != (uint8_t)(first + i)) { ok = 0; break; }
            }
            rpmsg_log_cpu1_printf("  %s\r\n", ok ? "OK" : "FAIL");
        }

        /* Send ACK */
        bulk_notify_t ack = { BULK_CMD_ACK, notify.offset, notify.size, 0 };
        rpmsg_lite_send(rpmsg, ept, src, (char *)&ack, sizeof(ack), RL_BLOCK);
    }

    return 0;
}

int rpmsg_bulk_test_run(void)
{
    rpmsg_log_cpu1_printf("\r\n=== Bulk Transfer Test (CPU1) ===\r\n");
    if (bulk_test_init() != 0) return -1;

    rpmsg_log_cpu1_printf("Waiting for bulk notifications...\r\n");

    int count = 0;
    while (count < 4)
    {
        if (handle_notify() != 0) break;
        count++;
    }

    //bulk_test_deinit();
    rpmsg_log_cpu1_printf("Bulk test done\r\n");
    return 0;
}
