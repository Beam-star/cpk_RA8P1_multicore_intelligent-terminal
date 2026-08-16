/*
 * sdram.c - CPU1 SDRAM 驱动
 */

#include "sdram.h"
#include "rpmsg_log.h"
#include <string.h>

void sdram_fill(uint32_t offset, uint32_t size, uint8_t pattern)
{
    volatile uint8_t *dst = (volatile uint8_t *)(SDRAM_CPU1_BASE + offset);
    for (uint32_t i = 0; i < size; i++)
        dst[i] = pattern;
}

int sdram_verify(uint32_t offset, uint32_t size, uint8_t pattern)
{
    const volatile uint8_t *src = (const volatile uint8_t *)(SDRAM_CPU1_BASE + offset);
    for (uint32_t i = 0; i < size; i++)
    {
        if (src[i] != pattern)
        {
            rpmsg_log_cpu1_printf("[SDRAM] MISMATCH at 0x%08X\r\n",
                                   (unsigned)(SDRAM_CPU1_BASE + offset + i));
            return -1;
        }
    }
    return 0;
}

int sdram_test_basic(void)
{
    rpmsg_log_cpu1_printf("[SDRAM] Test 1: Basic read/write\r\n");

    volatile uint8_t  *p8  = (volatile uint8_t  *)(SDRAM_CPU1_BASE);
    volatile uint16_t *p16 = (volatile uint16_t *)(SDRAM_CPU1_BASE + 0x100);
    volatile uint32_t *p32 = (volatile uint32_t *)(SDRAM_CPU1_BASE + 0x200);

    *p8  = 0xAA;
    *p16 = 0xBBCC;
    *p32 = 0xDDEEFF00;

    uint8_t  v8  = *p8;
    uint16_t v16 = *p16;
    uint32_t v32 = *p32;

    rpmsg_log_cpu1_printf("[SDRAM]   8b:  0xAA   -> 0x%02X  %s\r\n", v8,  v8  == 0xAA        ? "OK" : "FAIL");
    rpmsg_log_cpu1_printf("[SDRAM]  16b:  0xBBCC -> 0x%04X  %s\r\n", v16, v16 == 0xBBCC      ? "OK" : "FAIL");
    rpmsg_log_cpu1_printf("[SDRAM]  32b:  0xDDEEFF00 -> 0x%08X %s\r\n", v32, v32 == 0xDDEEFF00 ? "OK" : "FAIL");

    return (v8 == 0xAA && v16 == 0xBBCC && v32 == 0xDDEEFF00) ? 0 : -1;
}

int sdram_test_sequential(void)
{
    rpmsg_log_cpu1_printf("[SDRAM] Test 2: Sequential (1MB, stride 4KB)\r\n");

    uint32_t test_size = 1024 * 1024;
    uint32_t stride = 4096;
    uint32_t errors = 0;

    for (uint32_t addr = 0; addr < test_size; addr += stride)
    {
        volatile uint32_t *p = (volatile uint32_t *)(SDRAM_CPU1_BASE + addr);
        *p = addr;
    }

    for (uint32_t addr = 0; addr < test_size; addr += stride)
    {
        volatile uint32_t *p = (volatile uint32_t *)(SDRAM_CPU1_BASE + addr);
        if (*p != addr)
        {
            rpmsg_log_cpu1_printf("[SDRAM] FAIL at 0x%08X\r\n", (unsigned)(SDRAM_CPU1_BASE + addr));
            errors++;
            if (errors >= 5) break;
        }
    }

    rpmsg_log_cpu1_printf("[SDRAM]   %u errors\r\n", (unsigned)errors);
    return (errors == 0) ? 0 : -1;
}

int sdram_test_random(void)
{
    rpmsg_log_cpu1_printf("[SDRAM] Test 3: Walking-ones\r\n");

    static const uint32_t offsets[] = { 0, 0x100000, 0x200000, 0x300000, 0x3FFFFC };
    uint32_t errors = 0;

    for (int t = 0; t < (int)(sizeof(offsets)/sizeof(offsets[0])); t++)
    {
        volatile uint32_t *base = (volatile uint32_t *)(SDRAM_CPU1_BASE + offsets[t]);

        for (int i = 0; i < 32; i++)
            base[i] = (1U << i);

        for (int i = 0; i < 32; i++)
        {
            uint32_t val = base[i];
            uint32_t exp = (1U << i);
            if (val != exp)
            {
                rpmsg_log_cpu1_printf("[SDRAM] FAIL at 0x%08X\r\n",
                                       (unsigned)(SDRAM_CPU1_BASE + offsets[t] + i*4));
                errors++;
                if (errors >= 5) break;
            }
        }
        if (errors >= 5) break;
    }

    rpmsg_log_cpu1_printf("[SDRAM]   %u errors\r\n", (unsigned)errors);
    return (errors == 0) ? 0 : -1;
}
