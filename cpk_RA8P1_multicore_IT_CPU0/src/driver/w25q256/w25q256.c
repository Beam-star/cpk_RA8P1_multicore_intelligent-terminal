/*
 * w25q256.c
 *
 * W25Q256JV QSPI Flash driver for RA8P1 CPK board (via OSPI_B, unit 0, CS0).
 * Memory-mapped at 0x80000000. Defaults to SPI mode (1S-1S-1S).
 *
 * Write path uses custom 4-byte DirectTransfer (WRITE_CUSTOM=1) to work
 * around FSP R_OSPI_B_Write issues, same approach as Titan-mini w25q64 driver.
 */

#include "w25q256.h"
#include <string.h>
#include <stdio.h>

/* ---------- Internal helpers ---------- */

static fsp_err_t w25q256_direct_cmd(uint8_t cmd)
{
    spi_flash_direct_transfer_t xfer = {0};
    xfer.command        = cmd;
    xfer.command_length = 1;
    return R_OSPI_B_DirectTransfer(&g_ospi0_ctrl, &xfer,
                                   SPI_FLASH_DIRECT_TRANSFER_DIR_WRITE);
}

/* ---------- Open / Close ---------- */

static spi_flash_cfg_t g_w25q256_cfg_fix;

/*
 * Runtime patch: override page_size_bytes 64->256 in RAM copy of g_ospi0_cfg.
 * R_OSPI_B_Write validates write size against page_size_bytes. With the
 * default 64, any chunk >64 bytes fails with FSP_ERR_INVALID_SIZE.
 * W25Q256 uses 256-byte pages.
 */
static void w25q256_patch_config(void)
{
    memcpy(&g_w25q256_cfg_fix, g_ospi0_ctrl.p_cfg, sizeof(spi_flash_cfg_t));
    g_ospi0_ctrl.p_cfg = &g_w25q256_cfg_fix;
    g_w25q256_cfg_fix.page_size_bytes = 256;
}

w25q256_err_t w25q256_open(void)
{
    fsp_err_t err = R_OSPI_B_Open(&g_ospi0_ctrl, &g_ospi0_cfg);
    if (err != FSP_SUCCESS && err != FSP_ERR_ALREADY_OPEN)
    {
        return W25Q256_ERR_OPEN;
    }

    w25q256_patch_config();

    /* Start in SPI mode (1S-1S-1S) for reliable basic communication.
     * Quad mode can be enabled later via w25q256_set_quad_mode(). */
    w25q256_set_spi_mode();

    return W25Q256_OK;
}

w25q256_err_t w25q256_close(void)
{
    R_OSPI_B_Close(&g_ospi0_ctrl);
    return W25Q256_OK;
}

/* ---------- Mode Switching ---------- */

w25q256_err_t w25q256_set_spi_mode(void)
{
    memcpy(&g_w25q256_cfg_fix, &g_ospi0_cfg, sizeof(spi_flash_cfg_t));
    g_ospi0_ctrl.p_cfg = &g_w25q256_cfg_fix;
    R_OSPI_B_SpiProtocolSet(&g_ospi0_ctrl, SPI_FLASH_PROTOCOL_1S_1S_1S);
    g_w25q256_cfg_fix.write_enable_bit = 1;
    return W25Q256_OK;
}

w25q256_err_t w25q256_set_quad_mode(void)
{
    memcpy(&g_w25q256_cfg_fix, &g_ospi0_cfg, sizeof(spi_flash_cfg_t));
    g_ospi0_ctrl.p_cfg = &g_w25q256_cfg_fix;
    R_OSPI_B_SpiProtocolSet(&g_ospi0_ctrl, SPI_FLASH_PROTOCOL_1S_4S_4S);
    g_w25q256_cfg_fix.write_enable_bit = 5;
    return W25Q256_OK;
}

/* ---------- Device Info ---------- */

w25q256_err_t w25q256_read_jedec_id(w25q256_jedec_id_t *p_id)
{
    spi_flash_direct_transfer_t xfer = {0};
    xfer.command        = 0x9F;
    xfer.command_length = 1;
    xfer.data_length    = 3;

    fsp_err_t err = R_OSPI_B_DirectTransfer(&g_ospi0_ctrl, &xfer,
                                             SPI_FLASH_DIRECT_TRANSFER_DIR_READ);
    if (err != FSP_SUCCESS)
    {
        return W25Q256_ERR_READ;
    }

    p_id->manufacturer = (uint8_t)(xfer.data & 0xFF);
    p_id->memory_type  = (uint8_t)((xfer.data >> 8) & 0xFF);
    p_id->capacity     = (uint8_t)((xfer.data >> 16) & 0xFF);
    return W25Q256_OK;
}

w25q256_err_t w25q256_read_status_reg1(uint8_t *p_status)
{
    spi_flash_direct_transfer_t xfer = {0};
    xfer.command        = 0x05;
    xfer.command_length = 1;
    xfer.data_length    = 1;

    fsp_err_t err = R_OSPI_B_DirectTransfer(&g_ospi0_ctrl, &xfer,
                                             SPI_FLASH_DIRECT_TRANSFER_DIR_READ);
    if (err != FSP_SUCCESS)
    {
        return W25Q256_ERR_READ;
    }

    *p_status = (uint8_t)(xfer.data & 0xFF);
    return W25Q256_OK;
}

