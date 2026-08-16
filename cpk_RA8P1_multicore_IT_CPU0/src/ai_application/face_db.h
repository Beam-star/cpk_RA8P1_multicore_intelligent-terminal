/**
 ******************************************************************************
 * @file    face_db.h
 * @brief   Face-embedding database stored in W25Q256 flash.
 *
 * Each entry = 24-byte ASCII name + 128-float embedding (512 B) = 536 B.
 * The database lives in a dedicated 64 KB flash block.  On init the whole
 * block is read into a RAM working copy; on add the block is erased and
 * re-written.  This is a simple wear-levelling-free design â€?sufficient
 * for infrequent enrolments (tens to low hundreds over product lifetime).
 *
 * Matching uses cosine similarity with a configurable threshold.
 ******************************************************************************
 */

#ifndef FACE_DB_H_
#define FACE_DB_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Constants ---- */

#define FACE_DB_NAME_LEN        24          /* max name length (ASCII, no null) */
#define FACE_DB_EMBEDDING_DIM   128         /* embedding vector dimension (tiny model) */
#define FACE_DB_EMBEDDING_BYTES (FACE_DB_EMBEDDING_DIM * sizeof(float))
#define FACE_DB_ENTRY_SIZE      (FACE_DB_NAME_LEN + FACE_DB_EMBEDDING_BYTES)
#define FACE_DB_MAX_ENTRIES     32          /* fit in one 64 KB block          */

/* Cosine-similarity threshold for a "match" (0.0 â€?1.0, higher = stricter).
 * ImageNet-pretrained features need a lower threshold than dedicated face models. */
#define FACE_DB_MATCH_THRESHOLD 0.28f

/* ---- Types ---- */

/** Single database entry (in flash and in RAM). */
typedef struct {
    char   name[FACE_DB_NAME_LEN];
    float  embedding[FACE_DB_EMBEDDING_DIM];
} face_db_entry_t;

/** In-flash database header. */
typedef struct {
    uint32_t magic;                          /* FACE_DB_MAGIC                   */
    uint32_t count;                          /* number of valid entries          */
    uint32_t crc;                            /* CRC-32 of entries[] (optional)  */
    uint32_t reserved;
    face_db_entry_t entries[FACE_DB_MAX_ENTRIES];
} face_db_t;

#define FACE_DB_MAGIC  0x46414345u   /* "FACE" in little-endian */

/* ---- Public API ---- */

/**
 * @brief  Initialise the face database.
 *
 * Reads the database block from W25Q256 into the internal RAM working copy.
 * If no valid database exists (bad magic / first boot), a blank one is
 * created in RAM and written to flash.
 *
 * @return true on success, false on unrecoverable flash error.
 */
bool face_db_init(void);

/**
 * @brief  Add a face to the database and persist to flash.
 *
 * @param name       Null-terminated ASCII name (max 23 chars + null).
 * @param embedding  128-float L2-normalised embedding vector.
 * @return           Entry index (0-based) on success, -1 on error.
 */
int face_db_add(const char *name, const float *embedding);

/**
 * @brief  Find the best-matching face for an embedding.
 *
 * @param embedding  128-float L2-normalised query embedding.
 * @param similarity [out] Cosine similarity of the best match (0â€?).
 * @return           Entry index (0-based) on match, -1 if no match.
 */
int face_db_match(const float *embedding, float *similarity);

/**
 * @brief  Get the name associated with an entry index.
 *
 * @param index  Valid index (0 .. count-1).
 * @return       Pointer to the null-terminated name, or "(unknown)".
 */
const char *face_db_get_name(int index);

/**
 * @brief  Get the number of stored faces.
 */
uint32_t face_db_get_count(void);

/**
 * @brief  Clear all entries from the database and persist to flash.
 * @return 0 on success, -1 on flash error.
 */
int face_db_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* FACE_DB_H_ */
