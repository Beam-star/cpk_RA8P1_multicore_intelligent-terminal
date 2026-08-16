/*
 * pcdc_thread_entry.cpp
 *
 * USB PCDC (CDC ACM) virtual COM port.
 *
 * Two modes coexist in the main loop:
 *   1. Echo mode  �?normal terminal data is echoed back (debugging).
 *   2. Flash mode �?protocol frames (STX byte prefix) execute flash commands.
 *
 * CDC class requests (GET_LINE_CODING, SET_LINE_CODING) are deferred from
 * the ISR callback to this thread via a binary semaphore �?the FSP API
 * forbids calling periControlDataSet/periControlDataGet from the RTOS
 * callback (ISR context).
 */

#define pcdc_freertos_callback  pcdc_freertos_callback
#include <pcdc_thread.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "hal_data.h"
#include "r_usb_pcdc_api.h"
#include "r_usb_basic_api.h"
#include "driver/w25q256/w25q256.h"
#include "driver/w25q256/w25q256_partition.h"
#include "pcdc_flash_protocol.h"

/* -------------------------------------------------------------------------- */
/* Shared state (set by callback, polled by thread)                           */
/* -------------------------------------------------------------------------- */

/* Protocol buffer (used for both echo and flash protocol) */
static uint8_t  g_proto_buf[PCDC_PROTO_BUF_SIZE];

static volatile uint32_t g_read_size  = 0;
static volatile bool     g_configured = false;
static volatile bool     g_read_done  = false;
static volatile bool     g_write_done = false;
static volatile bool     g_detached   = false;
static volatile bool     g_proto_write_pending = false; /* protocol response in flight */

/* Deferred class-request handling �?callback -> thread */
static SemaphoreHandle_t g_class_req_sem   = NULL;
static usb_setup_t       g_class_req_setup;
static volatile bool     g_class_req_pending = false;

/* Event ring buffer for diagnosis */
#define EVT_LOG_N  16
static volatile uint32_t g_evt_log[EVT_LOG_N];
static volatile int32_t  g_on_log[EVT_LOG_N];
static volatile uint32_t g_evt_idx = 0;

/* Static buffers for protocol handler (must NOT be on stack �?too large) */
static uint8_t          g_rd_buf[PCDC_MAX_PAYLOAD];    /* READ response     */
static uint8_t          g_dir_buf[PART_ASSET_DIR_SIZE]; /* DIR_READ response */
static flash_asset_dir_t g_asset_dir;                   /* DIR_ADD working   */

/* -------------------------------------------------------------------------- */
/* syscall stub                                                                */
/* -------------------------------------------------------------------------- */

extern "C" void _exit(int status) __attribute__((weak));
extern "C" void _exit(int status)
{
    (void)status;
    while (1) { __asm volatile("wfi"); }
}

/* -------------------------------------------------------------------------- */
/* Helpers: send ACK / NACK                                                    */
/* -------------------------------------------------------------------------- */

static fsp_err_t send_ack(const uint8_t *payload, uint16_t len)
{
    uint8_t tx[PCDC_MAX_FRAME];
    uint16_t frame_len = pcdc_build_frame(tx, PCDC_CMD_ACK, payload, len);
    if (frame_len == 0) return FSP_ERR_USB_FAILED;
    g_write_done = false;
    return R_USB_Write(&g_basic0_ctrl, tx, frame_len, USB_CLASS_PCDC);
}

static fsp_err_t send_nack(uint8_t error_code)
{
    return send_ack(&error_code, 1);  /* NACK via ACK frame; error_code is payload */
}

/* -------------------------------------------------------------------------- */
/* Protocol frame handler (called from PCDC thread, NOT from ISR)              */
/* -------------------------------------------------------------------------- */

