/**
 ******************************************************************************
 * @file    scs_servo.c
 * @brief   FEETECH STS 磁编码串行舵机驱动（FT-SCS 协议）— Renesas FSP 移植
 *
 * 从 FTServo_stm32HAL/SCSLib（SCS.c + SMS_STS.c）移植：
 *   - 去掉 STM32 HAL_UART_Transmit/Receive 依赖，改用 FSP g_uart4 (SCI4/UART4)。
 *   - RX：UART4_Callback (RXI 中断) → 环形缓冲 → 阻塞读（带超时）。
 *   - TX：静态缓冲 + g_uart4.p_api->write()（异步，靠"写后必读应答"串行化）。
 *
 * 本驱动只在单一任务（tracking 任务）里被调用，所有读写都是阻塞式，
 * 因此无需额外互斥锁；每次 write 之后紧接着阻塞 read 应答，保证上一帧
 * 字节已被 TXI 发完，静态发送缓冲不会被下一帧覆盖。
 ******************************************************************************
 */

#include "scs_servo.h"
#include "hal_data.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

/* ======================================================================== */
/*  UART 硬件层（SCI4/UART4）                                                 */
/* ======================================================================== */

#define SCS_RING_SIZE        256
#define SCS_MAX_FRAME        64      /* 单帧最大字节数（写位置 13B，读帧更小） */
#define SCS_READ_TIMEOUT_MS  10      /* 应答读超时（1Mbps 下整帧 < 1ms） */

static volatile uint8_t  g_rx_ring[SCS_RING_SIZE];
static volatile uint32_t g_rx_head = 0;    /* ISR 写 */
static volatile uint32_t g_rx_tail = 0;    /* 任务读 */

static uint8_t s_tx_buf[SCS_MAX_FRAME];

/* FSP 声明的 UART4 回调（ISR 上下文，极轻量：仅入环） */
void UART4_Callback(uart_callback_args_t *p_args)
{
    if (p_args->event != UART_EVENT_RX_CHAR) {
        return;
    }

    uint8_t  b    = (uint8_t)p_args->data;
    uint32_t next = (g_rx_head + 1) % SCS_RING_SIZE;
    if (next != g_rx_tail) {                 /* 未满才写入 */
        g_rx_ring[g_rx_head] = b;
        g_rx_head = next;
    }
    /* 溢出则丢弃该字节，靠 checksum 与超时丢弃当前应答帧 */
}

static bool ring_pop(uint8_t *out)
{
    if (g_rx_head == g_rx_tail) return false;
    *out = g_rx_ring[g_rx_tail];
    g_rx_tail = (g_rx_tail + 1) % SCS_RING_SIZE;
    return true;
}

/* 清空接收缓冲（发送前调用，避免上一次超时残留的脏字节被误读） */
static void ring_flush(void)
{
    taskENTER_CRITICAL();
    g_rx_head = g_rx_tail;
    taskEXIT_CRITICAL();
}

