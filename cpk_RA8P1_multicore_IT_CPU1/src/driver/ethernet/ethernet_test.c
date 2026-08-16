/*
 * ethernet_test.c - Ethernet test: ping + speed + TCP echo + UDP echo + UDP screen receiver
 *
 * - Ping test: ICMP echo, runs before TCP echo to avoid buffer contention.
 * - Speed test: TCP send throughput measurement (device -> PC).
 * - TCP echo server: listens on port, echoes received data back.
 * - UDP echo server: listens on port, echoes received UDP datagrams back.
 * - UDP screen receiver: receives 1024x600 RGB565 frames via UDP,
 *   writes directly to LCD framebuffer (fb_background[0] in SDRAM).
 */

#include "ethernet_test.h"
#include "ethernet.h"
#include "common_data.h"       /* fb_background[], g_display0_ctrl */
#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"
#include "rpmsg_log.h"
#include "bsp_api.h"
//#include "r_glcdc.h"

#include <string.h>
#include <stdio.h>

/* ---- Ping test ---- */

void ethernet_test_ping(void)
{
    uint32_t target_addr;
    BaseType_t ret;
    int sent = 0, fail = 0;

    target_addr = FreeRTOS_inet_addr(ETH_TEST_PING_TARGET);
    if (target_addr == 0) {
        rpmsg_log_cpu1_printf("[PING] Invalid target: %s\r\n", ETH_TEST_PING_TARGET);
        return;
    }

    char ip_str[16];
    ethernet_get_ip(ip_str, sizeof(ip_str));
    rpmsg_log_cpu1_printf("[PING] From %s -> %s  count=%d\r\n",
                          ip_str, ETH_TEST_PING_TARGET, ETH_TEST_PING_COUNT);

    for (int i = 0; i < ETH_TEST_PING_COUNT; i++) {
        ret = FreeRTOS_SendPingRequest(target_addr, 8, pdMS_TO_TICKS(2000));
        if (ret != 0) {
            sent++;
            rpmsg_log_cpu1_printf("[PING] %d: OK (id=%ld)\r\n", i + 1, (long)ret);
        } else {
            fail++;
            rpmsg_log_cpu1_printf("[PING] %d: FAILED (no buffer)\r\n", i + 1);
        }
        vTaskDelay(pdMS_TO_TICKS(ETH_TEST_PING_INTERVAL));
    }

    rpmsg_log_cpu1_printf("[PING] Result: sent=%d, fail=%d\r\n", sent, fail);
}

/* ---- Speed test ---- */

