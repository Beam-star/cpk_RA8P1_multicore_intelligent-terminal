/**
 ******************************************************************************
 * @file    cftp_protocol.c
 * @brief   CFTP 协议实现
 ******************************************************************************
 */

#include "cftp_protocol.h"
#include "sdhi_driver.h"
#include <stdio.h>
#include <string.h>

/* ---- CRC16 查找表 (XMODEM, 多项式 0x8005) ---- */
static const uint16_t CRC16_TABLE[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    /* ... 完整 256 项 CRC16 查找表 */
};

uint16_t cftp_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0x0000;
    for (uint32_t i = 0; i < len; i++) {
        crc = (uint16_t)((crc << 8) ^
              CRC16_TABLE[((crc >> 8) ^ data[i]) & 0xFF]);
    }
    return crc;
}

void cftp_handle_list(Socket_t client_sock)
{
    /* TODO: 扫描 SD 卡 /MEETING/ 目录
     * sd_file_info_t files[128];
     * int count = sd_card_list_dir("/MEETING", files, 128);
     * 构建 JSON: [{"name":"...","size":...,"date":"...","duration":...}, ...]
     * cftp_send_response(sock, 200, json_str);
     */
    cftp_send_response(client_sock, 200, "[]");
}

void cftp_handle_get(Socket_t client_sock, const char *args)
{
    /* TODO: 解析 GET <filename> <offset>
     * 1. 打开 SD 卡文件
     * 2. seek 到 offset
     * 3. 循环: sd_card_fread → cftp_crc16 → cftp_send_file_chunk
     * 4. 最后一块 < CFTP_PAYLOAD_SIZE 表示完成
     */
    (void)client_sock; (void)args;
}

void cftp_handle_delete(Socket_t client_sock, const char *filename)
{
    /* TODO: sd_card_delete(filename);
     * cftp_send_response(sock, 200, "{\"deleted\":\"...\"}");
     */
    (void)client_sock; (void)filename;
}

void cftp_handle_info(Socket_t client_sock)
{
    /* TODO: sd_info_t info;
     * sd_card_get_info(&info);
     * JSON: {"total_mb":...,"free_mb":...,"fat_type":"FAT32"}
     */
    cftp_send_response(client_sock, 200,
                       "{\"total_mb\":32000,\"free_mb\":28000,\"fat_type\":\"FAT32\"}");
}

void cftp_send_response(Socket_t sock, int status_code, const char *json)
{
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "%d %d\r\n%s",
                       status_code, (int)strlen(json), json);
    FreeRTOS_send(sock, buf, len, 0);
}

bool cftp_send_file_chunk(Socket_t sock, uint32_t seq,
                          const uint8_t *data, uint32_t len)
{
    uint8_t chunk[CFTP_CHUNK_SIZE];

    /* 序列号 (大端) */
    chunk[0] = (uint8_t)(seq >> 24);
    chunk[1] = (uint8_t)(seq >> 16);
    chunk[2] = (uint8_t)(seq >> 8);
    chunk[3] = (uint8_t)(seq);

    /* CRC16 (大端) */
    uint16_t crc = cftp_crc16(data, len);
    chunk[4] = (uint8_t)(crc >> 8);
    chunk[5] = (uint8_t)(crc);

    /* 数据 */
    memcpy(chunk + CFTP_HEADER_SIZE, data, len);

    int32_t sent = FreeRTOS_send(sock, chunk, CFTP_HEADER_SIZE + len, 0);
    return (sent == (int32_t)(CFTP_HEADER_SIZE + len));
}
