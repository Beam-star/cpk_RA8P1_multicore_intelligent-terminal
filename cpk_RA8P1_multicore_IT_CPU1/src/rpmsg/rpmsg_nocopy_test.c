/*
 * rpmsg_nocopy_test.c - CPU1 零拷贝测试（使用 rpmsg_core 共享实例）
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

#define CMD_BULK_DATA         (0x10)
#define CMD_BULK_ACK          (0x11)

typedef struct { uint32_t command; uint32_t size; } bulk_hdr_t;

static struct rpmsg_lite_instance *rpmsg = NULL;
static rpmsg_queue_handle queue = NULL;
static struct rpmsg_lite_endpoint *ept = NULL;
static volatile uint32_t remote_addr = REMOTE_EPT_ADDR;

static int nocopy_test_init(void)
{
    rpmsg = (struct rpmsg_lite_instance *)rpmsg_core_get_instance();
    if (rpmsg == NULL) return -1;

    queue = rpmsg_queue_create(rpmsg);
    if (queue == NULL) return -1;

    ept = rpmsg_lite_create_ept(rpmsg, LOCAL_EPT_ADDR, rpmsg_queue_rx_cb, queue);
    if (ept == NULL) return -1;

    rpmsg_log_cpu1_printf("[CPU1] NoCopy test ready\r\n");
    return 0;
}

static void nocopy_test_deinit(void)
{
    if (ept) { rpmsg_lite_destroy_ept(rpmsg, ept); ept = NULL; }
    if (queue) { rpmsg_queue_destroy(rpmsg, queue); queue = NULL; }
}

static int handle_message(void)
{
    char *rx_data;
    uint32_t len, recv_addr;
    int status;

    status = rpmsg_queue_recv_nocopy(rpmsg, queue, &recv_addr, &rx_data, &len, RL_BLOCK);
    if (status != RL_SUCCESS) return 0;

    remote_addr = recv_addr;
    bulk_hdr_t *hdr = (bulk_hdr_t *)rx_data;
    rpmsg_log_cpu1_printf("Received: cmd=0x%02X size=%u\r\n",
                           (unsigned)hdr->command, (unsigned)hdr->size);

    if (hdr->command == CMD_BULK_DATA)
    {
        uint8_t *payload = (uint8_t *)(rx_data + sizeof(bulk_hdr_t));
        uint32_t data_size = hdr->size;
        int ok = 1;

        if (data_size > 0)
        {
            uint8_t first = payload[0];
            for (uint32_t i = 0; i < data_size; i++)
            {
                if (payload[i] != (uint8_t)(first + i))
                {
                    rpmsg_log_cpu1_printf("  Mismatch at %u\r\n", (unsigned)i);
                    ok = 0;
                    break;
                }
            }
        }
        rpmsg_log_cpu1_printf("  %s (%u bytes)\r\n", ok ? "OK" : "FAIL", (unsigned)data_size);

        /* Send ACK */
        uint32_t tx_size;
        void *tx_buf = rpmsg_lite_alloc_tx_buffer(rpmsg, &tx_size, RL_BLOCK);
        if (tx_buf != NULL)
        {
            bulk_hdr_t *ack = (bulk_hdr_t *)tx_buf;
            ack->command = CMD_BULK_ACK;
            ack->size = data_size;
            rpmsg_lite_send_nocopy(rpmsg, ept, remote_addr,
                                    tx_buf, (uint32_t)sizeof(bulk_hdr_t));
        }
    }

    rpmsg_queue_nocopy_free(rpmsg, rx_data);
    return 0;
}

int rpmsg_nocopy_test_run(void)
{
    rpmsg_log_cpu1_printf("\r\n=== NoCopy Test (CPU1) ===\r\n");
    if (nocopy_test_init() != 0) return -1;

    rpmsg_log_cpu1_printf("Waiting for bulk data...\r\n");

    int count = 0;
    while (count < 10)
    {
        if (handle_message() != 0) break;
        count++;
    }

    rpmsg_log_cpu1_printf("NoCopy test done\r\n");
    return 0;
}
