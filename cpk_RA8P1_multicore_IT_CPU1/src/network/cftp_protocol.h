/**
 ******************************************************************************
 * @file    cftp_protocol.h
 * @brief   CFTP (Camera File Transfer Protocol) 协议实现
 *
 * 协议格式:
 *   请求:  <COMMAND> [arg1] [arg2]\r\n
 *   响应:  <STATUS> <payload_length>\r\n<payload>
 *
 *   状态码:
 *     200 OK
 *     400 Bad Request
 *     404 File Not Found
 *     500 Internal Error
 *
 *   LIST 响应示例:
 *     200 256\r\n
 *     [{"name":"recording.avi","size":1970000000,"date":"20250708_143000","duration":3600}, ...]
 *
 *   GET 传输格式 (分块):
 *     每块: [4B seq_num (BE)] [2B crc16 (BE)] [nB data]
 *     最后一块 data_len < CFTP_CHUNK_SIZE 表示传输完成
 *
 *   CRC16: 多项式 0x8005 (XMODEM), 初始值 0x0000
 ******************************************************************************
 */

#ifndef CFTP_PROTOCOL_H_
#define CFTP_PROTOCOL_H_

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS_Sockets.h"

/* ---- 协议常量 ---- */
#define CFTP_CHUNK_SIZE         1460
#define CFTP_HEADER_SIZE        6      /* seq(4) + crc(2) */
#define CFTP_PAYLOAD_SIZE       (CFTP_CHUNK_SIZE - CFTP_HEADER_SIZE)

/* ---- 命令处理 ---- */

/** 处理 LIST 命令: 扫描 SD 卡, 返回 JSON 文件清单 */
void cftp_handle_list(Socket_t client_sock);

/** 处理 GET 命令: 分块传输 SD 卡文件 */
void cftp_handle_get(Socket_t client_sock, const char *args);

/** 处理 DELETE 命令: 删除 SD 卡文件 */
void cftp_handle_delete(Socket_t client_sock, const char *filename);

/** 处理 INFO 命令: 返回 SD 卡存储空间信息 */
void cftp_handle_info(Socket_t client_sock);

/* ---- 工具函数 ---- */

/** 计算 CRC16 (XMODEM) */
uint16_t cftp_crc16(const uint8_t *data, uint32_t len);

/** 发送 JSON 响应 */
void cftp_send_response(Socket_t sock, int status_code, const char *json);

/** 发送分块文件数据 */
bool cftp_send_file_chunk(Socket_t sock, uint32_t seq,
                          const uint8_t *data, uint32_t len);

#endif /* CFTP_PROTOCOL_H_ */
