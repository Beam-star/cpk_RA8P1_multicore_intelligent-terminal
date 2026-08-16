/*
 * rpmsg_bulk.h
 *
 * Shared memory bulk data transfer using RPMsg for notification only.
 *
 * Principle: RPMsg is used only to transmit offset+size metadata messages.
 * Actual large data resides in a dedicated shared memory region accessible
 * to both cores directly.  No mutex needed — only one writer at a time
 * (RPMsg notification synchronizes access).
 */

#ifndef RPMSG_BULK_H_
#define RPMSG_BULK_H_

#include <stdint.h>
#include "rpmsg_core.h"

/*
 * Bulk data area: 放在共享内存的末尾 8KB 区域
 *
 * SDRAM模式: 共享内存 0x69E00000 ~ 0x69FFFFFF，bulk区域在 0x69FFE000
 * RAM模式:   共享内存 0x220E2000 ~ 0x220F1FFF，bulk区域在 0x220F0000
 */
#ifdef RPMSG_USE_SDRAM
#define BULK_SHMEM_ADDR      (RPMSG_LITE_SHMEM_BASE + SH_MEM_TOTAL_SIZE - 0x2000UL)  /* 0x68FFE000 */
#else
#define BULK_SHMEM_ADDR      0x220F0000UL
#endif
#define BULK_SHMEM_SIZE      0x2000UL   /* 8KB */

/* Commands */
#define BULK_CMD_PUT         0x20       /* Writer → Reader: data ready */
#define BULK_CMD_ACK         0x21       /* Reader → Writer: read done */

/* Notification message (sent via RPMsg) */
typedef struct {
    uint32_t command;
    uint32_t offset;
    uint32_t size;
    uint32_t checksum;
} bulk_notify_t;

/* ---- Direct shared memory access (both cores) ---- */

static inline void bulk_shmem_read(uint32_t offset, void *buf, uint32_t size)
{
    const uint8_t *src = (const uint8_t *)(BULK_SHMEM_ADDR + offset);
    uint8_t *dst = (uint8_t *)buf;
    uint32_t i;
    for (i = 0; i < size; i++) dst[i] = src[i];
}

static inline void bulk_shmem_write(uint32_t offset, const void *data, uint32_t size)
{
    uint8_t *dst = (uint8_t *)(BULK_SHMEM_ADDR + offset);
    const uint8_t *src = (const uint8_t *)data;
    uint32_t i;
    for (i = 0; i < size; i++) dst[i] = src[i];
}

static inline uint32_t bulk_shmem_fill(uint32_t offset, uint32_t size, uint8_t seed)
{
    uint8_t *dst = (uint8_t *)(BULK_SHMEM_ADDR + offset);
    uint32_t checksum = 0;
    uint32_t i;
    for (i = 0; i < size; i++) {
        uint8_t v = (uint8_t)(seed + i);
        dst[i] = v;
        checksum ^= (uint32_t)v;
    }
    return checksum;
}

static inline int bulk_shmem_verify(uint32_t offset, uint32_t size, uint8_t seed, uint32_t expected)
{
    const uint8_t *src = (const uint8_t *)(BULK_SHMEM_ADDR + offset);
    uint32_t checksum = 0;
    uint32_t i;
    for (i = 0; i < size; i++) {
        uint8_t v = src[i];
        checksum ^= (uint32_t)v;
        if (v != (uint8_t)(seed + i)) return 0;
    }
    return (checksum == expected) ? 1 : 0;
}

#endif /* RPMSG_BULK_H_ */