int ethernet_test_speed(void)
{
    Socket_t sock;
    struct freertos_sockaddr addr;
    uint8_t *send_buf;
    uint32_t total_sent = 0;
    uint32_t start_tick, end_tick, elapsed_ms;
    float throughput_kbps;
    int connect_retry;
    #define SPEED_CONNECT_RETRIES  3

    rpmsg_log_cpu1_printf("[SPEED] Target: %s:%d\r\n",
                          ETH_TEST_SPEED_SERVER, ETH_TEST_SPEED_PORT);

    /* Set connect address */
    memset(&addr, 0, sizeof(addr));
    addr.sin_port = FreeRTOS_htons(ETH_TEST_SPEED_PORT);
    addr.sin_addr = FreeRTOS_inet_addr(ETH_TEST_SPEED_SERVER);

    /* Retry connect loop */
    for (connect_retry = 1; connect_retry <= SPEED_CONNECT_RETRIES; connect_retry++) {
        rpmsg_log_cpu1_printf("[SPEED] Connect attempt %d/%d ...\r\n",
                              connect_retry, SPEED_CONNECT_RETRIES);

        sock = FreeRTOS_socket(FREERTOS_AF_INET, FREERTOS_SOCK_STREAM, FREERTOS_IPPROTO_TCP);
        if (sock == FREERTOS_INVALID_SOCKET) {
            rpmsg_log_cpu1_printf("[SPEED] Socket create failed\r\n");
            return -1;
        }

        if (FreeRTOS_connect(sock, &addr, sizeof(addr)) == 0) {
            break;  /* success */
        }

        rpmsg_log_cpu1_printf("[SPEED] Connect failed, retrying in 2s...\r\n");
        FreeRTOS_closesocket(sock);
        sock = FREERTOS_INVALID_SOCKET;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (sock == FREERTOS_INVALID_SOCKET) {
        rpmsg_log_cpu1_printf("[SPEED] All %d connect attempts failed\r\n", SPEED_CONNECT_RETRIES);
        rpmsg_log_cpu1_printf("[SPEED] Check: 1) PC script running? 2) Firewall allow port %d?\r\n",
                              ETH_TEST_SPEED_PORT);
        return -1;
    }

    rpmsg_log_cpu1_printf("[SPEED] Connected! Sending %d bytes ...\r\n", ETH_TEST_SPEED_SIZE);

    /* Set send timeout */
    TickType_t send_timeout = pdMS_TO_TICKS(5000);
    FreeRTOS_setsockopt(sock, 0, FREERTOS_SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

    /* Allocate send buffer (use a reasonable chunk size) */
    #define SPEED_CHUNK_SIZE  1460  /* TCP MSS-sized chunk */
    send_buf = (uint8_t *)pvPortMalloc(SPEED_CHUNK_SIZE);
    if (send_buf == NULL) {
        rpmsg_log_cpu1_printf("[SPEED] Buffer alloc failed\r\n");
        FreeRTOS_closesocket(sock);
        return -1;
    }

    /* Fill with test pattern */
    for (int i = 0; i < SPEED_CHUNK_SIZE; i++) {
        send_buf[i] = (uint8_t)(i & 0xFF);
    }

    /* Send data and measure time */
    start_tick = xTaskGetTickCount();

    while (total_sent < ETH_TEST_SPEED_SIZE) {
        uint32_t to_send = ETH_TEST_SPEED_SIZE - total_sent;
        if (to_send > SPEED_CHUNK_SIZE) {
            to_send = SPEED_CHUNK_SIZE;
        }

        int32_t ret = FreeRTOS_send(sock, send_buf, to_send, 0);
        if (ret > 0) {
            total_sent += (uint32_t)ret;
        } else {
            rpmsg_log_cpu1_printf("[SPEED] Send error at %lu bytes\r\n", (unsigned long)total_sent);
            break;
        }
    }

    end_tick = xTaskGetTickCount();
    elapsed_ms = (uint32_t)((end_tick - start_tick) * portTICK_PERIOD_MS);

    vPortFree(send_buf);
    FreeRTOS_shutdown(sock, FREERTOS_SHUT_RDWR);
    vTaskDelay(pdMS_TO_TICKS(500));
    FreeRTOS_closesocket(sock);

    /* Calculate throughput */
    if (elapsed_ms > 0 && total_sent > 0) {
        throughput_kbps = (float)total_sent * 8.0f / (float)elapsed_ms;  /* kbit/s */
        rpmsg_log_cpu1_printf("[SPEED] Result: %lu bytes in %lu ms\r\n",
                              (unsigned long)total_sent, (unsigned long)elapsed_ms);
        rpmsg_log_cpu1_printf("[SPEED] Throughput: %.1f kbit/s (%.2f Mbit/s)\r\n",
                              throughput_kbps, throughput_kbps / 1000.0f);
    } else {
        rpmsg_log_cpu1_printf("[SPEED] No data sent\r\n");
    }

    return 0;
}

/* ---- TCP Echo Server ---- */

static void tcp_echo_task(void *pvParameters)
{
    (void) pvParameters;
    Socket_t xListeningSocket, xConnectedSocket;
    struct freertos_sockaddr xBindAddress, xClientAddress;
    socklen_t xClientAddressLength;
    TickType_t xReceiveTimeout = pdMS_TO_TICKS(5000);
    char rx_buf[256];
    int32_t bytes_received;

    xListeningSocket = FreeRTOS_socket(FREERTOS_AF_INET,
                                       FREERTOS_SOCK_STREAM,
                                       FREERTOS_IPPROTO_TCP);
    if (xListeningSocket == FREERTOS_INVALID_SOCKET) {
        rpmsg_log_cpu1_printf("[TCP] Failed to create socket\r\n");
        vTaskDelete(NULL);
        return;
    }

    memset(&xBindAddress, 0, sizeof(xBindAddress));
    xBindAddress.sin_port = FreeRTOS_htons(ETH_TEST_TCP_PORT);
    xBindAddress.sin_addr = 0;

    if (FreeRTOS_bind(xListeningSocket, &xBindAddress, sizeof(xBindAddress)) != 0) {
        rpmsg_log_cpu1_printf("[TCP] Failed to bind port %d\r\n", ETH_TEST_TCP_PORT);
        FreeRTOS_closesocket(xListeningSocket);
        vTaskDelete(NULL);
        return;
    }

    if (FreeRTOS_listen(xListeningSocket, 1) != 0) {
        rpmsg_log_cpu1_printf("[TCP] Failed to listen\r\n");
        FreeRTOS_closesocket(xListeningSocket);
        vTaskDelete(NULL);
        return;
    }

    rpmsg_log_cpu1_printf("[TCP] Echo server listening on port %d\r\n", ETH_TEST_TCP_PORT);

    while (1) {
        rpmsg_log_cpu1_printf("[TCP] Waiting for connection...\r\n");

        xClientAddressLength = sizeof(xClientAddress);
        xConnectedSocket = FreeRTOS_accept(xListeningSocket,
                                           &xClientAddress,
                                           &xClientAddressLength);
        if (xConnectedSocket == FREERTOS_INVALID_SOCKET) {
            continue;
        }

        FreeRTOS_setsockopt(xConnectedSocket, 0,
                            FREERTOS_SO_RCVTIMEO, &xReceiveTimeout,
                            sizeof(xReceiveTimeout));

        uint32_t client_ip = xClientAddress.sin_addr;
        rpmsg_log_cpu1_printf("[TCP] Client connected: %d.%d.%d.%d:%d\r\n",
                              (int)(client_ip & 0xFF),
                              (int)((client_ip >> 8) & 0xFF),
                              (int)((client_ip >> 16) & 0xFF),
                              (int)((client_ip >> 24) & 0xFF),
                              (int)FreeRTOS_ntohs(xClientAddress.sin_port));

        while (1) {
            bytes_received = FreeRTOS_recv(xConnectedSocket, rx_buf,
                                           sizeof(rx_buf) - 1, 0);
            if (bytes_received > 0) {
                int32_t sent = FreeRTOS_send(xConnectedSocket, rx_buf,
                                             bytes_received, 0);
                if (sent < 0) {
                    break;
                }
            } else {
                break;
            }
        }

        rpmsg_log_cpu1_printf("[TCP] Client disconnected\r\n");
        FreeRTOS_closesocket(xConnectedSocket);
    }
}

int ethernet_test_tcp_echo_start(int priority, int stack_size)
{
    BaseType_t ret = xTaskCreate(tcp_echo_task,
                                 "TCP_Echo",
                                 (configSTACK_DEPTH_TYPE)stack_size,
                                 NULL,
                                 (UBaseType_t)priority,
                                 NULL);
    if (ret != pdPASS) {
        rpmsg_log_cpu1_printf("[TCP] Failed to create echo task\r\n");
        return -1;
    }
    return 0;
}

/* ---- UDP Echo Server ---- */

static void udp_echo_task(void *pvParameters)
{
    (void) pvParameters;
    Socket_t sock;
    uint8_t rx_buf[512];
    struct freertos_sockaddr sender_addr;
    socklen_t sender_len;
    int32_t received;
    uint32_t pkt_count = 0;

    sock = FreeRTOS_socket(FREERTOS_AF_INET, FREERTOS_SOCK_DGRAM,
                           FREERTOS_IPPROTO_UDP);
    if (sock == FREERTOS_INVALID_SOCKET) {
        rpmsg_log_cpu1_printf("[UDP] Echo: socket create failed\r\n");
        vTaskDelete(NULL);
        return;
    }

    struct freertos_sockaddr bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_port = FreeRTOS_htons(ETH_TEST_UDP_PORT);
    bind_addr.sin_addr = 0;

    if (FreeRTOS_bind(sock, &bind_addr, sizeof(bind_addr)) != 0) {
        rpmsg_log_cpu1_printf("[UDP] Echo: bind port %d failed\r\n", ETH_TEST_UDP_PORT);
        FreeRTOS_closesocket(sock);
        vTaskDelete(NULL);
        return;
    }

    /* 设置接收超时，避免永久阻塞 */
    TickType_t recv_timeout = pdMS_TO_TICKS(1000);
    FreeRTOS_setsockopt(sock, 0, FREERTOS_SO_RCVTIMEO,
                        &recv_timeout, sizeof(recv_timeout));

    rpmsg_log_cpu1_printf("[UDP] Echo server listening on port %d\r\n", ETH_TEST_UDP_PORT);

    while (1) {
        sender_len = sizeof(sender_addr);
        received = FreeRTOS_recvfrom(sock, rx_buf, sizeof(rx_buf), 0,
                                     &sender_addr, &sender_len);
        if (received > 0) {
            pkt_count++;
            /* 回传数据 */
            FreeRTOS_sendto(sock, rx_buf, received, 0,
                            &sender_addr, sender_len);

            /* 打印前几个包的信息 */
            if (pkt_count <= 5 || (pkt_count % 100) == 0) {
                uint32_t ip = sender_addr.sin_addr;
                rpmsg_log_cpu1_printf("[UDP] Echo #%lu: %ld bytes from %d.%d.%d.%d:%d\r\n",
                                      (unsigned long)pkt_count, (long)received,
                                      (int)(ip & 0xFF), (int)((ip >> 8) & 0xFF),
                                      (int)((ip >> 16) & 0xFF), (int)((ip >> 24) & 0xFF),
                                      (int)FreeRTOS_ntohs(sender_addr.sin_port));
            }
        }
        /* received <= 0: timeout or error, continue loop */
    }
}

int ethernet_test_udp_echo_start(int priority, int stack_size)
{
    BaseType_t ret = xTaskCreate(udp_echo_task,
                                 "UDP_Echo",
                                 (configSTACK_DEPTH_TYPE)stack_size,
                                 NULL,
                                 (UBaseType_t)priority,
                                 NULL);
    if (ret != pdPASS) {
        rpmsg_log_cpu1_printf("[UDP] Echo: task create failed\r\n");
        return -1;
    }
    return 0;
}

/* ---- UDP Screen Receiver ----
 *
 * 接收 PC 发来的 1024x600 RGB565 图传数据，直接写入 LCD framebuffer。
 * 零拷贝：不使用额外缓冲区，UDP 数据经格式转换后直接写 fb_background[0]。
 *
 * 协议 (与 udp_screen_sender_1024x600.py 匹配):
 *   包头 12 字节 (大端): magic(0x5556) + frame_id + packet_id + packet_count + payload_len
 *   有效载荷: 1024 字节 RGB565 大端像素数据
 *   每帧 = 1024*600*2/1024 = 1200 包
 *
 * 像素格式转换:
 *   PC 发送: RGB565 大端 (R[15:11] G[10:5] B[4:0], 高字节在前)
 *   LCD 需要: BGR565 小端 (GLCDC byte-lane swap 导致 R/B 互换)
 *   转换: 字节交换 + R/B 通道交换，单步完成
 */

#if ETH_UDP_SCREEN_ENABLE

/* 包头结构 (网络字节序 / 大端) */
#pragma pack(push, 1)
typedef struct {
    uint16_t magic;         /* 魔数 0x5556 */
    uint32_t frame_id;      /* 帧序号 */
    uint16_t packet_id;     /* 包序号 (0-based) */
    uint16_t packet_count;  /* 本帧总包数 */
    uint16_t payload_len;   /* 本包有效载荷长度 */
} udp_screen_header_t;
#pragma pack(pop)

#define UDP_SCREEN_HEADER_SIZE  sizeof(udp_screen_header_t)  /* 12 bytes */

/*
 * GLCDC 原生双缓冲:
 * fb_background[0] 和 fb_background[1] 由 e2studio FSP 配置生成 (各 1.2MB)。
 * CPU 写入后台缓冲区，帧完成后调用 R_GLCDC_BufferChange() 切换显示缓冲区，
 * GLCDC 在下一个 vsync 自动切换，无撕裂。
 */

static void udp_screen_task(void *pvParameters)
{
    (void) pvParameters;
    Socket_t sock;
    uint8_t rx_buf[UDP_SCREEN_HEADER_SIZE + ETH_UDP_SCREEN_PAYLOAD + 16];
    struct freertos_sockaddr sender_addr;
    socklen_t sender_len;
    int32_t received;

    /* 双缓冲索引: 0 或 1, 交替使用 */
    uint8_t write_idx  = 1;               /* 当前写入的缓冲区 (初始为 1, 因为 GLCDC 初始显示 0) */
    uint8_t *fb_write  = fb_background[1]; /* CPU 写入的后台缓冲区 */

    /* 帧统计 */
    uint32_t cur_frame_id   = 0xFFFFFFFF;
    uint16_t pkts_received  = 0;
    uint32_t total_frames   = 0;
    uint32_t total_dropped  = 0;
    uint32_t total_errors   = 0;
    uint32_t total_rx       = 0;           /* 总接收包数（含错误包） */
    uint32_t timeout_cnt    = 0;           /* 连续超时计数 */
    bool     first_pkt_logged = false;     /* 首包是否已打印 */

    /* 创建 UDP socket */
    sock = FreeRTOS_socket(FREERTOS_AF_INET, FREERTOS_SOCK_DGRAM,
                           FREERTOS_IPPROTO_UDP);
    if (sock == FREERTOS_INVALID_SOCKET) {
        rpmsg_log_cpu1_printf("[SCR] Socket create failed\r\n");
        vTaskDelete(NULL);
        return;
    }

    /* 绑定端口 */
    struct freertos_sockaddr bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_port = FreeRTOS_htons(ETH_UDP_SCREEN_PORT);
    bind_addr.sin_addr = 0;

    if (FreeRTOS_bind(sock, &bind_addr, sizeof(bind_addr)) != 0) {
        rpmsg_log_cpu1_printf("[SCR] Bind port %d failed\r\n", ETH_UDP_SCREEN_PORT);
        FreeRTOS_closesocket(sock);
        vTaskDelete(NULL);
        return;
    }

    /* 接收超时 500ms，用于检测帧间隔 */
    TickType_t recv_timeout = pdMS_TO_TICKS(500);
    FreeRTOS_setsockopt(sock, 0, FREERTOS_SO_RCVTIMEO,
                        &recv_timeout, sizeof(recv_timeout));

    rpmsg_log_cpu1_printf("[SCR] Screen receiver listening on port %d\r\n",
                          ETH_UDP_SCREEN_PORT);
    rpmsg_log_cpu1_printf("[SCR] Target LCD: %dx%d RGB565, framebuffer @ SDRAM\r\n",
                          ETH_UDP_SCREEN_WIDTH, ETH_UDP_SCREEN_HEIGHT);
    rpmsg_log_cpu1_printf("[SCR] Frame: %lu bytes, %d packets/frame\r\n",
                          (unsigned long)ETH_UDP_SCREEN_FB_SIZE,
                          ETH_UDP_SCREEN_PKTS);

    rpmsg_log_cpu1_printf("[SCR] GLCDC double buffer: fb[0]=%p fb[1]=%p\r\n",
                          fb_background[0], fb_background[1]);

    while (1) {
        sender_len = sizeof(sender_addr);
        received = FreeRTOS_recvfrom(sock, rx_buf, sizeof(rx_buf), 0,
                                     &sender_addr, &sender_len);

        if (received <= 0) {
            timeout_cnt++;
            /* 超时: 如果正在收帧则视为帧结束 */
            if (pkts_received > 0 && pkts_received < ETH_UDP_SCREEN_PKTS) {
                total_dropped++;
                rpmsg_log_cpu1_printf("[SCR] Frame %lu incomplete: %u/%u pkts, DROPPED (timeout #%lu)\r\n",
                                      (unsigned long)cur_frame_id,
                                      pkts_received, ETH_UDP_SCREEN_PKTS,
                                      (unsigned long)timeout_cnt);
                pkts_received = 0;
                cur_frame_id = 0xFFFFFFFF;
            }
            /* 每 10 次超时打印一次状态（约 5 秒） */
            if ((timeout_cnt % 10) == 0) {
                rpmsg_log_cpu1_printf("[SCR] timeout #%lu, total_rx=%lu\r\n",
                                      (unsigned long)timeout_cnt,
                                      (unsigned long)total_rx);
            }
            continue;
        }

        /* 首包诊断: 打印发送方 IP 和收到的字节数 */
        if (!first_pkt_logged) {
            first_pkt_logged = true;
            uint32_t sip = sender_addr.sin_addr;
            rpmsg_log_cpu1_printf("[SCR] *** First packet: %ld bytes from %d.%d.%d.%d:%d ***\r\n",
                                  (long)received,
                                  (int)(sip & 0xFF), (int)((sip >> 8) & 0xFF),
                                  (int)((sip >> 16) & 0xFF), (int)((sip >> 24) & 0xFF),
                                  (int)FreeRTOS_ntohs(sender_addr.sin_port));
        }
        total_rx++;

        /* 打印前 3 个包的完整包头，帮助诊断协议匹配 */
        if (total_rx <= 3) {
            udp_screen_header_t *dbg_hdr = (udp_screen_header_t *)rx_buf;
            rpmsg_log_cpu1_printf("[SCR] hdr[%lu] magic=0x%04X fid=%lu pid=%u/%u len=%u\r\n",
                                  (unsigned long)total_rx,
                                  FreeRTOS_ntohs(dbg_hdr->magic),
                                  (unsigned long)FreeRTOS_ntohl(dbg_hdr->frame_id),
                                  FreeRTOS_ntohs(dbg_hdr->packet_id),
                                  FreeRTOS_ntohs(dbg_hdr->packet_count),
                                  FreeRTOS_ntohs(dbg_hdr->payload_len));
        }

        /* 验证包大小 */
        if (received < (int32_t)UDP_SCREEN_HEADER_SIZE) {
            total_errors++;
            continue;
        }

        /* 解析包头 */
        udp_screen_header_t *hdr = (udp_screen_header_t *)rx_buf;

        /* 验证魔数 (网络字节序转主机字节序) */
        if (FreeRTOS_ntohs(hdr->magic) != ETH_UDP_SCREEN_MAGIC) {
            total_errors++;
            if (total_errors <= 5) {
                rpmsg_log_cpu1_printf("[SCR] MAGIC ERR: 0x%04X (expect 0x%04X)\r\n",
                                      FreeRTOS_ntohs(hdr->magic), ETH_UDP_SCREEN_MAGIC);
            }
            continue;
        }

        /* 网络字节序(大端) -> 主机字节序(小端) */
        uint32_t frame_id     = FreeRTOS_ntohl(hdr->frame_id);
        uint16_t packet_id    = FreeRTOS_ntohs(hdr->packet_id);
        uint16_t packet_count = FreeRTOS_ntohs(hdr->packet_count);
        uint16_t payload_len  = FreeRTOS_ntohs(hdr->payload_len);

        /* 验证字段 */
        if (packet_count != ETH_UDP_SCREEN_PKTS ||
            packet_id >= packet_count ||
            payload_len > ETH_UDP_SCREEN_PAYLOAD) {
            total_errors++;
            /* 打印前几个错误包的详情，帮助定位协议不匹配 */
            if (total_errors <= 5) {
                rpmsg_log_cpu1_printf("[SCR] FIELD ERR: frame=%lu pkt=%u/%u len=%u\r\n",
                                      (unsigned long)frame_id,
                                      packet_id, packet_count, payload_len);
            }
            continue;
        }

        /* 检测新帧 */
        if (frame_id != cur_frame_id) {
            if (pkts_received > 0 && pkts_received < ETH_UDP_SCREEN_PKTS) {
                total_dropped++;
            }
            cur_frame_id = frame_id;
            pkts_received = 0;
        }

        /*
         * 写入后台缓冲区 (非 GLCDC 正在读取的前台缓冲区)。
         * 网络发来的是 RGB565 大端字节序，ARM 是小端。
         * 逐像素读取为 uint16_t（自动完成大端→小端转换）。
         */
        uint32_t offset = (uint32_t)packet_id * ETH_UDP_SCREEN_PAYLOAD;
        if (offset + payload_len <= ETH_UDP_SCREEN_FB_SIZE) {
            uint16_t *dst  = (uint16_t *)(fb_write + offset);
            uint8_t  *src  = rx_buf + UDP_SCREEN_HEADER_SIZE;
            uint16_t pixels = payload_len / 2;
            for (uint16_t i = 0; i < pixels; i++) {
                dst[i] = ((uint16_t)src[i * 2] << 8) | src[i * 2 + 1];
            }
        }

        pkts_received++;

        /* 打印前 3 包 + 每 200 包 */
        if (total_rx <= 3 || (total_rx % 200) == 0) {
            rpmsg_log_cpu1_printf("[SCR] rx pkt pid=%u/%u total=%lu\r\n",
                                  pkts_received, ETH_UDP_SCREEN_PKTS,
                                  (unsigned long)total_rx);
        }

        /* 帧收完 */
        if (pkts_received >= ETH_UDP_SCREEN_PKTS) {
            total_frames++;
            pkts_received = 0;
            cur_frame_id = 0xFFFFFFFF;

            /* GLCDC 双缓冲切换 */
            R_GLCDC_BufferChange(&g_display0_ctrl, fb_write, DISPLAY_FRAME_LAYER_1);

            /* 交换缓冲区索引 */
            write_idx = 1 - write_idx;
            fb_write = fb_background[write_idx];

            /* 帧完成统计 (每 30 帧打印) */
            if ((total_frames % 30) == 0) {
                rpmsg_log_cpu1_printf("[SCR] Frame #%lu | dropped=%lu err=%lu\r\n",
                                      (unsigned long)total_frames,
                                      (unsigned long)total_dropped,
                                      (unsigned long)total_errors);
            }
        }
    }
}

int ethernet_test_udp_screen_start(int priority, int stack_size)
{
    BaseType_t ret = xTaskCreate(udp_screen_task,
                                 "UDP_Screen",
                                 (configSTACK_DEPTH_TYPE)stack_size,
                                 NULL,
                                 (UBaseType_t)priority,
                                 NULL);
    if (ret != pdPASS) {
        rpmsg_log_cpu1_printf("[SCR] Task create failed\r\n");
        return -1;
    }
    return 0;
}

#endif /* ETH_UDP_SCREEN_ENABLE */

/* ---- Run all tests ---- */

void ethernet_test_run(void)
{
    rpmsg_log_cpu1_printf("\r\n===== Ethernet Test Start =====\r\n");

    char ip[16], nm[16], gw[16];
    ethernet_get_ip(ip, sizeof(ip));
    ethernet_get_netmask(nm, sizeof(nm));
    ethernet_get_gateway(gw, sizeof(gw));
    rpmsg_log_cpu1_printf("[TEST] IP: %s  Mask: %s  GW: %s  DHCP: %s\r\n",
                          ip, nm, gw, ethernet_is_dhcp() ? "Yes" : "No");

    /* 1. Speed test (needs PC receiver script running first) */
    rpmsg_log_cpu1_printf("[TEST] Starting speed test...\r\n");
    ethernet_test_speed();

    /* 2. Ping test (run before TCP echo server to avoid buffer contention) */
    vTaskDelay(pdMS_TO_TICKS(1000));
    ethernet_test_ping();

    /* 3. Start TCP echo server (runs in background forever) */
    vTaskDelay(pdMS_TO_TICKS(1000));
    ethernet_test_tcp_echo_start(3, 1024);

    /* 4. Start UDP echo server (runs in background forever) */
    vTaskDelay(pdMS_TO_TICKS(500));
    ethernet_test_udp_echo_start(3, 1024);

#if ETH_UDP_SCREEN_ENABLE
    /* 5. Start UDP screen receiver (runs in background forever) */
    vTaskDelay(pdMS_TO_TICKS(500));
    ethernet_test_udp_screen_start(3, 2048);
#endif

    rpmsg_log_cpu1_printf("\r\n===== Ethernet Test Done =====\r\n");
    rpmsg_log_cpu1_printf("[TEST] TCP echo server on port %d\r\n", ETH_TEST_TCP_PORT);
    rpmsg_log_cpu1_printf("[TEST] UDP echo server on port %d\r\n", ETH_TEST_UDP_PORT);
#if ETH_UDP_SCREEN_ENABLE
    rpmsg_log_cpu1_printf("[TEST] UDP screen receiver on port %d\r\n", ETH_UDP_SCREEN_PORT);
#endif
    rpmsg_log_cpu1_printf("[TEST] TCP connect: telnet %s %d\r\n", ip, ETH_TEST_TCP_PORT);
    rpmsg_log_cpu1_printf("[TEST] UDP echo test: python -c \"import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.sendto(b'hello',('%s',%d)); print(s.recvfrom(1024))\"\r\n",
                          ip, ETH_TEST_UDP_PORT);
#if ETH_UDP_SCREEN_ENABLE
    rpmsg_log_cpu1_printf("[TEST] UDP screen: python udp_screen_sender_1024x600.py --target-ip %s --local-ip <PC_IP>\r\n", ip);
#endif
}
