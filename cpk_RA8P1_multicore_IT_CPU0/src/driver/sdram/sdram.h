/*
 * sdram.h
 *
 * W9812G2 SDRAM driver for RA8P1 CPK board
 * Base address: 0x68000000, Size: 16MB (0x01000000), 32-bit data bus
 */

#ifndef SDRAM_H_
#define SDRAM_H_

#include <stdint.h>

/* W9812G2 SDRAM memory map (RA8P1 CPK board) */
#define SDRAM_BASE_ADDR       0x68000000UL
#define SDRAM_SIZE            0x01000000UL   /* 16MB */
#define SDRAM_END_ADDR        (SDRAM_BASE_ADDR + SDRAM_SIZE - 1)

/* CPU0 allocated region: 15MB (0xF00000) */
#define SDRAM_CPU0_BASE       SDRAM_BASE_ADDR
#define SDRAM_CPU0_SIZE       0x00F00000UL

/* CPU1 allocated region: 1MB (0x100000) */
#define SDRAM_CPU1_BASE       (SDRAM_BASE_ADDR + SDRAM_CPU0_SIZE)
#define SDRAM_CPU1_SIZE       0x00100000UL

/* Direct memory access macros */
#define SDRAM_U8(addr)   (*(volatile uint8_t  *)(addr))
#define SDRAM_U16(addr)  (*(volatile uint16_t *)(addr))
#define SDRAM_U32(addr)  (*(volatile uint32_t *)(addr))

/* API functions */
int  sdram_test_basic(void);
int  sdram_test_sequential(void);
int  sdram_test_random(void);
void sdram_fill(uint32_t offset, uint32_t size, uint8_t pattern);
int  sdram_verify(uint32_t offset, uint32_t size, uint8_t pattern);

#endif /* SDRAM_H_ */