/* 阻塞读 nLen 字节，超时返回 false（已读部分丢弃） */
static bool scs_rx(uint8_t *buf, int nlen)
{
    TickType_t start = xTaskGetTickCount();
    int got = 0;
    while (got < nlen) {
        while (got < nlen && ring_pop(&buf[got])) {
            got++;
        }
        if (got >= nlen) return true;
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(SCS_READ_TIMEOUT_MS)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}

/* 阻塞发一帧（静态缓冲 + 异步 write，串行化保证安全） */
static void scs_tx(const uint8_t *buf, uint32_t len)
{
    memcpy(s_tx_buf, buf, len);
    g_uart4.p_api->write(&g_uart4_ctrl, s_tx_buf, len);
    /* write 异步：字节由 TXI 稍后发出。后续必然紧跟阻塞读，串行化保证安全。 */
}

/* ======================================================================== */
/*  FT-SCS 协议层（移植自 SCS.c）                                             */
/* ======================================================================== */

enum {
    INST_PING       = 0x01,
    INST_READ       = 0x02,
    INST_WRITE      = 0x03,
};

/* 1 个 16 位 → 2 个 8 位（小端：低字节在前） */
static void host_to_scs(uint8_t *lo, uint8_t *hi, int val)
{
    *lo = (uint8_t)(val & 0xFF);
    *hi = (uint8_t)((val >> 8) & 0xFF);
}

/* 2 个 8 位（小端）→ 1 个 16 位 */
static int scs_to_host(uint8_t lo, uint8_t hi)
{
    return (int)((uint16_t)lo | ((uint16_t)hi << 8));
}

/* 组帧并发送：FF FF ID LEN CMD [MEMADDR] [数据…] CHECKSUM */
static void scs_write_buf(uint8_t id, uint8_t mem_addr, const uint8_t *dat,
                          uint8_t nlen, uint8_t cmd)
{
    uint8_t frame[SCS_MAX_FRAME];
    uint8_t n = 0;
    uint8_t len;
    uint8_t sum;

    frame[n++] = 0xFF;
    frame[n++] = 0xFF;
    frame[n++] = id;
    frame[n++] = 0;                 /* LEN 占位，下面回填 */
    frame[n++] = cmd;

    if (dat) {
        len        = (uint8_t)(nlen + 3);   /* LEN = 参数数 + 2，参数数 = 1(地址)+nlen */
        frame[3]   = len;
        frame[n++] = mem_addr;
    } else {
        len      = 2;
        frame[3] = len;
        /* 无数据命令不带 MEMADDR 字节（PING/RESET 等） */
    }

    sum = (uint8_t)(id + len + cmd + mem_addr);
    if (dat) {
        for (uint8_t i = 0; i < nlen; i++) {
            sum = (uint8_t)(sum + dat[i]);
            frame[n++] = dat[i];
        }
    }
    frame[n++] = (uint8_t)(~sum);

    scs_tx(frame, n);
}

/* 等待写指令应答帧（ACK）：FF FF ID 02 STATUS CHECKSUM（6 字节）。 */
static bool scs_ack(uint8_t id)
{
    uint8_t buf[4];

    if (!scs_rx(buf, 4)) return false;                       /* FF FF ID LEN */
    if (buf[0] != 0xFF || buf[1] != 0xFF) return false;
    if (buf[2] != id)          return false;                  /* ID  */
    if (buf[3] != 2)           return false;                  /* LEN */

    /* STATUS + CHECKSUM（共 2 字节） */
    if (!scs_rx(buf, 2)) return false;
    /* buf[0]=STATUS, buf[1]=CHECKSUM */
    if ((uint8_t)(~(id + 2 + buf[0])) != buf[1]) return false;
    return true;
}

/* 普通写指令，等待应答。 */
static bool scs_gen_write(uint8_t id, uint8_t mem_addr, const uint8_t *dat,
                          uint8_t nlen)
{
    ring_flush();
    scs_write_buf(id, mem_addr, dat, nlen, INST_WRITE);
    return scs_ack(id);
}

/* 读数据指令（INST_READ），返回读取到的字节数，失败返回 0 */
static int scs_read(uint8_t id, uint8_t mem_addr, uint8_t *out, uint8_t nlen)
{
    uint8_t buf[4];
    uint8_t status, cal;
    uint8_t sum;
    int     size;

    ring_flush();
    scs_write_buf(id, mem_addr, &nlen, 1, INST_READ);

    /* 应答帧头：FF FF ID LEN（4 字节），随后 STATUS + 数据 + CHECKSUM */
    if (!scs_rx(buf, 4)) return 0;
    if (buf[0] != 0xFF || buf[1] != 0xFF) return 0;
    if (buf[2] != id)                       return 0;
    if (buf[3] != (uint8_t)(nlen + 2))      return 0;

    if (!scs_rx(&status, 1)) return 0;
    size = scs_rx(out, nlen) ? nlen : 0;
    if (!size) return 0;
    if (!scs_rx(&cal, 1)) return 0;

    sum = (uint8_t)(id + (uint8_t)(nlen + 2) + status);
    for (int i = 0; i < size; i++) sum = (uint8_t)(sum + out[i]);
    if ((uint8_t)(~sum) != cal) return 0;
    return size;
}

/* 写 1 字节，等待 ACK。返回是否成功。 */
static bool scs_write_byte(uint8_t id, uint8_t mem_addr, uint8_t val)
{
    return scs_gen_write(id, mem_addr, &val, 1);
}

/* 读 1 字节，失败返回 -1 */
static int scs_read_byte(uint8_t id, uint8_t mem_addr)
{
    uint8_t b;
    if (scs_read(id, mem_addr, &b, 1) != 1) return -1;
    return b;
}

/* 读 2 字节（小端），失败返回 -1 */
static int scs_read_word(uint8_t id, uint8_t mem_addr)
{
    uint8_t b[2];
    if (scs_read(id, mem_addr, b, 2) != 2) return -1;
    return scs_to_host(b[0], b[1]);
}

/* ======================================================================== */
/*  Public API                                                               */
/* ======================================================================== */

bool scs_servo_init(void)
{
    /* UART4 已在 FSP 配置 p_callback = UART4_Callback（RXI 中断投递 RX 字节）。 */
    fsp_err_t err = g_uart4.p_api->open(&g_uart4_ctrl, &g_uart4_cfg);
    if (err != FSP_SUCCESS) {
        printf("[SERVO] UART4 open failed: %ld\r\n", (long)err);
        return false;
    }
    printf("[SERVO] UART4 opened (STS servo, ID=%d)\r\n", (int)SCS_SERVO_ID);

    /* 等舵机上电稳定：实测首个命令（读位置）容易无应答，稍等再发首个写命令 */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 使能扭矩。写命令是 fire-and-forget（舵机收到即执行，即使无 ACK），
     * 且 STS 首个命令常无应答 —— 因此扭矩使能的 ACK 超时**不能**判定初始化失败：
     * 一旦 return false，g_servo_ready 被置 false，整个追踪回路就不发写位置命令，
     * 表现为「扭矩存在但舵机不会转动」。追踪路径只发写命令，不依赖 ACK/读回。 */
    if (!scs_write_byte(SCS_SERVO_ID, SCS_STS_ADDR_TORQUE_ENABLE, 1)) {
        printf("[SERVO] torque enable no ack (write still sent, continue)\r\n");
        /* 不 return false —— 继续初始化，保证追踪回路能正常驱动 */
    }

    /* 回中位（0°），慢速 + 适中加速度 */
    scs_servo_write_pos(SCS_STS_POS_CENTER, 60, 30);
    printf("[SERVO] init done, centered\r\n");
    return true;
}

bool scs_servo_write_pos(uint16_t pos, uint16_t speed, uint8_t acc)
{
    /* 写位置 = 加速度(1) + 目标位置(2) + 预留时间(2) + 速度(2) 共 7 字节，
     * 起始地址 SCS_STS_ADDR_ACC(41)。BIT15 方向位：正负号由调用方处理，
     * 这里只接受 0..4095 无符号位置。 */
    uint8_t b[7];
    b[0] = acc;
    host_to_scs(&b[1], &b[2], (int)pos);
    host_to_scs(&b[3], &b[4], 0);          /* 预留 */
    host_to_scs(&b[5], &b[6], (int)speed);

    return scs_gen_write(SCS_SERVO_ID, SCS_STS_ADDR_ACC, b, 7);
}

int scs_servo_read_pos(void)
{
    return scs_read_word(SCS_SERVO_ID, SCS_STS_ADDR_PRESENT_POS_L);
}

bool scs_servo_torque(bool on)
{
    return scs_write_byte(SCS_SERVO_ID, SCS_STS_ADDR_TORQUE_ENABLE, on ? 1 : 0);
}

int scs_servo_is_moving(void)
{
    return scs_read_byte(SCS_SERVO_ID, SCS_STS_ADDR_MOVING);
}

/* ======================================================================== */
/*  测试：舵机左右 ±30° 摆动                                                   */
/* ======================================================================== */

void scs_servo_test_run(void)
{
    printf("\r\n[SERVO TEST] ===== start =====\r\n");

    /* 1. 打开 UART4 */
    fsp_err_t err = g_uart4.p_api->open(&g_uart4_ctrl, &g_uart4_cfg);
    if (err != FSP_SUCCESS) {
        printf("[SERVO TEST] UART4 open FAILED: %ld\r\n", (long)err);
        return;
    }
    printf("[SERVO TEST] UART4 open OK\r\n");

    /* 2. 等舵机上电稳定（不读位置，只用写命令） */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* 3. 使能扭矩。写命令即使 ACK 超时，舵机也已经执行，故不据此退出 */
    if (scs_servo_torque(true)) {
        printf("[SERVO TEST] torque enabled\r\n");
    } else {
        printf("[SERVO TEST] torque enable no ACK (write still sent, continue)\r\n");
    }

    /* 4. 左右 ±30° 摆动 3 个来回（只写位置，不读回） */
    for (int c = 0; c < 3; c++) {
        printf("[SERVO TEST] -> +30deg (2393)\r\n");
        scs_servo_write_pos((uint16_t)(SCS_STS_POS_CENTER + 345), 2400, 50);
        vTaskDelay(pdMS_TO_TICKS(1500));

        printf("[SERVO TEST] -> -30deg (1703)\r\n");
        scs_servo_write_pos((uint16_t)(SCS_STS_POS_CENTER - 345), 2400, 50);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    /* 5. 回中位 */
    scs_servo_write_pos(SCS_STS_POS_CENTER, 2400, 50);
    vTaskDelay(pdMS_TO_TICKS(1000));

    printf("[SERVO TEST] ===== done (should have swept +/-30 deg) =====\r\n");
}
