/**
 ******************************************************************************
 * @file    file_transfer_server.h
 * @brief   TCP 文件传输服务头文件 (CFTP 协议)
 *
 * 功能: 将 SD 卡中的 AVI 会议记录文件通过 TCP 传输至 PC/手机客户端
 *
 * 协议: CFTP (Camera File Transfer Protocol) — 自定义应用层协议
 *
 * 命令:
 *   LIST              — 扫描 SD 卡录制目录, 返回 JSON 文件清单
 *   GET <filename> <offset> — 从 SD 卡读取文件, 从 offset 偏移处开始分块传输
 *   DELETE <filename> — 删除 SD 卡上指定文件
 *   INFO              — 获取 SD 卡存储空间信息
 *
 * 传输层:
 *   TCP 端口 8080 (以太网) / TCP 端口 8080 (WiFi via W800)
 *   分块大小: ≤1460 字节 (对齐 TCP MSS)
 *   校验: CRC16 (每块)
 *   断点续传: 支持 (通过 GET offset 参数)
 *
 * 并发: 最多 3 个客户端同时连接, 各自独立传输
 ******************************************************************************
 */

#ifndef FILE_TRANSFER_SERVER_H_
#define FILE_TRANSFER_SERVER_H_

#include <stdint.h>
#include <stdbool.h>

/* ---- 服务配置 ---- */
#define CFTP_SERVER_PORT        (8080)
#define CFTP_MAX_CLIENTS        (3)
#define CFTP_CHUNK_SIZE         (1460)     /* TCP MSS */
#define CFTP_RECV_TIMEOUT_MS    (5000)

/* ---- API ---- */

/**
 * @brief 启动 TCP 文件传输服务
 * @param priority    FreeRTOS 任务优先级
 * @param stack_size  栈大小 (word)
 * @return true=服务已启动
 */
bool file_transfer_server_start(int priority, int stack_size);

/**
 * @brief 停止文件传输服务
 */
void file_transfer_server_stop(void);

/**
 * @brief 获取当前连接的客户端数量
 */
int file_transfer_server_client_count(void);

/**
 * @brief 检查指定文件是否正在传输中
 */
bool file_transfer_is_file_busy(const char *filename);

#endif /* FILE_TRANSFER_SERVER_H_ */
