/**
 ******************************************************************************
 * @file    face_detection_flash_prog.c
 * @brief   One-time model programming tool for W25Q256 flash
 *
 * USAGE: Set FACE_DETECT_FLASH_PROGRAMMER = 1 in face_detection_build_mode.h
 ******************************************************************************
 */
#include "face_detection_build_mode.h"

#if FACE_DETECT_FLASH_PROGRAMMER

#include "face_detection_config.h"
#include "w25q256.h"
#include "model/sub_0000_model_data.h"
#include "model/sub_0000_command_stream.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int face_detection_flash_program(void)
{
    printf("=== Face Detection Model Flash Programmer ===\r\n");

    /* 1. Open W25Q256 */
    w25q256_err_t werr = w25q256_open();
    if (werr != W25Q256_OK) {
        printf("ERROR: W25Q256 open failed: %s\r\n", w25q256_err_str(werr));
        return -1;
    }

    /* 2. Read JEDEC ID */
    w25q256_jedec_id_t id;
    werr = w25q256_read_jedec_id(&id);
    if (werr != W25Q256_OK) {
        printf("ERROR: W25Q256 read ID failed\r\n");
        return -1;
    }
    printf("W25Q256 ID: %02X %02X %02X\r\n", id.manufacturer, id.memory_type, id.capacity);

    /* 3. Calculate sizes */
    uint32_t model_size = (uint32_t)sub_0000_model_data_size;
    uint32_t cmd_size   = (uint32_t)sub_0000_command_stream_size;
    uint32_t total_size = model_size + cmd_size;

    printf("Model data size:    %lu bytes\r\n", (unsigned long)model_size);
    printf("Command stream size:%lu bytes\r\n", (unsigned long)cmd_size);
    printf("Total:              %lu bytes\r\n", (unsigned long)total_size);

    /* 4. Quick test �?also test at command stream address range */
    printf("Quick test: write 256 bytes to offset 0x100000...\r\n");
    {
        w25q256_erase_sector(0x100000);
        static uint8_t test_wr[256];
        static uint8_t test_rd[256];
        for (int i = 0; i < 256; i++) test_wr[i] = (uint8_t)(i & 0xFF);
        werr = w25q256_write(0x100000, test_wr, 256);
        w25q256_read(0x100000, test_rd, 256);
        int mismatches = 0;
        for (int i = 0; i < 256; i++) {
            if (test_rd[i] != test_wr[i]) mismatches++;
        }
        printf("Quick test @0x100000: %s (%d/256 mismatches)\r\n",
               (mismatches == 0) ? "PASS" : "FAIL", mismatches);
    }

    /* Test at command stream address range (0x67000 sector) */
    printf("Quick test: write 16 bytes to offset 0x67000...\r\n");
    {
        w25q256_erase_sector(0x67000);
        static uint8_t test_wr2[16] = {0x43, 0x4F, 0x50, 0x31, 0xAA, 0xBB, 0xCC, 0xDD,
                                        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
        static uint8_t test_rd2[16];
        werr = w25q256_write(0x670A0, test_wr2, 16);
        /* Read with +16 offset (same as model data read) */
        w25q256_read(0x670A0 + 16, test_rd2, 16);
        int mismatches2 = 0;
        for (int i = 0; i < 16; i++) {
            if (test_rd2[i] != test_wr2[i]) mismatches2++;
        }
        printf("Quick test @0x670A0 (+16 read): %s (%d/16 mismatches)\r\n",
               (mismatches2 == 0) ? "PASS" : "FAIL", mismatches2);
        printf("  Written: %02X %02X %02X %02X ...\r\n",
               test_wr2[0], test_wr2[1], test_wr2[2], test_wr2[3]);
        printf("  Read:    %02X %02X %02X %02X ...\r\n",
               test_rd2[0], test_rd2[1], test_rd2[2], test_rd2[3]);
        /* Also read without offset */
        w25q256_read(0x670A0, test_rd2, 16);
        printf("  Read (no offset): %02X %02X %02X %02X ...\r\n",
               test_rd2[0], test_rd2[1], test_rd2[2], test_rd2[3]);
    }

    /* 5. Erase flash for model data */
    printf("Erasing %lu bytes...\r\n", (unsigned long)total_size);
    uint32_t sectors = (total_size + 4095) / 4096;
    for (uint32_t i = 0; i < sectors; i++) {
        werr = w25q256_erase_sector(FACE_MODEL_FLASH_OFFSET + i * 4096);
        if (werr != W25Q256_OK) {
            printf("ERROR: Erase failed at sector %lu\r\n", (unsigned long)i);
            return -1;
        }
    }
    printf("Erased %lu sectors\r\n", (unsigned long)sectors);

    /* 6. Write model data (no offset �?read side compensates) */
    printf("Writing model data (%lu bytes)...\r\n", (unsigned long)model_size);
    werr = w25q256_write(FACE_MODEL_FLASH_OFFSET,
                        sub_0000_model_data,
                        model_size);
    if (werr != W25Q256_OK) {
        printf("ERROR: Model data write failed: %s\r\n", w25q256_err_str(werr));
        return -1;
    }

    /* 7. Verify model data */
    printf("Verifying model data...\r\n");
    {
        uint8_t flash_check[8];
        w25q256_read(0x104C0, flash_check, 8);
        printf("  Flash[0x104C0]: %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
               flash_check[0], flash_check[1], flash_check[2], flash_check[3],
               flash_check[4], flash_check[5], flash_check[6], flash_check[7]);
        /* flash[0x104C0] has model[0x104D0] = 95 C6 53 26 */
        if (flash_check[0] != 0x95) {
            printf("ERROR: Model data verification failed!\r\n");
            return -1;
        }
    }
    printf("Model data verified OK\r\n");

    /* 8. Write command stream (no offset �?read side compensates) */
    printf("Writing command stream (%lu bytes)...\r\n", (unsigned long)cmd_size);
    werr = w25q256_write(FACE_MODEL_FLASH_OFFSET + model_size,
                        sub_0000_command_stream,
                        cmd_size);
    if (werr != W25Q256_OK) {
        printf("ERROR: Command stream write failed: %s\r\n", w25q256_err_str(werr));
        return -1;
    }

    /* 9. Verify command stream �?read from exact address (no offset) */
    {
        uint8_t cs_verify[8];
        w25q256_read(FACE_MODEL_FLASH_OFFSET + model_size, cs_verify, 8);
        printf("CmdStream flash[%lX]: %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
               (unsigned long)(FACE_MODEL_FLASH_OFFSET + model_size),
               cs_verify[0], cs_verify[1], cs_verify[2], cs_verify[3],
               cs_verify[4], cs_verify[5], cs_verify[6], cs_verify[7]);
        printf("Expected:            43 4F 50 31 01 00 10 00\r\n");

        if (cs_verify[0] != 0x43 || cs_verify[1] != 0x4F ||
            cs_verify[2] != 0x50 || cs_verify[3] != 0x31) {
            printf("ERROR: Command stream verification FAILED!\r\n");
            return -1;
        }
        printf("Command stream verified OK\r\n");
    }

    printf("=== Model programmed and verified successfully! ===\r\n");
    printf("Total bytes written: %lu\r\n", (unsigned long)total_size);
    printf("\r\nNext step: Set FACE_DETECT_FLASH_PROGRAMMER = 0 and rebuild.\r\n");

    return 0;
}

#endif /* FACE_DETECT_FLASH_PROGRAMMER */
