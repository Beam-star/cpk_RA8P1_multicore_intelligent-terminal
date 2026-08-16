/**
 ******************************************************************************
 * @file    zw111_fingerprint.c
 * @brief   ZW111 半导体指纹模块驱动（UART6 / SCI6, P909=RX P908=TX, 57600 8N1）
 *
 * 数据流：
 *   UART6 RX 中断 (UART6_Callback) → 环形缓冲 → 阻塞读（带超时）。
 *   所有收发都在单一 worker 任务里串行执行（录入/打卡一次只做一件事）。
 *
 * 协议要点（详见 指纹模组产品用户手册_V1.5.1.pdf 3.x）：
 *   - 帧头 0xEF01，设备地址默认 0xFFFFFFFF（4B 大端）。
 *   - 命令包包标识=01，应答包包标识=07。
 *   - 包长度 = 指令/确认码 + 参数 + 校验和 的总字节数（不含包长度本身）。
 *   - 校验和 = 包标识 + 包长度(2B) + 指令/确认码 + 参数 之和（16 位大端）。
 *   - 多字节参数一律大端（高字节在前）。
 ******************************************************************************
 */

#include "zw111_fingerprint.h"
#include "fp_name_db.h"
#include "hal_data.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <string.h>
#include <stdio.h>

/* ======================================================================== */
/*  可调参数                                                                  */
/* ======================================================================== */

#define ZW111_ENROLL_TIMES        3          /* 录入次数（小面积建议 3 次） */
#define ZW111_ENROLL_PARAM        0x0008     /* bit3=1 允许覆盖 ID */
#define ZW111_IDENTIFY_LEVEL      2          /* 比对分数等级 1~5，默认 2 */
#define ZW111_IDENTIFY_PARAM      0x0000     /* bit2=0 要求返回关键步骤 */

#define ZW111_RX_TIMEOUT_MS       15000      /* 录入/打卡单包应答超时（等待手指） */
#define ZW111_PROBE_TIMEOUT_MS    500        /* 握手/读参数等短应答超时 */

#define ZW111_CAPACITY            100        /* ZW111 指纹库容量（88×112, 100 枚） */

/* ======================================================================== */
/*  协议常量                                                                  */
/* ======================================================================== */

#define ZW111_HEADER0             0xEF
#define ZW111_HEADER1             0x01
#define ZW111_ADDR_DEFAULT        0xFFFFFFFFu
#define ZW111_PKT_CMD             0x01
#define ZW111_PKT_RESP            0x07

#define ZW111_CMD_AUTO_ENROLL     0x31   /* 一站式录入 */
#define ZW111_CMD_AUTO_IDENTIFY   0x32   /* 一站式验证（1:N） */
#define ZW111_CMD_EMPTY           0x0D   /* 清空指纹库 */
#define ZW111_CMD_HANDSHAKE       0x35   /* 握手检测 */
#define ZW111_CMD_VALID_NUM       0x1D   /* 读有效模板个数 */
#define ZW111_CMD_CANCEL          0x30   /* 取消自动注册/自动验证 */

#define ZW111_RING_SIZE           1024
#define ZW111_MAX_FRAME           32     /* 应答最大帧长（头 9 + 体 ≤ 23） */

/* 录入/打卡应答的终止步（参数 1 语义见手册表 3-52 / 3-55） */
#define ZW111_ENROLL_STEP_STORE   0x06   /* 存储模板（录入最后一步） */
#define ZW111_IDENTIFY_STEP_MATCH 0x05   /* 已注册指纹比对（打卡最后一步） */

/* ======================================================================== */
/*  UART 硬件层                                                               */
/* ======================================================================== */

static volatile uint8_t  g_rx_ring[ZW111_RING_SIZE];
static volatile uint32_t g_rx_head = 0;   /* ISR 写 */
static volatile uint32_t g_rx_tail = 0;   /* 任务读 */

static uint8_t s_tx_buf[ZW111_MAX_FRAME];

