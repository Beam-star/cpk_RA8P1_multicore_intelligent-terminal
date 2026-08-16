/**
 ******************************************************************************
 * @file    w800.c
 * @brief   W800 WiFi 模块驱动实现 (UART AT 指令)
 *
 * AT 指令示例:
 *   AT+WJOIN=ssid,password  — 连接 WiFi
 *   AT+WJAP=ssid,password   — 启动热点
 *   AT+SKCT=ip,port         — 创建 TCP socket
 *   AT+SKSEND=sock,data     — TCP 发送
 *
 * UART 配置: 115200, 8N1, 无流控
 ******************************************************************************
 */

#include "w800.h"
#include "hal_data.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

static wifi_status_t g_wifi_status = WIFI_STA_DISCONNECTED;
static char g_wifi_ip[16] = {0};

bool w800_init(uint8_t uart_ch)
{
    printf("[W800] Initializing WiFi module (UART%u, 115200)...\r\n", uart_ch);

    /* TODO: 通过 FSP API 初始化 UART
     * R_SCI_UART_Open(&g_uart_wifi_ctrl, &g_uart_wifi_cfg);
     * 发送 AT 指令测试通信: AT\r\n → 期待 AT OK
     * 配置为 Station 模式: AT+WMODE=STA\r\n
     */

    g_wifi_status = WIFI_STA_DISCONNECTED;
    printf("[W800] Initialization complete\r\n");
    return true;
}

bool w800_connect(const char *ssid, const char *password)
{
    printf("[W800] Connecting to WiFi: %s...\r\n", ssid);
    /* TODO: 发送 AT+WJOIN=%s,%s\r\n, 等待 +WJOIN:OK */
    (void)ssid; (void)password;

    g_wifi_status = WIFI_STA_CONNECTED;
    strcpy(g_wifi_ip, "192.168.1.101");
    printf("[W800] Connected, IP: %s\r\n", g_wifi_ip);
    return true;
}

bool w800_start_ap(const char *ssid, const char *password)
{
    printf("[W800] Starting AP: %s...\r\n", ssid);
    /* TODO: 发送 AT+WJAP=%s,%s\r\n */
    (void)ssid; (void)password;

    g_wifi_status = WIFI_AP_MODE;
    strcpy(g_wifi_ip, "192.168.4.1");
    printf("[W800] AP started, IP: %s\r\n", g_wifi_ip);
    return true;
}

wifi_status_t w800_get_status(void)
{
    return g_wifi_status;
}

bool w800_get_ip(char *ip_buf, int buf_size)
{
    if (ip_buf && buf_size > 0) {
        strncpy(ip_buf, g_wifi_ip, buf_size);
        return true;
    }
    return false;
}

int w800_tcp_connect(const char *ip, uint16_t port)
{
    /* TODO: AT+SKCT=%s,%u\r\n → socket_id */
    (void)ip; (void)port;
    return 0;
}

int w800_tcp_send(int sock, const void *data, uint32_t len)
{
    /* TODO: AT+SKSEND=%d,%d\r\n + 数据 */
    (void)sock; (void)data; (void)len;
    return (int)len;
}

int w800_tcp_recv(int sock, void *buf, uint32_t len, uint32_t timeout_ms)
{
    (void)sock; (void)buf; (void)len; (void)timeout_ms;
    return 0;
}

void w800_tcp_close(int sock)
{
    /* TODO: AT+SKCLS=%d\r\n */
    (void)sock;
}

void w800_disconnect(void)
{
    /* TODO: AT+WLEAV\r\n */
    g_wifi_status = WIFI_STA_DISCONNECTED;
}
