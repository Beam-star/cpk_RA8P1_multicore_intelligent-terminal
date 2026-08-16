/**
 ******************************************************************************
 * @file    face_db.c
 * @brief   Face-embedding database in W25Q256 flash �?see face_db.h.
 *
 * Stores up to 32 face entries in a dedicated 64 KB flash block.
 * Entire block is cached in RAM; writes are erase-block-rewrite.
 ******************************************************************************
 */

#include "face_db.h"
#include "../driver/w25q256/w25q256.h"
#include "../driver/w25q256/w25q256_partition.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ---- Local state ---- */

static face_db_t  g_db;                  /* RAM working copy               */
static bool       g_db_ready = false;    /* true after successful init    */

/* Forward declaration */
extern void SCB_CleanDCache_by_Addr(volatile void *addr, int32_t dsize);

/* ---- Internal helpers ---- */

/**
 * Erase the 64 KB flash block that holds the face database,
 * then write the RAM copy back.  Returns 0 on success.
 */
static int face_db_flush(void)
{
    w25q256_err_t werr;

    /*
     * Re-open the flash controller before erase/write.  After boot_logo
     * used XIP (memory-mapped reads), the OSPI_B may be in a state where
     * direct commands don't work reliably.  Close + re-open resets it.
     */
    w25q256_close();
    werr = w25q256_open();
    if (werr != W25Q256_OK) {
        printf("[FACE_DB] Re-open failed: %s\r\n", w25q256_err_str(werr));
        return -1;
    }

    printf("[FACE_DB] Erasing 64KB block @0x%06lX...\r\n",
           (unsigned long)PART_FACE_DB_OFFSET);
    werr = w25q256_erase_block_64k(PART_FACE_DB_OFFSET);
    if (werr != W25Q256_OK) {
        printf("[FACE_DB] Erase failed: %s\r\n", w25q256_err_str(werr));
        return -1;
    }
    printf("[FACE_DB] Erase done\r\n");

    /*
     * The write path uses 4-byte-at-a-time DirectTransfer (see W25Q256
     * README).  This is slow (~10�?0 KB/s) but reliable.
     * sizeof(face_db_t) �?17 KB �?~1�? seconds.
     */
    uint32_t bytes_to_write = sizeof(face_db_t);
    printf("[FACE_DB] Writing %lu bytes...\r\n", (unsigned long)bytes_to_write);
    werr = w25q256_write(PART_FACE_DB_OFFSET, (const uint8_t *)&g_db, bytes_to_write);
    if (werr != W25Q256_OK) {
        printf("[FACE_DB] Write failed: %s\r\n", w25q256_err_str(werr));
        return -1;
    }

    printf("[FACE_DB] Flushed %lu bytes to flash @0x%06lX\r\n",
           (unsigned long)bytes_to_write, (unsigned long)PART_FACE_DB_OFFSET);
    return 0;
}

/** Cosine similarity between two L2-normalised 128-d vectors. */
static float cosine_similarity(const float *a, const float *b)
{
    float dot = 0.0f;
    for (int i = 0; i < FACE_DB_EMBEDDING_DIM; i++) {
        dot += a[i] * b[i];
    }
    /* Vectors assumed already L2-normalised �?dot = cosine */
    if (dot > 1.0f) dot = 1.0f;
    if (dot < -1.0f) dot = -1.0f;
    return (dot + 1.0f) * 0.5f;  /* map [-1,1] �?[0,1] */
}

/* ---- Public API ---- */

bool face_db_init(void)
{
    w25q256_err_t werr;

    werr = w25q256_open();
    if (werr != W25Q256_OK) {
        printf("[FACE_DB] W25Q256 open failed: %s\r\n", w25q256_err_str(werr));
        return false;
    }

    /* Read the entire database block from flash */
    werr = w25q256_read(PART_FACE_DB_OFFSET, (uint8_t *)&g_db, sizeof(g_db));
    if (werr != W25Q256_OK) {
        printf("[FACE_DB] Read failed: %s\r\n", w25q256_err_str(werr));
        return false;
    }

    /* Validate */
    if (g_db.magic == FACE_DB_MAGIC && g_db.count <= FACE_DB_MAX_ENTRIES) {
        printf("[FACE_DB] Loaded: %lu entries\r\n", (unsigned long)g_db.count);
        for (uint32_t i = 0; i < g_db.count; i++) {
            printf("[FACE_DB]   [%lu] %s\r\n", (unsigned long)i, g_db.entries[i].name);
        }
    } else {
        /* First boot or corrupted �?initialise blank database */
        printf("[FACE_DB] No valid DB (magic=0x%08lX) �?creating blank\r\n",
               (unsigned long)g_db.magic);
        memset(&g_db, 0, sizeof(g_db));
        g_db.magic = FACE_DB_MAGIC;
        g_db.count = 0;
        g_db.crc   = 0;

        if (face_db_flush() != 0) {
            printf("[FACE_DB] Initial write failed\r\n");
            return false;
        }
    }

    g_db_ready = true;
    return true;
}

int face_db_add(const char *name, const float *embedding)
{
    if (!g_db_ready) {
        printf("[FACE_DB] Not ready\r\n");
        return -1;
    }
    if (g_db.count >= FACE_DB_MAX_ENTRIES) {
        printf("[FACE_DB] Database full (%u entries)\r\n", (unsigned)FACE_DB_MAX_ENTRIES);
        return -1;
    }

    face_db_entry_t *e = &g_db.entries[g_db.count];

    /* Copy name */
    strncpy(e->name, name, FACE_DB_NAME_LEN - 1);
    e->name[FACE_DB_NAME_LEN - 1] = '\0';

    /* Copy embedding */
    memcpy(e->embedding, embedding, FACE_DB_EMBEDDING_BYTES);

    g_db.count++;

    int ret = face_db_flush();
    if (ret != 0) {
        g_db.count--;  /* roll back */
        return -1;
    }

    printf("[FACE_DB] Added entry %lu: \"%s\"\r\n",
           (unsigned long)(g_db.count - 1), e->name);
    return (int)(g_db.count - 1);
}

int face_db_match(const float *embedding, float *similarity)
{
    if (!g_db_ready || g_db.count == 0) {
        if (similarity) *similarity = 0.0f;
        return -1;
    }

    float best_sim = 0.0f;
    int   best_idx = -1;

    for (uint32_t i = 0; i < g_db.count; i++) {
        float sim = cosine_similarity(embedding, g_db.entries[i].embedding);
        if (sim > best_sim) {
            best_sim = sim;
            best_idx = (int)i;
        }
    }

    if (similarity) *similarity = best_sim;

    if (best_sim >= FACE_DB_MATCH_THRESHOLD) {
        return best_idx;
    }
    return -1;  /* no match */
}

const char *face_db_get_name(int index)
{
    if (!g_db_ready || index < 0 || (uint32_t)index >= g_db.count) {
        return "(unknown)";
    }
    return g_db.entries[index].name;
}

uint32_t face_db_get_count(void)
{
    return g_db_ready ? g_db.count : 0;
}

int face_db_clear(void)
{
    if (!g_db_ready) {
        printf("[FACE_DB] Not ready\r\n");
        return -1;
    }
    g_db.count = 0;
    memset(g_db.entries, 0, sizeof(g_db.entries));
    int ret = face_db_flush();
    if (ret == 0) {
        printf("[FACE_DB] All entries cleared\r\n");
    }
    return ret;
}
