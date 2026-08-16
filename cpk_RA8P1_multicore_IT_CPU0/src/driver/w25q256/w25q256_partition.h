/*
 * w25q256_partition.h
 *
 * W25Q256JV 32MB Flash partition layout for CPK board RA8P1.
 *
 * All offsets and sizes are #define macros for use by the AI model loader,
 * boot animation player, LVGL image display, and PCDC flash transfer protocol.
 *
 * W25Q256 geometry: 32MB total, 4KB sectors, 64KB blocks, 256B pages.
 * All partitions fit within the first 16MB — 3-byte addressing works.
 */

#ifndef W25Q256_PARTITION_H_
#define W25Q256_PARTITION_H_

#include <stdint.h>

/* ---------- Partition Offsets & Sizes ---------- */

/* AI Model region: face detection weights + NPU command stream */
#define PART_AI_MODEL_OFFSET        0x00000000UL
#define PART_AI_MODEL_SIZE          0x00070000UL     /* 448 KB (7 × 64KB blocks) */

/* Asset directory: one 4KB sector */
#define PART_ASSET_DIR_OFFSET       0x00070000UL
#define PART_ASSET_DIR_SIZE         0x00001000UL     /* 4 KB (1 sector) */

/* LVGL pixel data: images + animation frames. Exact layout defined by asset
 * directory entries. Expanded from 7.1MB to ~10.4MB for 32MB flash. */
#define PART_LVGL_DATA_OFFSET       0x00071000UL
#define PART_LVGL_DATA_SIZE         0x00A8F000UL     /* ~10.8 MB */

/* Boot logo: full-screen 1024x600 RGB565 static image, registered in the asset
 * directory under the name "logo". Lives at the start of the LVGL data region.
 * 1024 * 600 * 2 = 1,228,800 bytes (0x12C000). Other UI assets go after it. */
#define PART_BOOT_LOGO_OFFSET       0x00071000UL
#define PART_BOOT_LOGO_SIZE         0x0012C000UL     /* 1024*600*2 = 1,228,800 B */
#define PART_UI_ASSETS_OFFSET       0x0019D000UL     /* first free offset after logo */

/* Top-strip logo: 640×120 RGB565 static image shown above the camera (layer 1
 * top strip). Registered in the asset directory under the name "toplogo". */
#define PART_TOP_LOGO_OFFSET        0x001E0000UL
#define PART_TOP_LOGO_SIZE          0x00025800UL     /* 640*120*2 = 153,600 B */

/* Animated UI frame sets (lvgl_ui_anim.c): contiguous raw RGB565 frames,
 * frame N at offset + N*(W*H*2). Loaded on demand into SDRAM double buffers. */
#define PART_MASCOT_ANIM_OFFSET     0x00210000UL
#define PART_MASCOT_ANIM_SIZE       0x00070000UL     /* 14 × 128×128×2 = 458,752 B */
#define PART_LOADING_ANIM_OFFSET    0x00290000UL
#define PART_LOADING_ANIM_SIZE      0x0005A000UL     /* 20 × 96×96×2 = 368,640 B */

/* Hand detection model (96×96×1 INT8 anchor-free detector): weights + command
 * stream.  Replaces the old face-embedding model partition. */
#define PART_HAND_MODEL_OFFSET      0x00B00000UL     /* 11 MB */
#define PART_HAND_MODEL_SIZE        0x00040000UL     /* 256 KB — weights (229,568 B) */
#define PART_HAND_CMDSTREAM_OFFSET  0x00B40000UL
#define PART_HAND_CMDSTREAM_SIZE    0x00010000UL     /* 64 KB — command stream (11,988 B) */

/* Face database: [OBSOLETE — uncalled; region reused by PART_FP_NAME_*] */
#define PART_FACE_DB_OFFSET         0x00BD0000UL
#define PART_FACE_DB_SIZE           0x0000F000UL     /* 60 KB (1 × 64KB block) */

/* Fingerprint name DB: ZW111 page_id (1..100) → participant name (English ≤31).
 * Direct-indexed table (magic + 100 × 32B = 3204 B) fits one 4KB sector.
 * 位于旧 face-embedding 分区起点，随指纹录入保存、打卡查询、清库擦除。 */
#define PART_FP_NAME_OFFSET         0x00BD0000UL
#define PART_FP_NAME_SIZE           0x00001000UL     /* 4 KB (1 sector) */

/* Test sector: kept free for w25q256_test_run() safety */
#define PART_TEST_OFFSET            0x00BDF000UL
#define PART_TEST_SIZE              0x00001000UL     /* 4 KB (1 sector) */

/* Reserved / future use: 0x00BE0000 – 0x01FFFFFF (~20.1 MB free) */

/* Total flash capacity */
#define PART_FLASH_TOTAL            0x02000000UL     /* 32 MB */

/* ---------- Asset Directory ---------- */

#define FLASH_ASSET_MAGIC           0x41535354UL     /* "ASST" */
#define FLASH_ASSET_MAX_ENTRIES     92                /* (4096-8)/44 = 92 entries per 4KB sector */

typedef struct {
    char     name[32];        /* null-terminated asset name, e.g. "boot_000"  */
    uint32_t offset;          /* absolute flash offset of the data            */
    uint32_t size;            /* data size in bytes                           */
    uint32_t crc32;           /* CRC32 of data (0 = unused)                   */
} flash_asset_entry_t;

typedef struct {
    uint32_t            magic;       /* FLASH_ASSET_MAGIC                       */
    uint32_t            count;       /* number of valid entries (0 = empty dir) */
    flash_asset_entry_t entries[FLASH_ASSET_MAX_ENTRIES];
} flash_asset_dir_t;

/* sizeof(flash_asset_dir_t) = 8 + 92×44 = 4056 bytes < 4096 (one sector) ✓ */

#endif /* W25Q256_PARTITION_H_ */