static void handle_protocol_frame(const uint8_t *buf, uint32_t size)
{
    uint16_t payload_len = 0;
    uint8_t  cmd = pcdc_validate_frame(buf, size, &payload_len);

    if (cmd == PCDC_CMD_NACK) {
        printf("[PCDC] Frame invalid (CRC fail or bad format), size=%lu\r\n",
               (unsigned long)size);
        send_nack(PCDC_ERR_CRC);
        return;
    }

    printf("[PCDC] CMD 0x%02X, payload=%u\r\n", (unsigned)cmd, payload_len);

    const uint8_t *payload = pcdc_payload_ptr(buf);
    fsp_err_t      usb_err;

    switch (cmd) {

    /* ---- PING ---- */
    case PCDC_CMD_PING:
        printf("[PCDC] PING -> ACK\r\n");
        send_ack((const uint8_t *)"PCDC_FLASH/1.0", 15);
        break;

    /* ---- INFO ---- */
    case PCDC_CMD_INFO: {
        pcdc_flash_info_t info;
        info.total_size  = W25Q256_CAPACITY;
        info.sector_size = W25Q256_SECTOR_SIZE;
        info.page_size   = W25Q256_PAGE_SIZE;
        info.max_payload = PCDC_MAX_PAYLOAD;
        send_ack((const uint8_t *)&info, sizeof(info));
        break;
    }

    /* ---- ERASE ---- */
    case PCDC_CMD_ERASE: {
        if (payload_len < 8) { send_nack(PCDC_ERR_BAD_LEN); break; }

        uint32_t addr = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8)
                      | ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
        uint32_t size = (uint32_t)payload[4] | ((uint32_t)payload[5] << 8)
                      | ((uint32_t)payload[6] << 16) | ((uint32_t)payload[7] << 24);

        if (addr >= W25Q256_CAPACITY || addr + size > W25Q256_CAPACITY) {
            send_nack(PCDC_ERR_ADDR_RANGE);
            break;
        }

        if (w25q256_open() != W25Q256_OK) { send_nack(PCDC_ERR_FLASH_ERASE); break; }

        /* Round up to sector boundary and erase sector by sector */
        uint32_t end      = addr + size;
        uint32_t sec_addr = addr & ~(W25Q256_SECTOR_SIZE - 1);
        w25q256_err_t werr = W25Q256_OK;

        while (sec_addr < end) {
            werr = w25q256_erase_sector(sec_addr);
            if (werr != W25Q256_OK) break;
            sec_addr += W25Q256_SECTOR_SIZE;
        }

        w25q256_close();

        if (werr != W25Q256_OK) {
            printf("[PCDC] Erase err at 0x%lX: %s\r\n",
                   (unsigned long)sec_addr, w25q256_err_str(werr));
            send_nack(PCDC_ERR_FLASH_ERASE);
        } else {
            printf("[PCDC] Erased 0x%lX +%lu -> %lu sectors\r\n",
                   (unsigned long)addr, (unsigned long)size,
                   (unsigned long)((sec_addr - (addr & ~(W25Q256_SECTOR_SIZE - 1)))
                                   / W25Q256_SECTOR_SIZE));
            send_ack(NULL, 0);
        }
        break;
    }

    /* ---- WRITE ---- */
    case PCDC_CMD_WRITE: {
        if (payload_len < 4) { send_nack(PCDC_ERR_BAD_LEN); break; }

        uint32_t addr = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8)
                      | ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
        uint32_t data_len = payload_len - 4;

        if (data_len == 0) { send_nack(PCDC_ERR_BAD_LEN); break; }
        if (addr >= W25Q256_CAPACITY || addr + data_len > W25Q256_CAPACITY) {
            send_nack(PCDC_ERR_ADDR_RANGE);
            break;
        }

        if (w25q256_open() != W25Q256_OK) { send_nack(PCDC_ERR_FLASH_WRITE); break; }

        w25q256_err_t werr = w25q256_write(addr, payload + 4, data_len);

        w25q256_close();

        if (werr != W25Q256_OK) {
            printf("[PCDC] Write err at 0x%lX: %s\r\n",
                   (unsigned long)addr, w25q256_err_str(werr));
            send_nack(PCDC_ERR_FLASH_WRITE);
        } else {
            send_ack(NULL, 0);
        }
        break;
    }

    /* ---- READ ---- */
    case PCDC_CMD_READ: {
        if (payload_len < 6) { send_nack(PCDC_ERR_BAD_LEN); break; }

        uint32_t addr = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8)
                      | ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
        uint16_t size = (uint16_t)payload[4] | ((uint16_t)payload[5] << 8);

        if (size > PCDC_MAX_PAYLOAD) { send_nack(PCDC_ERR_BAD_LEN); break; }
        if (addr >= W25Q256_CAPACITY || addr + size > W25Q256_CAPACITY) {
            send_nack(PCDC_ERR_ADDR_RANGE);
            break;
        }

        /* Memory-mapped read �?need OSPI_B open for XIP access */
        if (w25q256_open() != W25Q256_OK) { send_nack(PCDC_ERR_FLASH_ERASE); break; }
        memcpy(g_rd_buf, (const void *)(W25Q256_MEM_BASE + addr), size);
        w25q256_close();

        send_ack(g_rd_buf, size);
        break;
    }

    /* ---- DIR_READ ---- */
    case PCDC_CMD_DIR_READ: {
        if (w25q256_open() != W25Q256_OK) { send_nack(PCDC_ERR_FLASH_ERASE); break; }
        memcpy(g_dir_buf, (const void *)(W25Q256_MEM_BASE + PART_ASSET_DIR_OFFSET),
               sizeof(g_dir_buf));
        w25q256_close();
        send_ack(g_dir_buf, sizeof(g_dir_buf));
        break;
    }

    /* ---- DIR_ADD ---- */
    case PCDC_CMD_DIR_ADD: {
        if (payload_len < 40) { send_nack(PCDC_ERR_BAD_LEN); break; }

        /* Payload: name(32B) + offset(4B) + size(4B) = 40B */
        const char *aname = (const char *)payload;
        uint32_t    aoff  = (uint32_t)payload[32] | ((uint32_t)payload[33] << 8)
                          | ((uint32_t)payload[34] << 16) | ((uint32_t)payload[35] << 24);
        uint32_t    asize = (uint32_t)payload[36] | ((uint32_t)payload[37] << 8)
                          | ((uint32_t)payload[38] << 16) | ((uint32_t)payload[39] << 24);

        /* Open W25Q256 first �?XIP reads require OSPI_B to be active */
        if (w25q256_open() != W25Q256_OK) { send_nack(PCDC_ERR_FLASH_ERASE); break; }

        /* Read current directory via XIP */
        memcpy(&g_asset_dir, (const void *)(W25Q256_MEM_BASE + PART_ASSET_DIR_OFFSET),
               sizeof(g_asset_dir));

        /* Validate or initialize */
        if (g_asset_dir.magic != FLASH_ASSET_MAGIC
            || g_asset_dir.count > FLASH_ASSET_MAX_ENTRIES) {
            memset(&g_asset_dir, 0, sizeof(g_asset_dir));
            g_asset_dir.magic = FLASH_ASSET_MAGIC;
            g_asset_dir.count = 0;
        }

        if (g_asset_dir.count >= FLASH_ASSET_MAX_ENTRIES) {
            w25q256_close();
            send_nack(PCDC_ERR_FLASH_WRITE);
            printf("[PCDC] DIR_ADD: directory full!\r\n");
            break;
        }

        /* Append entry */
        flash_asset_entry_t *e = &g_asset_dir.entries[g_asset_dir.count];
        strncpy(e->name, aname, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        e->offset = aoff;
        e->size   = asize;
        e->crc32  = 0;
        g_asset_dir.count++;

        /* Erase and write back */
        w25q256_err_t werr = w25q256_erase_sector(PART_ASSET_DIR_OFFSET);
        if (werr == W25Q256_OK) {
            werr = w25q256_write(PART_ASSET_DIR_OFFSET,
                                (const uint8_t *)&g_asset_dir,
                                sizeof(g_asset_dir));
        }

        w25q256_close();

        if (werr != W25Q256_OK) {
            printf("[PCDC] DIR_ADD write err: %s\r\n", w25q256_err_str(werr));
            send_nack(PCDC_ERR_FLASH_WRITE);
        } else {
            printf("[PCDC] DIR_ADD: '%s' offset=0x%lX size=%lu (#%lu)\r\n",
                   e->name, (unsigned long)aoff, (unsigned long)asize,
                   (unsigned long)g_asset_dir.count);
            send_ack(NULL, 0);
        }
        break;
    }

    /* ---- RESET ---- */
    case PCDC_CMD_RESET:
        printf("[PCDC] RESET �?rebooting...\r\n");
        send_ack(NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(50));  /* allow ACK to be sent */
        __NVIC_SystemReset();
        break;

    default:
        printf("[PCDC] Unknown cmd: 0x%02X\r\n", (unsigned)cmd);
        send_nack(PCDC_ERR_BAD_CMD);
        break;
    }

    (void)usb_err;  /* unused in most paths */
}

