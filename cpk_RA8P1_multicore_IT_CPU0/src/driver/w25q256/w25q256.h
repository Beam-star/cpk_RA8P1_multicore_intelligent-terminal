/*
 * w25q256.h
 *
 * W25Q256JV QSPI Flash driver for RA8P1 (via OSPI_B, unit 0, CS0).
 * Capacity: 256Mbit (32MB), Page: 256B, Sector: 4KB, Block: 32KB/64KB
 * JEDEC ID: EF 40 19
 * Memory-mapped base: 0x90000000
 *
 * e2studio config requirements:
 *   - OSPI_B module: Unit 0, Channel 0 (CS0)
 *   - SPI protocol: 1S-4S-4S (Quad, default for CPK board)
 *   - Erase commands: 0x20 (4KB), 0x52 (32KB), 0xD8 (64KB), 0x60 (chip)
 *   - Page Size Bytes: 256 (must match W25Q256 actual page size)
 *   - Address bytes: 3 (for ≤16MB) or 4 (for full 32MB range)
 *   - Status Dummy Cycles: 0 (W25Q256 does not support dummy cycles in SR reads)
 *
 * CPK board notes:
 *   - Quad mode (1S-4S-4S) is tested and working on CPK hardware
 *   - Normal R_OSPI_B_Write path works — no DirectTransfer workaround needed
 */

#ifndef W25Q256_H_
#define W25Q256_H_

#include <stdint.h>
#include <stdbool.h>
#include "bsp_api.h"
#include "hal_data.h"

#define W25Q256_MEM_BASE          0x80000000UL   /* OSPI CS0 on CPK board */
#define W25Q256_CAPACITY          0x02000000UL   /* 32MB */

/* ---------- Geometry ---------- */
#define W25Q256_PAGE_SIZE         256U
#define W25Q256_SECTOR_SIZE       0x1000U        /* 4KB */
#define W25Q256_BLOCK32K_SIZE     0x8000U        /* 32KB */
#define W25Q256_BLOCK64K_SIZE     0x10000U       /* 64KB */

/* ---------- Expected JEDEC ID ---------- */
#define W25Q256_JEDEC_MFR         0xEF   /* Winbond */
#define W25Q256_JEDEC_TYPE        0x40
#define W25Q256_JEDEC_CAPACITY    0x19   /* 256Mbit */

/* ---------- Status Register 1 Bits ---------- */
#define W25Q256_SR1_BUSY          (1U << 0)
#define W25Q256_SR1_WEL           (1U << 1)

/* ---------- Timeout ---------- */
#define W25Q256_BUSY_TIMEOUT_MS   5000U

/* ---------- Return Codes ---------- */
typedef enum {
    W25Q256_OK = 0,
    W25Q256_ERR_OPEN,
    W25Q256_ERR_ERASE,
    W25Q256_ERR_WRITE,
    W25Q256_ERR_READ,
    W25Q256_ERR_BUSY_TIMEOUT,
    W25Q256_ERR_VERIFY,
    W25Q256_ERR_ADDR_RANGE,
} w25q256_err_t;

/* ---------- JEDEC ID ---------- */
typedef struct {
    uint8_t manufacturer;
    uint8_t memory_type;
    uint8_t capacity;
} w25q256_jedec_id_t;

/* ---------- API ---------- */

#ifdef __cplusplus
extern "C" {
#endif

w25q256_err_t w25q256_open(void);
w25q256_err_t w25q256_close(void);

w25q256_err_t w25q256_read_jedec_id(w25q256_jedec_id_t *p_id);
w25q256_err_t w25q256_read_status_reg1(uint8_t *p_status);

w25q256_err_t w25q256_erase_sector(uint32_t addr);
w25q256_err_t w25q256_erase_block_64k(uint32_t addr);
w25q256_err_t w25q256_erase_chip(void);

w25q256_err_t w25q256_write(uint32_t addr, const uint8_t *p_data, uint32_t length);
w25q256_err_t w25q256_read(uint32_t addr, uint8_t *p_data, uint32_t length);
w25q256_err_t w25q256_write_and_verify(uint32_t addr, const uint8_t *p_data, uint32_t length);

w25q256_err_t w25q256_wait_busy(void);
const char *  w25q256_err_str(w25q256_err_t err);

/* Protocol mode switching */
w25q256_err_t w25q256_set_spi_mode(void);   /* switch to 1S-1S-1S (SPI) */
w25q256_err_t w25q256_set_quad_mode(void);  /* switch to 1S-4S-4S (Quad) */

#ifdef __cplusplus
}
#endif

/* Memory-mapped direct read */
#define W25Q256_READ_U8(off)   (*(volatile uint8_t  *)(W25Q256_MEM_BASE + (off)))
#define W25Q256_READ_U32(off)  (*(volatile uint32_t *)(W25Q256_MEM_BASE + (off)))

#endif /* W25Q256_H_ */
