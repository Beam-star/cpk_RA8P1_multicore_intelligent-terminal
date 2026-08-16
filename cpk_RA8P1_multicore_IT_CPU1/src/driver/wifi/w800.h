/**
 ******************************************************************************
 * @file    w800.h
 * @brief   W800 WiFi 模块驱动头文件 (UART AT 指令)
 *
 * 模块: WinnerMicro W800 (2.4GHz WiFi, 802.11b/g/n)
 * 接口: UART (AT 指令集), 波特率 115200
 * 用途: 无线会议记录文件传输
 *
 * WiFi 模式: Station (连接路由器) / SoftAP (自建热点)
 ******************************************************************************
 */

#ifndef W800_H_
#define W800_H_

#include <stdint.h>
#include <stdbool.h>

/* ---- WiFi 状态 ---- */
typedef enum {
    WIFI_STA_DISCONNECTED = 0,
    WIFI_STA_CONNECTING,
    WIFI_STA_CONNECTED,
    WIFI_AP_MODE,
} wifi_status_t;

/* ---- API ---- */

/**
 * @brief 初始化 W800 WiFi 模块
 * @param uart_ch  UART 通道号
 */
bool w800_init(uint8_t uart_ch);

/**
 * @brief 连接 WiFi (Station 模式)
 * @param ssid     WiFi SSID
 * @param password WiFi 密码
 */
bool w800_connect(const char *ssid, const char *password);

/**
 * @brief 启动 SoftAP 热点模式
 * @param ssid     热点名称
 * @param password 热点密码 (至少8位)
 */
bool w800_start_ap(const char *ssid, const char *password);

/**
 * @brief 获取当前 WiFi 状态
 */
wifi_status_t w800_get_status(void);

/**
 * @brief 获取分配的 IP 地址
 * @param ip_buf  输出缓冲区 (≥16字节)
 */
bool w800_get_ip(char *ip_buf, int buf_size);

/**
 * @brief 创建 TCP 客户端连接
 * @param ip    目标 IP
 * @param port  目标端口
 * @return socket 句柄, -1=失败
 */
int w800_tcp_connect(const char *ip, uint16_t port);

/**
 * @brief TCP 发送数据
 */
int w800_tcp_send(int sock, const void *data, uint32_t len);

/**
 * @brief TCP 接收数据
 */
int w800_tcp_recv(int sock, void *buf, uint32_t len, uint32_t timeout_ms);

/**
 * @brief 关闭 TCP 连接
 */
void w800_tcp_close(int sock);

/**
 * @brief 断开 WiFi
 */
void w800_disconnect(void);

#endif /* W800_H_ */