/* -------------------------------------------------------------------------- */
/* RTOS callback                                                               */
/* -------------------------------------------------------------------------- */

extern "C" void pcdc_freertos_callback(usb_event_info_t *p_event_info,
                                       usb_hdl_t         handler,
                                       usb_onoff_t       on_off)
{
    uint32_t i = g_evt_idx;
    g_evt_log[i % EVT_LOG_N] = (uint32_t)p_event_info->event;
    g_on_log[i % EVT_LOG_N]  = (int32_t)on_off;
    g_evt_idx = i + 1;

    /* Only DETACH event means real disconnect; SUSPEND also comes with USB_OFF */
    if (USB_STATUS_DETACH == p_event_info->event) {
        g_configured = false;
        g_detached   = true;
        return;
    }

    /* ---- CDC class requests: DEFER to thread (DO NOT call periControl* here) ----
     *
     * FSP docs: "Do not call periControlDataGet/Set from callback (for RTOS)."
     * These APIs manipulate USB FIFO registers while the control transfer state
     * machine is mid-flight.  Instead we save the setup packet, signal the
     * PCDC thread via semaphore, and handle the request in TASK CONTEXT.
     */
    if (USB_STATUS_REQUEST == p_event_info->event) {
        g_class_req_setup   = p_event_info->setup;
        g_class_req_pending = true;

        if (g_class_req_sem) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xSemaphoreGiveFromISR(g_class_req_sem, &xHigherPriorityTaskWoken);
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        }

        /* Log setup packet for diagnosis */
        g_evt_log[i % EVT_LOG_N] = 0x80000000UL
            | (p_event_info->setup.request_type & 0xFFFF);
        g_on_log[i % EVT_LOG_N]  = (int32_t)p_event_info->setup.request_value;
        g_evt_idx = i + 2;
        i = g_evt_idx;
        g_evt_log[i % EVT_LOG_N] = p_event_info->setup.request_index;
        g_on_log[i % EVT_LOG_N]  = (int32_t)p_event_info->setup.request_length;

        return;
    }

    switch (p_event_info->event) {
    case USB_STATUS_CONFIGURED:
        g_configured = true;
        break;
    case USB_STATUS_READ_COMPLETE:
        g_read_size = p_event_info->data_size;
        g_read_done = true;
        break;
    case USB_STATUS_WRITE_COMPLETE:
        g_write_done = true;
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* Deferred class-request handler (called from PCDC thread, NOT from ISR)     */
/* -------------------------------------------------------------------------- */

static void handle_class_request(void)
{
    uint16_t req = g_class_req_setup.request_type & USB_BREQUEST;
    fsp_err_t err;

    if (USB_PCDC_GET_LINE_CODING == req) {
        uint8_t lc[7] = {0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08};
        err = g_basic0.p_api->periControlDataSet(&g_basic0_ctrl, lc, 7);
        if (FSP_SUCCESS != err) {
            printf("[PCDC] GET_LINE_CODING err: 0x%lx\r\n", (unsigned long)err);
        }
    }
    else if (USB_PCDC_SET_LINE_CODING == req) {
        uint8_t lc[7];
        err = g_basic0.p_api->periControlDataGet(&g_basic0_ctrl, lc, 7);
        if (FSP_SUCCESS != err) {
            printf("[PCDC] SET_LINE_CODING err: 0x%lx\r\n", (unsigned long)err);
        }
        else {
            uint32_t baud = (uint32_t)lc[0] | ((uint32_t)lc[1] << 8)
                          | ((uint32_t)lc[2] << 16) | ((uint32_t)lc[3] << 24);
            printf("[PCDC] LineCoding: %lu,%u,%u,%u\r\n",
                   (unsigned long)baud, (unsigned)lc[4],
                   (unsigned)lc[5], (unsigned)lc[6]);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* PCDC thread entry                                                          */
/* -------------------------------------------------------------------------- */

extern "C" void pcdc_thread_entry(void *pvParameters);

void pcdc_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED(pvParameters);

    /*
     * PCDC is only active in MODE_PCDC_ECHO (g_pcdc_echo_mode == true).
     * In MODE_LVGL_DEMO and MODE_CAMERA the PCDC thread is still created
     * by FSP auto-generated code (main.c:103), but USB must NOT be opened
     * because it would enable USB interrupts that compete with I2C/ICU/VIN
     * for CPU time and potentially trigger xQueueGiveFromISR assertions.
     *
     * Self-delete the task immediately �?no USB init, zero side effects.
     */
    extern bool g_pcdc_echo_mode;
    if (!g_pcdc_echo_mode) {
        vTaskDelete(NULL);
    }

    g_class_req_sem = xSemaphoreCreateBinary();
    if (NULL == g_class_req_sem) {
        printf("[PCDC] FATAL: semaphore create failed\r\n");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    printf("[PCDC] Starting, heap: %lu\r\n",
           (unsigned long)xPortGetFreeHeapSize());

    fsp_err_t err = R_USB_Open(&g_basic0_ctrl, &g_basic0_cfg);
    if (FSP_SUCCESS != err) {
        printf("[PCDC] FATAL: R_USB_Open = 0x%lx\r\n", (unsigned long)err);
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    printf("[PCDC] USB opened, heap: %lu\r\n",
           (unsigned long)xPortGetFreeHeapSize());

    /* --- Wait for CONFIGURED --- */
    printf("[PCDC] Waiting for enumeration...\r\n");
    {
        uint32_t tick = 0;
        while (!g_configured && !g_detached) {
            vTaskDelay(pdMS_TO_TICKS(200));
            tick++;
            if ((tick % 10) == 0) {
                printf("[PCDC] wait... cfg=%d detach=%d evts=%lu\r\n",
                       (int)g_configured, (int)g_detached,
                       (unsigned long)g_evt_idx);
            }
        }
    }

    if (!g_configured) {
        uint32_t total = g_evt_idx;
        printf("[PCDC] FAILED after %lu events. Last %u:\r\n",
               (unsigned long)total, (unsigned)EVT_LOG_N);
        for (uint32_t i = 0; i < EVT_LOG_N; i++) {
            printf("  evt=%lu onoff=%ld\r\n",
                   (unsigned long)g_evt_log[i],
                   (long)g_on_log[i]);
        }

        printf("[PCDC] Waiting for re-enumeration...\r\n");
        g_detached = false;
        g_evt_idx  = 0;
        while (!g_configured) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    printf("[PCDC] CONFIGURED �?echo+flash ready\r\n");

    /* --- Post first read --- */
    g_read_done = false;
    err = R_USB_Read(&g_basic0_ctrl, g_proto_buf, sizeof(g_proto_buf),
                     USB_CLASS_PCDC);
    if (FSP_SUCCESS != err) {
        printf("[PCDC] Read post err: 0x%lx\r\n", (unsigned long)err);
    }

    /* --- Main loop: echo + flash protocol --- */
    uint32_t echo_count = 0;

    while (1) {
        /* Wait for event: class request (semaphore) or 100ms timeout */
        BaseType_t sem_rc = xSemaphoreTake(g_class_req_sem, pdMS_TO_TICKS(100));

        /* ---- Deferred class request handling (HIGHEST priority) ---- */
        if (sem_rc == pdTRUE && g_class_req_pending) {
            g_class_req_pending = false;
            handle_class_request();
        }

        /* ---- DETACH ---- */
        if (g_detached) {
            printf("[PCDC] DETACHED, waiting for reconnect...\r\n");
            g_detached = false;
            g_evt_idx  = 0;
            while (!g_configured) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            printf("[PCDC] Re-configured\r\n");
            g_read_done = false;
            err = R_USB_Read(&g_basic0_ctrl, g_proto_buf,
                             sizeof(g_proto_buf), USB_CLASS_PCDC);
            if (FSP_SUCCESS != err) {
                printf("[PCDC] Read err: 0x%lx\r\n", (unsigned long)err);
            }
            continue;
        }

        /* ---- READ_COMPLETE ---- */
        if (g_read_done) {
            g_read_done = false;
            uint32_t n = g_read_size;

            /*
             * Auto-detect mode by first byte:
             *   STX (0xAA) -> flash protocol frame
             *   otherwise  -> echo back
             */
            if (n >= 5 && g_proto_buf[0] == PCDC_STX) {
                handle_protocol_frame(g_proto_buf, n);
                /* Protocol response is now in-flight via USB Write.
                 * Mark pending so WRITE_COMPLETE will re-post read. */
                g_proto_write_pending = true;
            } else if (n > 0) {
                /* Echo */
                echo_count++;
                if ((echo_count & 0x1F) == 0) {
                    printf("[PCDC] Echo #%lu (%lu B)\r\n",
                           (unsigned long)echo_count, (unsigned long)n);
                }

                g_write_done = false;
                err = R_USB_Write(&g_basic0_ctrl, g_proto_buf, n,
                                  USB_CLASS_PCDC);
                if (FSP_SUCCESS != err) {
                    printf("[PCDC] Echo write err: 0x%lx\r\n",
                           (unsigned long)err);
                }
                /* Echo write is pending �?re-post read after write completes */
                g_proto_write_pending = true;
            }
        }

        /* ---- WRITE_COMPLETE ---- */
        if (g_write_done) {
            g_write_done = false;

            /* Protocol or echo write just completed �?safe to re-post read */
            if (g_proto_write_pending) {
                g_proto_write_pending = false;
                if (!g_detached) {
                    err = R_USB_Read(&g_basic0_ctrl, g_proto_buf,
                                     sizeof(g_proto_buf), USB_CLASS_PCDC);
                    if (FSP_SUCCESS != err) {
                        printf("[PCDC] Re-read err: 0x%lx\r\n",
                               (unsigned long)err);
                    }
                }
            }
        }
    }
}