/* FSP 声明的 UART6 回调（ISR 上下文，仅入环） */
void UART6_Callback(uart_callback_args_t *p_args)
{
    if (p_args->event != UART_EVENT_RX_CHAR) {
        return;
    }
    uint8_t  b    = (uint8_t)p_args->data;
    uint32_t next = (g_rx_head + 1) % ZW111_RING_SIZE;
    if (next != g_rx_tail) {
        g_rx_ring[g_rx_head] = b;
        g_rx_head = next;
    }
}

static bool ring_pop(uint8_t *out)
{
    if (g_rx_head == g_rx_tail) return false;
    *out = g_rx_ring[g_rx_tail];
    g_rx_tail = (g_rx_tail + 1) % ZW111_RING_SIZE;
    return true;
}

/* 清空接收缓冲（发送前调用，丢弃上电握手 0x55 等残留字节） */
static void ring_flush(void)
{
    taskENTER_CRITICAL();
    g_rx_head = g_rx_tail;
    taskEXIT_CRITICAL();
}

/* 取消标志：UI 点叉时置位；worker 在阻塞读里检测到后终止模组操作 */
static volatile bool g_cancel_requested = false;

/* 阻塞读 n 字节，超时返回 false（已读部分丢弃）；用户取消时也立即返回 false */
static bool zw111_rx(uint8_t *buf, uint32_t n, uint32_t timeout_ms)
{
    uint32_t   got   = 0;
    TickType_t start = xTaskGetTickCount();
    while (got < n) {
        while (got < n && ring_pop(&buf[got])) {
            got++;
        }
        if (got >= n) return true;
        if (g_cancel_requested) return false;   /* 取消：立即中止等待 */
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return true;
}

/* 阻塞发一帧（静态缓冲 + 异步 write；发送后必然紧跟阻塞读，串行化保证安全） */
static void zw111_tx(const uint8_t *buf, uint32_t len)
{
    memcpy(s_tx_buf, buf, len);
    g_uart6.p_api->write(&g_uart6_ctrl, s_tx_buf, len);
}

/* ======================================================================== */
/*  协议层                                                                    */
/* ======================================================================== */

/* 发送命令包：EF 01 + 地址(4B) + 01 + 长度(2B) + 指令(1B) + 参数 + 校验和(2B) */
static void zw111_send_command(uint8_t cmd, const uint8_t *params, uint16_t param_len)
{
    uint8_t  frame[ZW111_MAX_FRAME];
    uint16_t len = (uint16_t)(1 + param_len + 2);   /* 指令 + 参数 + 校验和 */
    uint16_t n   = 0;

    frame[n++] = ZW111_HEADER0;
    frame[n++] = ZW111_HEADER1;
    /* 设备地址（大端，默认 0xFFFFFFFF） */
    frame[n++] = (uint8_t)(ZW111_ADDR_DEFAULT >> 24);
    frame[n++] = (uint8_t)(ZW111_ADDR_DEFAULT >> 16);
    frame[n++] = (uint8_t)(ZW111_ADDR_DEFAULT >> 8);
    frame[n++] = (uint8_t)(ZW111_ADDR_DEFAULT);
    frame[n++] = ZW111_PKT_CMD;
    frame[n++] = (uint8_t)(len >> 8);
    frame[n++] = (uint8_t)(len & 0xFF);
    frame[n++] = cmd;

    uint16_t sum = (uint16_t)(ZW111_PKT_CMD + (uint8_t)(len >> 8) + (uint8_t)(len & 0xFF) + cmd);
    for (uint16_t i = 0; i < param_len; i++) {
        frame[n++] = params[i];
        sum = (uint16_t)(sum + params[i]);
    }
    frame[n++] = (uint8_t)(sum >> 8);
    frame[n++] = (uint8_t)(sum & 0xFF);

    ring_flush();
    zw111_tx(frame, n);
}

/*
 * 读一个完整应答包。
 *   返回 0：成功，*confirm=确认码，out_params[]=返回参数，*out_len=参数字节数。
 *   返回 -1：超时 / 帧头错 / 校验错。
 */
static int zw111_read_response(uint8_t *confirm, uint8_t *out_params,
                               uint16_t *out_len, uint32_t timeout_ms)
{
    uint8_t hdr[9];   /* EF 01 + 地址(4) + 包标识(1) + 包长度(2) */

    if (!zw111_rx(hdr, 9, timeout_ms)) {
        return g_cancel_requested ? ZW111_ERR_CANCELLED : -1;
    }
    if (hdr[0] != ZW111_HEADER0 || hdr[1] != ZW111_HEADER1) return -1;
    if (hdr[6] != ZW111_PKT_RESP) return -1;

    uint16_t len = (uint16_t)((hdr[7] << 8) | hdr[8]);
    if (len < 3 || len > ZW111_MAX_FRAME) return -1;   /* 确认码 + 参数 + 校验和 */

    uint8_t body[ZW111_MAX_FRAME];
    if (!zw111_rx(body, len, timeout_ms)) {
        return g_cancel_requested ? ZW111_ERR_CANCELLED : -1;
    }

    /* 校验和 = 包标识 + 包长度(2B) + 确认码 + 参数（不含校验和本身） */
    uint16_t sum = (uint16_t)(hdr[6] + (uint8_t)(len >> 8) + (uint8_t)(len & 0xFF));
    for (uint16_t i = 0; i + 2 < len; i++) {
        sum = (uint16_t)(sum + body[i]);
    }
    uint16_t chk = (uint16_t)((body[len - 2] << 8) | body[len - 1]);
    if (sum != chk) return -1;

    *confirm = body[0];
    uint16_t plen = (uint16_t)(len - 3);   /* 去掉确认码 + 2 字节校验和 */
    if (plen > 0) {
        memcpy(out_params, &body[1], plen);
    }
    if (out_len) *out_len = plen;
    return 0;
}

/* ======================================================================== */
/*  高层操作（阻塞，worker 任务内执行）                                       */
/* ======================================================================== */

static uint16_t s_next_id = 1;   /* 下一个录入 ID（上电后经读有效模板数恢复） */

static int do_enroll(uint16_t *enrolled_id)
{
    uint16_t id = s_next_id;
    uint8_t  params[5];

    params[0] = (uint8_t)(id >> 8);              /* ID 大端 */
    params[1] = (uint8_t)(id & 0xFF);
    params[2] = ZW111_ENROLL_TIMES;              /* 录入次数 */
    params[3] = (uint8_t)(ZW111_ENROLL_PARAM >> 8);
    params[4] = (uint8_t)(ZW111_ENROLL_PARAM & 0xFF);

    printf("[FP] enroll id=%u times=%u\r\n", (unsigned)id, (unsigned)ZW111_ENROLL_TIMES);
    zw111_send_command(ZW111_CMD_AUTO_ENROLL, params, 5);

    int safety = 0;
    while (1) {
        if (++safety > 50) return ZW111_ERR_TRANSPORT;

        uint8_t  confirm, p[8];
        uint16_t plen = 0;
        int rc = zw111_read_response(&confirm, p, &plen, ZW111_RX_TIMEOUT_MS);
        if (rc == ZW111_ERR_CANCELLED) {
            zw111_send_command(ZW111_CMD_CANCEL, NULL, 0);
            return ZW111_ERR_CANCELLED;
        }
        if (rc != 0) {
            return ZW111_ERR_TRANSPORT;
        }
        if (confirm != 0x00) {
            return (int)confirm;   /* 失败确认码 */
        }
        /* 成功步骤：直到「存储模板」参数 1 = 0x06 为止 */
        if (plen >= 2 && p[0] == ZW111_ENROLL_STEP_STORE) {
            if (enrolled_id) *enrolled_id = id;   /* 回传本次录入的 ID */
            s_next_id++;           /* 预留该 ID */
            return 0;
        }
    }
}

static int do_identify(uint16_t *page_id, uint16_t *score)
{
    uint8_t params[5];

    params[0] = (uint8_t)ZW111_IDENTIFY_LEVEL;   /* 分数等级 */
    params[1] = 0xFF;                            /* ID = 0xFFFF → 1:N */
    params[2] = 0xFF;
    params[3] = (uint8_t)(ZW111_IDENTIFY_PARAM >> 8);
    params[4] = (uint8_t)(ZW111_IDENTIFY_PARAM & 0xFF);

    zw111_send_command(ZW111_CMD_AUTO_IDENTIFY, params, 5);

    int safety = 0;
    while (1) {
        if (++safety > 50) return ZW111_ERR_TRANSPORT;

        uint8_t  confirm, p[8];
        uint16_t plen = 0;
        int rc = zw111_read_response(&confirm, p, &plen, ZW111_RX_TIMEOUT_MS);
        if (rc == ZW111_ERR_CANCELLED) {
            zw111_send_command(ZW111_CMD_CANCEL, NULL, 0);
            return ZW111_ERR_CANCELLED;
        }
        if (rc != 0) {
            return ZW111_ERR_TRANSPORT;
        }
        if (confirm != 0x00) {
            return (int)confirm;   /* 09 未搜索到 / 24 库空 / 26 超时 等 */
        }
        /* 成功步骤：直到「已注册指纹比对」参数 = 0x05 为止 */
        if (plen >= 5 && p[0] == ZW111_IDENTIFY_STEP_MATCH) {
            *page_id = (uint16_t)((p[1] << 8) | p[2]);   /* 匹配 ID（大端） */
            *score   = (uint16_t)((p[3] << 8) | p[4]);   /* 得分（大端） */
            return 0;
        }
    }
}

/* 清空指纹库（PS_Empty，无需手指，短应答）。成功后录入 ID 计数复位。 */
static int do_clear(void)
{
    printf("[FP] clear library\r\n");
    zw111_send_command(ZW111_CMD_EMPTY, NULL, 0);

    uint8_t  confirm, p[4];
    uint16_t plen = 0;
    if (zw111_read_response(&confirm, p, &plen, 2000) != 0) {
        return ZW111_ERR_TRANSPORT;
    }
    if (confirm != 0x00) {
        return (int)confirm;   /* 0x11 清空失败 */
    }
    s_next_id = 1;   /* 清空后录入 ID 计数复位 */
    return 0;
}

/* ======================================================================== */
/*  worker 任务                                                               */
/* ======================================================================== */

static QueueHandle_t  g_req_queue = NULL;
static zw111_done_cb_t g_done_cb   = NULL;
static volatile bool  g_busy       = false;

static void worker_task(void *pv)
{
    (void)pv;
    uint32_t op;
    while (1) {
        if (xQueueReceive(g_req_queue, &op, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* 名字库写回 flash：不占 busy、不触发 done 回调 */
        if (op == (uint32_t)ZW111_OP_FLUSH_NAMES) {
            fp_name_db_flush();
            continue;
        }

        g_busy = true;
        g_cancel_requested = false;   /* 清掉上次可能残留的取消标志 */
        zw111_result_t r;
        memset(&r, 0, sizeof(r));

        if (op == (uint32_t)ZW111_OP_ENROLL) {
            r.status = do_enroll(&r.page_id);
        } else if (op == (uint32_t)ZW111_OP_CLEAR) {
            r.status = do_clear();
        } else {
            r.status = do_identify(&r.page_id, &r.score);
        }

        g_busy = false;
        g_cancel_requested = false;

        if (r.status == ZW111_ERR_CANCELLED) {
            printf("[FP] op cancelled\r\n");   /* 取消不回调，UI 已自行收尾 */
        } else {
            printf("[FP] op=%s status=%d page=%u score=%u\r\n",
                   (op == (uint32_t)ZW111_OP_ENROLL) ? "enroll"
                 : (op == (uint32_t)ZW111_OP_CLEAR)  ? "clear"
                 :                                    "identify",
                   r.status, (unsigned)r.page_id, (unsigned)r.score);
            if (g_done_cb) {
                g_done_cb((zw111_op_t)op, &r);
            }
        }
    }
}

/* ======================================================================== */
/*  Public                                                                   */
/* ======================================================================== */

/* 读有效模板个数，恢复下一个录入 ID（断电后计数不丢） */
static void resume_next_id(void)
{
    zw111_send_command(ZW111_CMD_VALID_NUM, NULL, 0);

    uint8_t  confirm, p[4];
    uint16_t plen = 0;
    if (zw111_read_response(&confirm, p, &plen, ZW111_PROBE_TIMEOUT_MS) == 0 &&
        confirm == 0x00 && plen >= 2) {
        uint16_t valid = (uint16_t)((p[0] << 8) | p[1]);
        if (valid < ZW111_CAPACITY) {
            s_next_id = (uint16_t)(valid + 1);
        }
        printf("[FP] valid templates=%u, next id=%u\r\n", (unsigned)valid, (unsigned)s_next_id);
    } else {
        printf("[FP] read valid-num no response\r\n");
    }
}

void zw111_fingerprint_init(void)
{
    fsp_err_t err = g_uart6.p_api->open(&g_uart6_ctrl, &g_uart6_cfg);
    if (err != FSP_SUCCESS) {
        printf("[FP] UART6 open failed: %ld\r\n", (long)err);
        return;
    }
    printf("[FP] UART6 opened (57600 8N1)\r\n");

    g_req_queue = xQueueCreate(4, sizeof(uint32_t));
    if (g_req_queue == NULL) {
        printf("[FP] queue create failed\r\n");
        return;
    }
    if (xTaskCreate(worker_task, "zw111_fp", 1024, NULL, 2, NULL) != pdPASS) {
        printf("[FP] worker task create failed\r\n");
        return;
    }

    /* 等模块上电稳定，再握手探测 + 恢复录入 ID 计数 */
    vTaskDelay(pdMS_TO_TICKS(100));

    zw111_send_command(ZW111_CMD_HANDSHAKE, NULL, 0);
    uint8_t  confirm, p[4];
    uint16_t plen = 0;
    if (zw111_read_response(&confirm, p, &plen, ZW111_PROBE_TIMEOUT_MS) == 0 &&
        confirm == 0x00) {
        printf("[FP] handshake OK\r\n");
        resume_next_id();
    } else {
        printf("[FP] handshake no response (module absent?)\r\n");
    }
}

void zw111_fingerprint_set_done_cb(zw111_done_cb_t cb)
{
    g_done_cb = cb;
}

static bool zw111_request(zw111_op_t op)
{
    if (g_req_queue == NULL || g_busy) return false;
    uint32_t v = (uint32_t)op;
    return (xQueueSend(g_req_queue, &v, 0) == pdPASS);
}

bool zw111_fingerprint_enroll(void)
{
    return zw111_request(ZW111_OP_ENROLL);
}

bool zw111_fingerprint_identify(void)
{
    return zw111_request(ZW111_OP_IDENTIFY);
}

void zw111_fingerprint_cancel(void)
{
    if (g_busy) {
        g_cancel_requested = true;   /* worker 会在阻塞读里检测并发送 PS_Cancel */
    }
}

bool zw111_fingerprint_clear(void)
{
    return zw111_request(ZW111_OP_CLEAR);
}

bool zw111_fingerprint_flush_names(void)
{
    return zw111_request(ZW111_OP_FLUSH_NAMES);
}

bool zw111_fingerprint_is_busy(void)
{
    return g_busy;
}

const char *zw111_confirm_str(int confirm)
{
    switch (confirm) {
    case 0x00: return "OK";
    case 0x01: return "packet error";
    case 0x02: return "no finger";
    case 0x03: return "image capture failed";
    case 0x04: return "fingerprint too dry";
    case 0x05: return "fingerprint too wet";
    case 0x06: return "image too messy";
    case 0x07: return "few feature points";
    case 0x08: return "not matched";
    case 0x09: return "not found";
    case 0x0A: return "merge failed";
    case 0x11: return "clear failed";
    case 0x1F: return "library full";
    case 0x22: return "template not empty";
    case 0x23: return "template empty";
    case 0x24: return "library empty";
    case 0x26: return "timeout";
    case 0x27: return "already exists";
    case 0x29: return "sensor init failed";
    default:   return "error";
    }
}