/* ---------- Wait Busy ---------- */

w25q256_err_t w25q256_wait_busy(void)
{
    spi_flash_status_t status;
    uint32_t timeout = W25Q256_BUSY_TIMEOUT_MS;

    while (timeout > 0)
    {
        fsp_err_t err = R_OSPI_B_StatusGet(&g_ospi0_ctrl, &status);
        if (err == FSP_SUCCESS && !status.write_in_progress)
        {
            return W25Q256_OK;
        }
        R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS);
        timeout--;
    }
    return W25Q256_ERR_BUSY_TIMEOUT;
}

/* ---------- Erase ---------- */

w25q256_err_t w25q256_erase_sector(uint32_t addr)
{
    fsp_err_t err = R_OSPI_B_Erase(&g_ospi0_ctrl,
                                    (uint8_t *)(W25Q256_MEM_BASE + addr),
                                    W25Q256_SECTOR_SIZE);
    if (err != FSP_SUCCESS)
    {
        return W25Q256_ERR_ERASE;
    }
    return w25q256_wait_busy();
}

w25q256_err_t w25q256_erase_block_64k(uint32_t addr)
{
    fsp_err_t err = R_OSPI_B_Erase(&g_ospi0_ctrl,
                                    (uint8_t *)(W25Q256_MEM_BASE + addr),
                                    W25Q256_BLOCK64K_SIZE);
    if (err != FSP_SUCCESS)
    {
        return W25Q256_ERR_ERASE;
    }
    return w25q256_wait_busy();
}

w25q256_err_t w25q256_erase_chip(void)
{
    fsp_err_t err = R_OSPI_B_Erase(&g_ospi0_ctrl,
                                    (uint8_t *)W25Q256_MEM_BASE,
                                    SPI_FLASH_ERASE_SIZE_CHIP_ERASE);
    if (err != FSP_SUCCESS)
    {
        return W25Q256_ERR_ERASE;
    }

    /* Chip erase can take up to 200 seconds for 32MB */
    uint32_t timeout = 200000;
    while (timeout > 0)
    {
        spi_flash_status_t status;
        err = R_OSPI_B_StatusGet(&g_ospi0_ctrl, &status);
        if (err == FSP_SUCCESS && !status.write_in_progress)
        {
            return W25Q256_OK;
        }
        R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS);
        timeout--;
    }
    return W25Q256_ERR_BUSY_TIMEOUT;
}

/* ---------- Write (custom 4-byte DirectTransfer, FSP workaround) ---------- */

/*
 * WRITE_CUSTOM = 1:  DirectTransfer, WREN + Page Program per 4 bytes.
 *                    Verified working on CPK (this is the path used by the
 *                    USB PCDC flash download, which flashes models/assets OK).
 *
 * WRITE_CUSTOM = 0:  R_OSPI_B_Write with 64-byte burst chunking.
 *                    Alternative path, kept for reference only.
 */
#define WRITE_CUSTOM 1

#if WRITE_CUSTOM

w25q256_err_t w25q256_write(uint32_t addr, const uint8_t *p_data, uint32_t length)
{
    if (addr + length > W25Q256_CAPACITY)
    {
        return W25Q256_ERR_ADDR_RANGE;
    }

    while (length > 0)
    {
        uint32_t page_remaining = W25Q256_PAGE_SIZE - (addr % W25Q256_PAGE_SIZE);
        uint32_t chunk = (length < page_remaining) ? length : page_remaining;

        /* 4-byte DirectTransfer: WREN -> Page Program -> wait busy */
        uint32_t writes_ok = 0, writes_fail = 0;
        for (uint32_t offset = 0; offset < chunk; offset += 4)
        {
            uint32_t remain = chunk - offset;
            uint32_t data_len = (remain < 4) ? remain : 4;

            /* Write Enable (0x06) */
            spi_flash_direct_transfer_t xfer = {0};
            xfer.command = 0x06;
            xfer.command_length = 1;
            fsp_err_t err = R_OSPI_B_DirectTransfer(&g_ospi0_ctrl, &xfer,
                                    SPI_FLASH_DIRECT_TRANSFER_DIR_WRITE);
            if (err != FSP_SUCCESS) {
                printf("[W25Q256] WREN err=%ld at offset=%lu\r\n",
                       (long)err, (unsigned long)offset);
                return W25Q256_ERR_WRITE;
            }

            /* Page Program (0x02): cmd + 3-byte addr + data */
            xfer = (spi_flash_direct_transfer_t){0};
            xfer.command        = 0x02;
            xfer.command_length = 1;
            xfer.address        = addr + offset;
            xfer.address_length = 3;
            xfer.data_length    = data_len;

            uint32_t d = 0;
            memcpy(&d, p_data + offset, data_len);
            xfer.data = d;

            err = R_OSPI_B_DirectTransfer(&g_ospi0_ctrl, &xfer,
                                          SPI_FLASH_DIRECT_TRANSFER_DIR_WRITE);
            if (err != FSP_SUCCESS) {
                printf("[W25Q256] PP err=%ld at offset=%lu\r\n",
                       (long)err, (unsigned long)offset);
                return W25Q256_ERR_WRITE;
            }

            w25q256_err_t ret = w25q256_wait_busy();
            if (ret != W25Q256_OK) {
                printf("[W25Q256] busy err=%d at offset=%lu\r\n",
                       (int)ret, (unsigned long)offset);
                writes_fail++;
            } else {
                writes_ok++;
            }
        }
        printf("[W25Q256] chunk @ 0x%08lX: %lu writes OK, %lu busy-fail\r\n",
               (unsigned long)addr, (unsigned long)writes_ok,
               (unsigned long)writes_fail);

        addr   += chunk;
        p_data += chunk;
        length -= chunk;
    }
    return W25Q256_OK;
}

