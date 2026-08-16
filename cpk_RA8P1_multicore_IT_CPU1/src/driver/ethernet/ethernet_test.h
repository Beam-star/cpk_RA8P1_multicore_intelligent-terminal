/*
 * ethernet_test.h - Ethernet test functions
 *   Ping + Speed test + TCP echo server + UDP echo server + UDP screen receiver
 */

#ifndef ETHERNET_TEST_H_
#define ETHERNET_TEST_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Ping test configuration ---- */
#define ETH_TEST_PING_TARGET    "192.168.1.10"   /* IP to ping (PC) */
#define ETH_TEST_PING_COUNT     10               /* Number of pings */
#define ETH_TEST_PING_INTERVAL  2000             /* Interval between pings (ms) */

/* ---- TCP echo server configuration ---- */
#define ETH_TEST_TCP_PORT       5000             /* TCP echo server listen port */

/* ---- Speed test configuration ---- */
#define ETH_TEST_SPEED_SERVER   "192.168.1.10"   /* PC IP running recv script */
#define ETH_TEST_SPEED_PORT     5001             /* PC recv port */
#define ETH_TEST_SPEED_SIZE     (64 * 1024)      /* Total bytes to send (64KB) */

/* ---- UDP echo server configuration ---- */
#define ETH_TEST_UDP_PORT       5000             /* UDP echo server listen port (TCP/UDP 不冲突) */

/* ---- UDP screen receiver configuration ----
 * 协议与 udp_screen_sender_1024x600.py 匹配
 *
 * 包头 (12 字节, 大端):
 *   uint16  magic         = 0x5556
 *   uint32  frame_id      帧序号
 *   uint16  packet_id     包序号 (0-based)
 *   uint16  packet_count  本帧总包数
 *   uint16  payload_len   有效载荷长度
 * 有效载荷: ETH_UDP_SCREEN_PAYLOAD 字节 RGB565 大端像素数据
 */

/* UDP 图传功能开关: 0=禁用 (RGBLCD 已迁移到 CPU0), 1=启用 */
#define ETH_UDP_SCREEN_ENABLE   0

#define ETH_UDP_SCREEN_PORT     5001             /* UDP 图传监听端口 */
#define ETH_UDP_SCREEN_MAGIC    0x5556           /* 帧魔数标识 */
#define ETH_UDP_SCREEN_WIDTH    1024             /* 帧宽度 = LCD 宽度 */
#define ETH_UDP_SCREEN_HEIGHT   600              /* 帧高度 = LCD 高度 */
#define ETH_UDP_SCREEN_BPP      2                /* 每像素字节数 (RGB565) */
#define ETH_UDP_SCREEN_PAYLOAD  1024             /* 每包有效载荷（字节） */

/* 每帧总包数 = (W * H * BPP) / PAYLOAD = 1024*600*2/1024 = 1200 */
#define ETH_UDP_SCREEN_PKTS     ((ETH_UDP_SCREEN_WIDTH * ETH_UDP_SCREEN_HEIGHT * ETH_UDP_SCREEN_BPP) / ETH_UDP_SCREEN_PAYLOAD)

/* 帧大小（字节）= 1024*600*2 = 1,228,800 */
#define ETH_UDP_SCREEN_FB_SIZE  (ETH_UDP_SCREEN_WIDTH * ETH_UDP_SCREEN_HEIGHT * ETH_UDP_SCREEN_BPP)

/**
 * @brief Run ping test. Pings ETH_TEST_PING_TARGET multiple times.
 *        Should be called BEFORE starting TCP echo server to avoid
 *        buffer contention.
 */
void ethernet_test_ping(void);

/**
 * @brief TCP throughput speed test.
 *
 * Device connects to PC and sends ETH_TEST_SPEED_SIZE bytes as fast as
 * possible. Reports elapsed time and throughput.
 *
 * PC side must run a receiver script first:
 *   python ethernet_speed_recv.py <port>
 *
 * @return 0 on success, negative on failure.
 */
int ethernet_test_speed(void);

/**
 * @brief Start TCP echo server as a FreeRTOS task.
 *
 * Listens on ETH_TEST_TCP_PORT, echoes received data back.
 * Runs indefinitely. Should be started AFTER ping/speed tests.
 *
 * @param priority   FreeRTOS task priority.
 * @param stack_size Task stack size in words.
 * @return 0 on success, negative on failure.
 */
int ethernet_test_tcp_echo_start(int priority, int stack_size);

/**
 * @brief Start UDP echo server as a FreeRTOS task.
 *
 * 监听 ETH_TEST_UDP_PORT 端口，将收到的 UDP 数据原样回传。
 * 用于验证 UDP 连通性。
 *
 * @param priority   FreeRTOS task priority.
 * @param stack_size Task stack size in words.
 * @return 0 on success, negative on failure.
 */
int ethernet_test_udp_echo_start(int priority, int stack_size);

/**
 * @brief Start UDP screen receiver as a FreeRTOS task.
 *
 * 监听 ETH_UDP_SCREEN_PORT 端口，接收 PC 发来的 1024x600 RGB565 图传数据。
 * 帧数据重组后直接写入 LCD 的 SDRAM framebuffer (fb_background[0])，
 * 无需额外缓冲区，零拷贝显示。
 *
 * PC 端运行:
 *   python udp_screen_sender_1024x600.py --target-ip <board_ip> --port 5001
 *
 * @param priority   FreeRTOS task priority.
 * @param stack_size Task stack size in words (建议 >= 2048).
 * @return 0 on success, negative on failure.
 */
#if ETH_UDP_SCREEN_ENABLE
int ethernet_test_udp_screen_start(int priority, int stack_size);
#endif

/**
 * @brief Run all ethernet tests.
 *
 * Execution order:
 *   1. Print network info
 *   2. Run speed test (needs PC receiver running)
 *   3. Run ping test
 *   4. Start TCP echo server (runs in background)
 *   5. Start UDP echo server (runs in background)
 *   6. Start UDP screen receiver (runs in background)
 */
void ethernet_test_run(void);

#ifdef __cplusplus
}
#endif

#endif /* ETHERNET_TEST_H_ */
