/*
 * sdram.c
 *
 * W9812G2 SDRAM driver - direct memory access implementation
 */

#include "sdram.h"
#include <stdio.h>
#include <string.h>

/* Fill a region of SDRAM with a pattern */
void sdram_fill(uint32_t offset, uint32_t size, uint8_t pattern)
{
    volatile uint8_t *dst = (volatile uint8_t *)(SDRAM_BASE_ADDR + offset);
    for (uint32_t i = 0; i < size; i++)
    {
        dst[i] = pattern;
    }
}

/* Verify a region of SDRAM matches the expected pattern */
int sdram_verify(uint32_t offset, uint32_t size, uint8_t pattern)
{
    const volatile uint8_t *src = (const volatile uint8_t *)(SDRAM_BASE_ADDR + offset);
    for (uint32_t i = 0; i < size; i++)
    {
        if (src[i] != pattern)
        {
            printf("[SDRAM] MISMATCH at offset 0x%08X: expected 0x%02X got 0x%02X\r\n",
                   (unsigned)(SDRAM_BASE_ADDR + offset + i),
                   (unsigned)pattern, (unsigned)src[i]);
            return -1;
        }
    }
    return 0;
}

/* Test 1: Basic byte/word/dword read/write at fixed address */
int sdram_test_basic(void)
{
    printf("[SDRAM] Test 1: Basic read/write\r\n");

    volatile uint8_t  *p8  = (volatile uint8_t  *)(SDRAM_BASE_ADDR);
    volatile uint16_t *p16 = (volatile uint16_t *)(SDRAM_BASE_ADDR + 0x100);
    volatile uint32_t *p32 = (volatile uint32_t *)(SDRAM_BASE_ADDR + 0x200);

    /* Write */
    *p8  = 0xAA;
    *p16 = 0xBBCC;
    *p32 = 0xDDEEFF00;

    /* Read back */
    uint8_t  v8  = *p8;
    uint16_t v16 = *p16;
    uint32_t v32 = *p32;

    printf("[SDRAM]   8-bit:  wrote 0xAA   read 0x%02X  %s\r\n", v8,  v8  == 0xAA        ? "OK" : "FAIL");
    printf("[SDRAM]  16-bit:  wrote 0xBBCC read 0x%04X  %s\r\n", v16, v16 == 0xBBCC      ? "OK" : "FAIL");
    printf("[SDRAM]  32-bit:  wrote 0xDDEEFF00 read 0x%08X %s\r\n", v32, v32 == 0xDDEEFF00 ? "OK" : "FAIL");

    return (v8 == 0xAA && v16 == 0xBBCC && v32 == 0xDDEEFF00) ? 0 : -1;
}

/* Test 2: Sequential write/read across 1MB (stride 4KB) */
int sdram_test_sequential(void)
{
    printf("[SDRAM] Test 2: Sequential write/read (1MB, stride 4KB)\r\n");

    uint32_t test_size = 1024 * 1024; /* 1MB */
    uint32_t stride = 4096;
    uint32_t errors = 0;

    /* Write */
    for (uint32_t addr = 0; addr < test_size; addr += stride)
    {
        volatile uint32_t *p = (volatile uint32_t *)(SDRAM_BASE_ADDR + addr);
        *p = addr;
    }

    /* Read back */
    for (uint32_t addr = 0; addr < test_size; addr += stride)
    {
        volatile uint32_t *p = (volatile uint32_t *)(SDRAM_BASE_ADDR + addr);
        uint32_t val = *p;
        if (val != addr)
        {
            printf("[SDRAM]   FAIL at 0x%08X: expected 0x%08X got 0x%08X\r\n",
                   (unsigned)(SDRAM_BASE_ADDR + addr), (unsigned)addr, (unsigned)val);
            errors++;
            if (errors >= 5) break;
        }
    }

    printf("[SDRAM]   Result: %u errors\r\n", (unsigned)errors);
    return (errors == 0) ? 0 : -1;
}

/* Test 3: Address line test (walking ones) at various locations */
int sdram_test_random(void)
{
    printf("[SDRAM] Test 3: Address walking-ones test\r\n");

    static const uint32_t offsets[] = { 0, 0x100000, 0x200000, 0x300000, 0x3FFFFC };
    uint32_t errors = 0;

    for (int t = 0; t < (int)(sizeof(offsets)/sizeof(offsets[0])); t++)
    {
        volatile uint32_t *base = (volatile uint32_t *)(SDRAM_BASE_ADDR + offsets[t]);

        /* Write walking ones */
        for (int i = 0; i < 32; i++)
        {
            base[i] = (1U << i);
        }

        /* Read back */
        for (int i = 0; i < 32; i++)
        {
            uint32_t val = base[i];
            uint32_t exp = (1U << i);
            if (val != exp)
            {
                printf("[SDRAM]   FAIL at 0x%08X: expected 0x%08X got 0x%08X\r\n",
                       (unsigned)(SDRAM_BASE_ADDR + offsets[t] + i*4), (unsigned)exp, (unsigned)val);
                errors++;
                if (errors >= 5) break;
            }
        }
        if (errors >= 5) break;
    }

    printf("[SDRAM]   Result: %u errors\r\n", (unsigned)errors);
    return (errors == 0) ? 0 : -1;
}