#else /* !WRITE_CUSTOM: R_OSPI_B_Write path */

#define W25Q256_BURST_SIZE  64U

w25q256_err_t w25q256_write(uint32_t addr, const uint8_t *p_data, uint32_t length)
{
    if (addr + length > W25Q256_CAPACITY)
    {
        return W25Q256_ERR_ADDR_RANGE;
    }

    while (length > 0)
    {
        uint32_t page_remaining = W25Q256_PAGE_SIZE - (addr % W25Q256_PAGE_SIZE);
        uint32_t page_chunk = (length < page_remaining) ? length : page_remaining;

        while (page_chunk > 0)
        {
            uint32_t burst_chunk = (page_chunk < W25Q256_BURST_SIZE)
                                   ? page_chunk : W25Q256_BURST_SIZE;

            fsp_err_t err = R_OSPI_B_Write(&g_ospi0_ctrl,
                                            (uint8_t *)p_data,
                                            (uint8_t *)(W25Q256_MEM_BASE + addr),
                                            burst_chunk);
            if (err != FSP_SUCCESS) { return W25Q256_ERR_WRITE; }

            w25q256_err_t ret = w25q256_wait_busy();
            if (ret != W25Q256_OK) { return ret; }

            addr       += burst_chunk;
            p_data     += burst_chunk;
            length     -= burst_chunk;
            page_chunk -= burst_chunk;
        }
    }
    return W25Q256_OK;
}

#endif /* WRITE_CUSTOM */

/* ---------- Read (memory-mapped) ---------- */

w25q256_err_t w25q256_read(uint32_t addr, uint8_t *p_data, uint32_t length)
{
    if (addr + length > W25Q256_CAPACITY)
    {
        return W25Q256_ERR_ADDR_RANGE;
    }

#if 0 /* BSP_CFG_DCACHE_ENABLED — TEMP disabled to isolate HardFault */
    SCB_InvalidateDCache_by_Addr((volatile void *)(W25Q256_MEM_BASE + addr), (int32_t)length);
#endif

    printf("[W25Q] reading %lu bytes from 0x%08lX to 0x%08lX\r\n",
           (unsigned long)length, (unsigned long)(W25Q256_MEM_BASE + addr),
           (unsigned long)p_data);
    memcpy(p_data, (const void *)(W25Q256_MEM_BASE + addr), length);
    printf("[W25Q] read done\r\n");
    return W25Q256_OK;
}

/* ---------- Write + Verify ---------- */

w25q256_err_t w25q256_write_and_verify(uint32_t addr, const uint8_t *p_data, uint32_t length)
{
    w25q256_err_t ret = w25q256_write(addr, p_data, length);
    if (ret != W25Q256_OK)
    {
        return ret;
    }

    uint8_t buf[256];
    uint32_t offset = 0;

    while (offset < length)
    {
        uint32_t chunk = length - offset;
        if (chunk > sizeof(buf))
        {
            chunk = sizeof(buf);
        }

        ret = w25q256_read(addr + offset, buf, chunk);
        if (ret != W25Q256_OK)
        {
            return W25Q256_ERR_READ;
        }

        if (memcmp(buf, p_data + offset, chunk) != 0)
        {
            return W25Q256_ERR_VERIFY;
        }

        offset += chunk;
    }
    return W25Q256_OK;
}

/* ---------- Error String ---------- */

const char *w25q256_err_str(w25q256_err_t err)
{
    switch (err)
    {
        case W25Q256_OK:               return "OK";
        case W25Q256_ERR_OPEN:         return "Open failed";
        case W25Q256_ERR_ERASE:        return "Erase failed";
        case W25Q256_ERR_WRITE:        return "Write failed";
        case W25Q256_ERR_READ:         return "Read failed";
        case W25Q256_ERR_BUSY_TIMEOUT: return "Busy timeout";
        case W25Q256_ERR_VERIFY:       return "Verify mismatch";
        case W25Q256_ERR_ADDR_RANGE:   return "Address out of range";
        default:                        return "Unknown error";
    }
}
