/*
 * rpmsg_nocopy_test.c - CPU0 零拷贝测试（使用 rpmsg_core 共享实例）
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

    printf("[CPU0] NoCopy test ready\r\n");
    return 0;
}

static void nocopy_test_deinit(void)
{
    if (ept) { rpmsg_lite_destroy_ept(rpmsg, ept); ept = NULL; }
    if (queue) { rpmsg_queue_destroy(rpmsg, queue); queue = NULL; }
}

static int bulk_send(uint32_t data_size)
{
    uint32_t tx_size;
    void *tx_buf = rpmsg_lite_alloc_tx_buffer(rpmsg, &tx_size, RL_BLOCK);
    if (tx_buf == NULL) { printf("[CPU0] ERROR: alloc failed\r\n"); return -1; }

    bulk_hdr_t *hdr = (bulk_hdr_t *)tx_buf;
    hdr->command = CMD_BULK_DATA;
    hdr->size = data_size;

    uint8_t *payload = (uint8_t *)(tx_buf + sizeof(bulk_hdr_t));
    for (uint32_t i = 0; i < data_size; i++)
        payload[i] = (uint8_t)(i & 0xFF);

    printf("[CPU0] Sending %u bytes (nocopy)...\r\n", (unsigned)data_size);
    int status = rpmsg_lite_send_nocopy(rpmsg, ept, remote_addr,
                                         tx_buf, (uint32_t)(sizeof(bulk_hdr_t) + data_size));
    if (status != RL_SUCCESS) { printf("[CPU0] ERROR: send %d\r\n", status); return -1; }

    char *rx_data;
    uint32_t len, src;
    status = rpmsg_queue_recv_nocopy(rpmsg, queue, &src, &rx_data, &len, RL_BLOCK);
    if (status != RL_SUCCESS) { printf("[CPU0] ERROR: no ACK\r\n"); return -1; }

    //rpmsg_log_cpu0_process();
    rpmsg_queue_nocopy_free(rpmsg, rx_data);
    return 0;
}

static int bulk_send_multi(uint32_t total_bytes)
{
    uint32_t max_payload = (uint32_t)RL_BUFFER_PAYLOAD_SIZE - (uint32_t)sizeof(bulk_hdr_t);
    uint32_t msgs = (total_bytes + max_payload - 1) / max_payload;

    printf("[CPU0] Multi: %u bytes in %u msgs\r\n", (unsigned)total_bytes, (unsigned)msgs);

    for (uint32_t seq = 0; seq < msgs; seq++)
    {
        uint32_t offset = seq * max_payload;
        uint32_t chunk = total_bytes - offset;
        if (chunk > max_payload) chunk = max_payload;

        uint32_t tx_size;
        void *tx_buf = rpmsg_lite_alloc_tx_buffer(rpmsg, &tx_size, RL_BLOCK);
        if (tx_buf == NULL) return -1;

        bulk_hdr_t *hdr = (bulk_hdr_t *)tx_buf;
        hdr->command = CMD_BULK_DATA;
        hdr->size = chunk;

        uint8_t *payload = (uint8_t *)(tx_buf + sizeof(bulk_hdr_t));
        for (uint32_t i = 0; i < chunk; i++)
            payload[i] = (uint8_t)((offset + i) & 0xFF);

        if (rpmsg_lite_send_nocopy(rpmsg, ept, remote_addr,
                                    tx_buf, (uint32_t)(sizeof(bulk_hdr_t) + chunk)) != RL_SUCCESS)
            return -1;

        char *rx_data;
        uint32_t len, src;
        if (rpmsg_queue_recv_nocopy(rpmsg, queue, &src, &rx_data, &len, RL_BLOCK) == RL_SUCCESS)
            rpmsg_queue_nocopy_free(rpmsg, rx_data);
        else
            return -1;
    }

    printf("[CPU0] Multi done\r\n");
    return 0;
}

int rpmsg_nocopy_test_run(void)
{
    printf("\r\n=== NoCopy Test (CPU0) ===\r\n\r\n");
    if (nocopy_test_init() != 0) return -1;

    vTaskDelay(pdMS_TO_TICKS(1000));
    //rpmsg_log_cpu0_process();

    int passed = 0, total = 0;
    static const uint32_t sizes[] = { 8, 64, 128, 256, 488 };
    printf("\r\n[CPU0] --- Single Msg ---\r\n");
    for (int i = 0; i < (int)(sizeof(sizes)/sizeof(sizes[0])); i++)
    {
        total++;
        printf("[CPU0] %d/%d: %u bytes\r\n", i+1, total, (unsigned)sizes[i]);
        if (bulk_send(sizes[i]) == 0) passed++;
        vTaskDelay(pdMS_TO_TICKS(50));
        //rpmsg_log_cpu0_process();
    }

    printf("\r\n[CPU0] --- Multi Msg (2KB) ---\r\n");
    total++;
    if (bulk_send_multi(2048) == 0) passed++;
    //rpmsg_log_cpu0_process();

    //nocopy_test_deinit();
    printf("\r\n[CPU0] NoCopy: %d/%d %s\r\n", passed, total,
           passed == total ? "ALL PASSED" : "SOME FAILED");
    return (passed == total) ? 0 : -1;
}
