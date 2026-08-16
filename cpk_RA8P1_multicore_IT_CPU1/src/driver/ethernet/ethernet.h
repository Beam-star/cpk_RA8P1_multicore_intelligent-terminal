/*
 * ethernet.h - RA8P1 Ethernet driver (FreeRTOS+TCP wrapper)
 *
 * Encapsulates FreeRTOS+TCP stack initialization and network status management.
 * Hardware: RA8P1 built-in MAC (RMAC) + external PHY via RGMII.
 */

#ifndef ETHERNET_H_
#define ETHERNET_H_

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS_Sockets.h"  /* Socket_t, freertos_sockaddr, etc. */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- IP mode selection ---- */
/* Define ETH_USE_DHCP to enable DHCP mode; otherwise static IP is used */
/* #define ETH_USE_DHCP */

/* ---- Static IP configuration (used when ETH_USE_DHCP is not defined) ---- */
#ifndef ETH_USE_DHCP
#define ETH_STATIC_IP          "192.168.1.100"
#define ETH_STATIC_NETMASK     "255.255.255.0"
#define ETH_STATIC_GATEWAY     "192.168.1.1"
#define ETH_STATIC_DNS         "8.8.8.8"
#endif

/* ---- Default MAC address ---- */
#define ETH_MAC_ADDR           {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}

/* ---- TCP Echo Server default port ---- */
#define ETH_TCP_ECHO_PORT      7

/* ---- Network status ---- */
typedef enum {
    ETH_STATUS_LINK_DOWN = 0,  /* Ethernet PHY link is down */
    ETH_STATUS_LINK_UP,        /* PHY link up, but IP not ready */
    ETH_STATUS_IP_READY,       /* Network fully operational */
} eth_status_t;

/**
 * @brief Initialize FreeRTOS+TCP stack and start network interface.
 *
 * Calls FreeRTOS_IPInit() which triggers the full hardware init chain:
 *   R_LAYER3_SWITCH_Open -> R_RMAC_Open -> PHY init -> auto-negotiation
 *
 * Must be called once from a FreeRTOS task before any network operation.
 * @return 0 on success, negative on failure.
 */
int ethernet_init(void);

/**
 * @brief Check current network status.
 * @return eth_status_t value indicating link/IP state.
 */
eth_status_t ethernet_get_status(void);

/**
 * @brief Wait until network is fully up (PHY link + IP stack ready).
 * @param timeout_ms  Maximum wait time in milliseconds. 0 = wait forever.
 * @return 0 if network is up, negative on timeout.
 */
int ethernet_wait_ready(uint32_t timeout_ms);

/**
 * @brief Get current IP address as string.
 * @param buf   Buffer to store IP string (at least 16 bytes).
 * @param size  Buffer size.
 * @return 0 on success.
 */
int ethernet_get_ip(char *buf, int size);

/**
 * @brief Get current netmask as string.
 */
int ethernet_get_netmask(char *buf, int size);

/**
 * @brief Get current gateway as string.
 */
int ethernet_get_gateway(char *buf, int size);

/**
 * @brief Send a single ICMP ping request.
 * @param target_ip  Destination IP in dot-decimal string (e.g. "192.168.1.1").
 * @return pdTRUE if ping request was sent, pdFALSE on failure.
 */
int ethernet_ping(const char *target_ip);

/**
 * @brief Check if DHCP was used to obtain the IP address.
 * @return true if DHCP is active, false if using static IP.
 */
bool ethernet_is_dhcp(void);

/* ========================================================================
 * UDP Socket Helper API
 *
 * BSD socket 风格的 UDP 辅助函数，对 FreeRTOS+TCP socket API 的简单封装。
 * FreeRTOS+TCP 原生支持 UDP，无需额外配置宏（UDP 始终编译）。
 *
 * 典型 UDP 通信流程：
 *   1. sock = ethernet_udp_create()           创建 DGRAM socket
 *   2. ethernet_udp_bind(sock, 5000)          绑定本地端口
 *   3. ethernet_udp_recvfrom(sock, ...)       接收数据
 *      或 ethernet_udp_sendto(sock, ...)     发送数据
 *   4. ethernet_udp_close(sock)               关闭 socket
 *
 * 也可以直接使用 FreeRTOS+TCP 原生 API：
 *   FreeRTOS_socket(FREERTOS_AF_INET, FREERTOS_SOCK_DGRAM, FREERTOS_IPPROTO_UDP)
 *   FreeRTOS_sendto() / FreeRTOS_recvfrom()
 * ======================================================================== */

/**
 * @brief Create a UDP socket (SOCK_DGRAM).
 * @return Socket handle, or FREERTOS_INVALID_SOCKET on failure.
 */
Socket_t ethernet_udp_create(void);

/**
 * @brief Bind UDP socket to a local port.
 * @param sock  Socket handle from ethernet_udp_create().
 * @param port  Local port number (host byte order).
 * @return 0 on success, negative on failure.
 */
int ethernet_udp_bind(Socket_t sock, uint16_t port);

/**
 * @brief Send UDP datagram to a specific address.
 * @param sock      Socket handle.
 * @param data      Data buffer to send.
 * @param len       Data length in bytes.
 * @param dest_ip   Destination IP in dot-decimal string (e.g. "192.168.1.10").
 * @param dest_port Destination port (host byte order).
 * @return Number of bytes sent, or negative on error.
 */
int ethernet_udp_sendto(Socket_t sock, const void *data, uint32_t len,
                        const char *dest_ip, uint16_t dest_port);

/**
 * @brief Receive a UDP datagram (blocking with timeout).
 * @param sock         Socket handle.
 * @param buf          Receive buffer.
 * @param buf_len      Buffer size in bytes.
 * @param timeout_ms   Receive timeout in milliseconds.
 * @param src_ip       [out] Source IP string buffer (can be NULL).
 * @param src_ip_len   Source IP buffer size.
 * @param src_port     [out] Source port (host byte order, can be NULL).
 * @return Number of bytes received, 0 on timeout, negative on error.
 */
int ethernet_udp_recvfrom(Socket_t sock, void *buf, uint32_t buf_len,
                          uint32_t timeout_ms,
                          char *src_ip, int src_ip_len, uint16_t *src_port);

/**
 * @brief Close a UDP socket.
 * @param sock  Socket handle.
 */
void ethernet_udp_close(Socket_t sock);

#ifdef __cplusplus
}
#endif

#endif /* ETHERNET_H_ */
