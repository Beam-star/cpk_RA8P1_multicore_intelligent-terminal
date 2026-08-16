/**
 ******************************************************************************
 * @file    file_transfer_server.c
 * @brief   TCP 文件传输服务实现 (CFTP 协议)
 *
 * 服务端工作流:
 *   1. 创建 TCP socket → bind(8080) → listen
 *   2. 主循环 accept() 等待客户端连接 (最多 3 个并发)
 *   3. 为每个客户端创建独立处理任务
 *   4. 客户端处理:
 *      a. 接收命令 (阻塞, 5s 超时)
 *      b. 解析命令 (LIST/GET/DELETE/INFO)
 *      c. LIST → 扫描 SD 卡 /MEETING/ → JSON 响应
 *      d. GET  → SDHI DMA 读取文件 → 分块 TCP 发送 (CRC16)
 *      e. DELETE → sd_card_delete()
 *      f. INFO → sd_card_get_info()
 *   5. 客户端断开 → 清理资源
 ******************************************************************************
 */

#include "file_transfer_server.h"
#include "cftp_protocol.h"
#include "sdhi_driver.h"
#include "FreeRTOS.h"
#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

/* ---- 客户端处理任务 ---- */
static void cftp_client_handler(void *pvParameters)
{
    Socket_t client_sock = (Socket_t)pvParameters;
    uint8_t  rx_buf[CFTP_CHUNK_SIZE];
    int32_t  rx_len;

    printf("[CFTP] Client connected, waiting for commands...\r\n");

    /* 设置接收超时 */
    TickType_t timeout = pdMS_TO_TICKS(CFTP_RECV_TIMEOUT_MS);
    FreeRTOS_setsockopt(client_sock, 0, FREERTOS_SO_RCVTIMEO,
                        &timeout, sizeof(timeout));

    while (1) {
        rx_len = FreeRTOS_recv(client_sock, rx_buf, sizeof(rx_buf) - 1, 0);
        if (rx_len <= 0) {
            break;  /* 超时或断开 */
        }
        rx_buf[rx_len] = '\0';

        /* TODO: 解析命令并执行
         * if (strncmp(rx_buf, "LIST", 4) == 0) {
         *     cftp_handle_list(client_sock);
         * } else if (strncmp(rx_buf, "GET ", 4) == 0) {
         *     cftp_handle_get(client_sock, rx_buf + 4);
         * } else if (strncmp(rx_buf, "DELETE ", 7) == 0) {
         *     cftp_handle_delete(client_sock, rx_buf + 7);
         * } else if (strncmp(rx_buf, "INFO", 4) == 0) {
         *     cftp_handle_info(client_sock);
         * }
         */
    }

    printf("[CFTP] Client disconnected\r\n");
    FreeRTOS_closesocket(client_sock);
    vTaskDelete(NULL);
}

/* ---- 服务端主任务 ---- */
static void cftp_server_task(void *pvParameters)
{
    (void)pvParameters;
    Socket_t listen_sock;
    struct freertos_sockaddr bind_addr;
    int client_count = 0;

    /* 创建监听 socket */
    listen_sock = FreeRTOS_socket(FREERTOS_AF_INET, FREERTOS_SOCK_STREAM,
                                  FREERTOS_IPPROTO_TCP);
    if (listen_sock == FREERTOS_INVALID_SOCKET) {
        printf("[CFTP] Failed to create socket\r\n");
        vTaskDelete(NULL);
        return;
    }

    /* 绑定端口 */
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_port = FreeRTOS_htons(CFTP_SERVER_PORT);
    bind_addr.sin_addr = 0;  /* INADDR_ANY */
    FreeRTOS_bind(listen_sock, &bind_addr, sizeof(bind_addr));
    FreeRTOS_listen(listen_sock, CFTP_MAX_CLIENTS);

    /* 获取本机 IP */
    extern int ethernet_get_ip(char *buf, int size);
    char ip[16];
    ethernet_get_ip(ip, sizeof(ip));
    printf("[CFTP] File transfer server listening on %s:%d\r\n",
           ip, CFTP_SERVER_PORT);

    /* 主 accept 循环 */
    while (1) {
        struct freertos_sockaddr client_addr;
        socklen_t client_len = sizeof(client_addr);
        Socket_t client_sock = FreeRTOS_accept(listen_sock,
                                               &client_addr, &client_len);
        if (client_sock == FREERTOS_INVALID_SOCKET) {
            continue;
        }

        if (client_count >= CFTP_MAX_CLIENTS) {
            FreeRTOS_closesocket(client_sock);  /* 拒绝多余连接 */
            continue;
        }

        client_count++;

        /* 为每个客户端创建独立任务 */
        char task_name[16];
        snprintf(task_name, sizeof(task_name), "cftp_%d", client_count);
        xTaskCreate(cftp_client_handler, task_name, 2048,
                    (void *)client_sock, 2, NULL);
    }
}

/* ---- Public API ---- */

bool file_transfer_server_start(int priority, int stack_size)
{
    BaseType_t ret = xTaskCreate(cftp_server_task, "CFTP_Srv",
                                  (configSTACK_DEPTH_TYPE)stack_size,
                                  NULL, (UBaseType_t)priority, NULL);
    if (ret != pdPASS) {
        printf("[CFTP] Failed to create server task\r\n");
        return false;
    }
    return true;
}

void file_transfer_server_stop(void)
{
    /* TODO: 关闭监听 socket, 通知所有客户端任务退出 */
}

int file_transfer_server_client_count(void)
{
    return 0;  /* TODO: 维护全局 client_count */
}

bool file_transfer_is_file_busy(const char *filename)
{
    (void)filename;
    return false;
}
