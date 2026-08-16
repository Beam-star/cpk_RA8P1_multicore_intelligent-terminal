/*
 * w25q256_test.c
 *
 * W25Q256 flash functional test and speed benchmark for CPU0 (CPK board).
 */

#include "w25q256.h"
#include "w25q256_test.h"
#include "perf_counter/perf_counter.h"
#include <stdio.h>
#include <string.h>

/* ---------- Functional Test ---------- */

#define TEST_SECTOR_ADDR   0x00BDF000U   /* last partition test sector */

static int test_pass;
static int test_fail;

static void test_check(const char *name, w25q256_err_t expected, w25q256_err_t actual)
{
    if (actual == expected) {
        printf("[W25Q256] PASS: %s\r\n", name);
        test_pass++;
    } else {
        printf("[W25Q256] FAIL: %s (expected %s, got %s)\r\n",
               name, w25q256_err_str(expected), w25q256_err_str(actual));
        test_fail++;
    }
}

int w25q256_test_run(void)
{
    test_pass = 0;
    test_fail = 0;

    printf("\r\n========================================\r\n");
    printf("  W25Q256 Functional Test\r\n");
    printf("  Test sector: 0x%08X\r\n", (unsigned)(W25Q256_MEM_BASE + TEST_SECTOR_ADDR));
    printf("========================================\r\n\r\n");

    test_check("Open", W25Q256_OK, w25q256_open());

    w25q256_jedec_id_t id = {0};
    w25q256_err_t ret = w25q256_read_jedec_id(&id);
    printf("[W25Q256]   JEDEC ID: %02X %02X %02X\r\n", id.manufacturer, id.memory_type, id.capacity);
    if (ret == W25Q256_OK &&
        id.manufacturer == W25Q256_JEDEC_MFR &&
        id.memory_type  == W25Q256_JEDEC_TYPE &&
        id.capacity     == W25Q256_JEDEC_CAPACITY)
        test_check("JEDEC ID (EF 40 19)", W25Q256_OK, W25Q256_OK);
    else
        test_check("JEDEC ID", W25Q256_OK, ret);

    uint8_t sr1 = 0;
    ret = w25q256_read_status_reg1(&sr1);
    printf("[W25Q256]   SR1: 0x%02X\r\n", sr1);
    test_check("Read SR1", W25Q256_OK, ret);

    ret = w25q256_erase_sector(TEST_SECTOR_ADDR);
    test_check("Erase sector", W25Q256_OK, ret);

    if (ret == W25Q256_OK) {
        uint8_t buf[64];
        w25q256_read(TEST_SECTOR_ADDR, buf, sizeof(buf));
        bool all_ff = true;
        for (uint32_t i = 0; i < sizeof(buf); i++)
            if (buf[i] != 0xFF) { all_ff = false; break; }
        test_check("Verify erased (0xFF)", W25Q256_OK, all_ff ? W25Q256_OK : W25Q256_ERR_VERIFY);
    }

    uint8_t wr[256], rd[256];
    for (uint32_t i = 0; i < 256; i++) wr[i] = (uint8_t)(i & 0xFF);
    ret = w25q256_write(TEST_SECTOR_ADDR, wr, 256);
    test_check("Write 256B", W25Q256_OK, ret);
    if (ret == W25Q256_OK) {
        w25q256_read(TEST_SECTOR_ADDR, rd, 256);
        test_check("Read-back match", W25Q256_OK, (memcmp(wr, rd, 256) == 0) ? W25Q256_OK : W25Q256_ERR_VERIFY);
    }

    uint8_t pattern[128];
    for (uint32_t i = 0; i < 128; i++) pattern[i] = (uint8_t)(0xA5 + i);
    w25q256_erase_sector(TEST_SECTOR_ADDR);
    test_check("write_and_verify (128B)", W25Q256_OK,
               w25q256_write_and_verify(TEST_SECTOR_ADDR + 256, pattern, 128));

    w25q256_erase_sector(TEST_SECTOR_ADDR);

    int total = test_pass + test_fail;
    printf("\r\n========================================\r\n");
    printf("[W25Q256] Result: %d/%d passed\r\n", test_pass, total);
    printf("[W25Q256] %s\r\n", (test_fail == 0) ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    printf("========================================\r\n\r\n");
    return (test_fail == 0) ? 0 : -1;
}

/* ---------- Speed Benchmark ---------- */

#define SPEED_SIZE       0x400000U   /* 4MB */
#define SPEED_CHUNK      4096U

static void speed_wait_wip(void)
{
    spi_flash_status_t status;
    uint32_t timeout = UINT32_MAX;
    do {
        R_OSPI_B_StatusGet(&g_ospi0_ctrl, &status);
    } while (status.write_in_progress && (--timeout > 0));
}

static void speed_erase_region(uint32_t size)
{
    printf("[SPEED] Erasing %uMB...\r\n", (unsigned)(size / (1024 * 1024)));
    uint32_t ms = (uint32_t)get_system_ms();
    for (uint32_t addr = 0; addr < size; addr += W25Q256_BLOCK32K_SIZE) {
        fsp_err_t err = R_OSPI_B_Erase(&g_ospi0_ctrl,
                                        (uint8_t *)(W25Q256_MEM_BASE + addr),
                                        W25Q256_BLOCK32K_SIZE);
        if (err != FSP_SUCCESS) {
            printf("[SPEED] Erase err=%d at 0x%08X\r\n", (int)err, (unsigned)addr);
            return;
        }
        speed_wait_wip();
    }
    ms = (uint32_t)get_system_ms() - ms;
    printf("[SPEED] Erase done: %u s\r\n\r\n", (unsigned)(ms / 1000));
}

static void speed_bench_rw(const char *mode)
{
    static uint8_t __attribute__((aligned(8))) buf[SPEED_CHUNK];
    for (uint32_t i = 0; i < SPEED_CHUNK; i++) buf[i] = (uint8_t)(i & 0xFF);

    FSP_CRITICAL_SECTION_DEFINE;
    uint32_t ms;
    fsp_err_t err;

    /* Write */
    printf("[SPEED] %s write...\r\n", mode);
    ms = (uint32_t)get_system_ms();
    uint8_t *dest = (uint8_t *)W25Q256_MEM_BASE;
    for (uint32_t i = 0; i < SPEED_SIZE / SPEED_CHUNK; i++) {
        FSP_CRITICAL_SECTION_ENTER;
        err = R_OSPI_B_Write(&g_ospi0_ctrl, buf, dest, SPEED_CHUNK);
        FSP_CRITICAL_SECTION_EXIT;
        if (err != FSP_SUCCESS) {
            printf("[SPEED] %s write err=%d at 0x%08X\r\n",
                   mode, (int)err, (unsigned)(uint32_t)dest);
            return;
        }
        speed_wait_wip();
        dest += SPEED_CHUNK;
    }
    ms = (uint32_t)get_system_ms() - ms;
    printf("[SPEED] %s write: %u ms, %u KB/s\r\n", mode, (unsigned)ms,
           (unsigned)((SPEED_SIZE / 1024) * 1000 / ms));

    /* Read */
#if BSP_CFG_DCACHE_ENABLED
    SCB_InvalidateDCache_by_Addr((volatile void *)W25Q256_MEM_BASE, SPEED_SIZE);
#endif
    volatile uint32_t dummy;
    printf("[SPEED] %s read...\r\n", mode);
    volatile uint8_t *src = (volatile uint8_t *)W25Q256_MEM_BASE;
    ms = (uint32_t)get_system_ms();
    for (uint32_t i = 0; i < SPEED_SIZE / sizeof(uint32_t); i++) {
        dummy = *(volatile uint32_t *)src;
        src += sizeof(uint32_t);
    }
    (void)dummy;
    ms = (uint32_t)get_system_ms() - ms;
    printf("[SPEED] %s read: %u ms, %u KB/s\r\n", mode, (unsigned)ms,
           (unsigned)((SPEED_SIZE / 1024) * 1000 / ms));
}

int w25q256_test_speed(void)
{
    printf("\r\n========================================\r\n");
    printf("  W25Q256 Speed Benchmark\r\n");
    printf("  Test size: %u MB\r\n", (unsigned)(SPEED_SIZE / (1024 * 1024)));
    printf("========================================\r\n\r\n");

    w25q256_err_t ret = w25q256_open();
    if (ret != W25Q256_OK) {
        printf("[SPEED] Open failed: %s\r\n", w25q256_err_str(ret));
        return -1;
    }

    /* --- 1S-1S-1S --- */
    printf("[SPEED] --- 1S-1S-1S (SPI) ---\r\n");
    w25q256_set_spi_mode();
    speed_erase_region(SPEED_SIZE);
    speed_bench_rw("1S-1S-1S");
    printf("\r\n");

    /* --- 1S-4S-4S --- */
    printf("[SPEED] --- 1S-4S-4S (Quad) ---\r\n");
    w25q256_set_quad_mode();
    speed_erase_region(SPEED_SIZE);
    speed_bench_rw("1S-4S-4S");
    printf("\r\n");

    /* Restore Quad mode */
    w25q256_set_quad_mode();

    printf("========================================\r\n");
    printf("[SPEED] Benchmark complete.\r\n");
    printf("========================================\r\n\r\n");
    return 0;
}
