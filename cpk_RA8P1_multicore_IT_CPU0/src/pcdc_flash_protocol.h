/*
 * pcdc_flash_protocol.h
 *
 * PCDC Flash Transfer Protocol — frame format, commands, CRC16 API.
 *
 * Used by the PCDC thread to receive and execute flash read/write/erase
 * commands from a PC-side tool over the USB virtual COM port.
 *
 * Frame format (overhead 8 bytes):
 *   | STX(0xAA) | CMD(1B) | LEN(2B LE) | PAYLOAD(LEN bytes) | CRC16(2B LE) |
 *
 * CRC16 polynomial 0x8005 (XMODEM), covers CMD + LEN + PAYLOAD.
 */

#ifndef PCDC_FLASH_PROTOCOL_H_
#define PCDC_FLASH_PROTOCOL_H_

#include <stdint.h>
#include <stdbool.h>

/* ---------- Frame constants ---------- */

#define PCDC_STX                    0xAAU
#define PCDC_MAX_PAYLOAD            256U        /* max payload per frame      */
#define PCDC_FRAME_OVERHEAD         6U          /* STX+CMD+LEN+CRC16          */
#define PCDC_MAX_FRAME              (PCDC_FRAME_OVERHEAD + PCDC_MAX_PAYLOAD)
#define PCDC_PROTO_BUF_SIZE         320U        /* generous headroom          */

/* ---------- Commands ---------- */

#define PCDC_CMD_PING               0x00U
#define PCDC_CMD_INFO               0x01U
#define PCDC_CMD_ERASE              0x10U
#define PCDC_CMD_WRITE              0x20U
#define PCDC_CMD_READ               0x30U
#define PCDC_CMD_DIR_READ           0x40U
#define PCDC_CMD_DIR_ADD            0x41U
#define PCDC_CMD_RESET              0xF0U

/* Response codes (re-use CMD field) */
#define PCDC_CMD_ACK                0xFEU
#define PCDC_CMD_NACK               0xFFU

/* ---------- NACK error codes ---------- */

#define PCDC_ERR_NONE               0x00U
#define PCDC_ERR_BAD_CMD            0x01U
#define PCDC_ERR_CRC                0x02U
#define PCDC_ERR_FLASH_WRITE        0x03U
#define PCDC_ERR_FLASH_ERASE        0x04U
#define PCDC_ERR_ADDR_RANGE         0x05U
#define PCDC_ERR_BAD_LEN            0x07U
#define PCDC_ERR_FLASH_BUSY         0x08U

/* ---------- INFO response structure ---------- */

typedef struct {
    uint32_t total_size;            /* flash total capacity, bytes    */
    uint32_t sector_size;           /* erase sector size, bytes       */
    uint32_t page_size;             /* page program size, bytes       */
    uint16_t max_payload;           /* max payload per WRITE frame    */
} pcdc_flash_info_t;

/* ---------- API ---------- */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Compute XMODEM CRC16 (polynomial 0x8005).
 * @param  data  Input bytes
 * @param  len   Number of bytes
 * @return 16-bit CRC
 */
uint16_t pcdc_crc16(const uint8_t *data, uint32_t len);

/**
 * @brief  Build a protocol frame in buf.
 *         buf must be at least PCDC_MAX_FRAME bytes.
 * @param  buf      Output buffer
 * @param  cmd      Command byte (or ACK/NACK)
 * @param  payload  Payload data (can be NULL if len == 0)
 * @param  len      Payload length in bytes (0..PCDC_MAX_PAYLOAD)
 * @return Total frame size written to buf (0 on error)
 */
uint16_t pcdc_build_frame(uint8_t *buf, uint8_t cmd,
                          const uint8_t *payload, uint16_t len);

/**
 * @brief  Validate a received frame.
 * @param  buf   Frame buffer
 * @param  size  Number of bytes received
 * @param  out_payload_len  Output: payload length (can be NULL)
 * @return Command byte if valid, PCDC_CMD_NACK (0xFF) if invalid
 */
uint8_t pcdc_validate_frame(const uint8_t *buf, uint32_t size,
                            uint16_t *out_payload_len);

/**
 * @brief  Extract payload pointer from a validated frame.
 * @param  buf  Frame buffer (must have passed pcdc_validate_frame)
 * @return Pointer to payload (points into buf at offset 4)
 */
static inline const uint8_t * pcdc_payload_ptr(const uint8_t *buf) {
    return buf + 4;
}

#ifdef __cplusplus
}
#endif

#endif /* PCDC_FLASH_PROTOCOL_H_ */
