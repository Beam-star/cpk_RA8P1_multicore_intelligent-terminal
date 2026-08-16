/*
 * sdram_test.c - CPU1 SDRAM 读写测试
 */

#include "sdram.h"
#include "rpmsg_log.h"
#include <stdio.h>

int sdram_test_run(void)
{
    rpmsg_log_cpu1_printf("\r\n");
    rpmsg_log_cpu1_printf("========================================\r\n");
    rpmsg_log_cpu1_printf("  SDRAM Test (CPU1 Remote)\r\n");
    rpmsg_log_cpu1_printf("========================================\r\n");
    rpmsg_log_cpu1_printf("SDRAM Base: 0x%08X\r\n", (unsigned)SDRAM_CPU1_BASE);
    rpmsg_log_cpu1_printf("SDRAM Size: %u MB\r\n", (unsigned)(SDRAM_SIZE / (1024*1024)));
    rpmsg_log_cpu1_printf("========================================\r\n");

    int passed = 0;
    int total = 3;

    if (sdram_test_basic() == 0) passed++;
    if (sdram_test_sequential() == 0) passed++;
    if (sdram_test_random() == 0) passed++;

    rpmsg_log_cpu1_printf("[SDRAM] Result: %d/%d passed\r\n", passed, total);

    if (passed == total)
    {
        rpmsg_log_cpu1_printf("[SDRAM] ALL TESTS PASSED\r\n");
        rpmsg_log_cpu1_printf("[SDRAM] CPU1 CAN access SDRAM.\r\n");
        volatile uint32_t *p = (volatile uint32_t *)SDRAM_CPU1_BASE;
        rpmsg_log_cpu1_printf("[SDRAM] dword at 0x%08X = 0x%08X\r\n",
                              (unsigned)SDRAM_CPU1_BASE, (unsigned)*p);
    }
    else
    {
        rpmsg_log_cpu1_printf("[SDRAM] SOME TESTS FAILED\r\n");
    }

    return (passed == total) ? 0 : -1;
}
