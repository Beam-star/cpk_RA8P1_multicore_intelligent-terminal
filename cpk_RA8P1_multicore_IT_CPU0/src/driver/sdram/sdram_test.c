/*
 * sdram_test.c
 *
 * SDRAM test entry point for CPU0.
 * Tests basic read/write, sequential access, and address line integrity.
 */

#include "sdram.h"
#include <stdio.h>

/* External: SDRAM BSP init (called by g_hal_init in e2studio) */
/* Make sure BSP > SDRAM Support is set to Enabled in e2studio configuration */

int sdram_test_run(void)
{
    printf("\r\n");
    printf("========================================\r\n");
    printf("  SDRAM Test (W9812G2)\r\n");
    printf("  CPU0 (Master Core)\r\n");
    printf("========================================\r\n");
    printf("SDRAM Base: 0x%08X\r\n", (unsigned)SDRAM_BASE_ADDR);
    printf("SDRAM Size: %u MB\r\n", (unsigned)(SDRAM_SIZE / (1024*1024)));
    printf("========================================\r\n\r\n");

    int passed = 0;
    int total = 3;

    if (sdram_test_basic() == 0) passed++;
    printf("\r\n");

    if (sdram_test_sequential() == 0) passed++;
    printf("\r\n");

    if (sdram_test_random() == 0) passed++;
    printf("\r\n");

    printf("[SDRAM] Result: %d/%d passed\r\n", passed, total);

    if (passed == total)
    {
        printf("[SDRAM] ALL TESTS PASSED\r\n");
        printf("[SDRAM] SDRAM is accessible and working correctly.\r\n");
        /* Show a few bytes from SDRAM to prove it's alive */
        volatile uint32_t *p = (volatile uint32_t *)SDRAM_BASE_ADDR;
        printf("[SDRAM] First dword at 0x%08X = 0x%08X\r\n",
               (unsigned)SDRAM_BASE_ADDR, (unsigned)*p);
    }
    else
    {
        printf("[SDRAM] SOME TESTS FAILED\r\n");
        printf("[SDRAM] Check BSP SDRAM configuration in e2studio.\r\n");
        printf("[SDRAM] Ensure SDRAM Support is enabled and timings match W9825G6KH-6.\r\n");
    }

    return (passed == total) ? 0 : -1;
}
