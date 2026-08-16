/**
 ******************************************************************************
 * @file    drw_cache.c
 * @brief   Override FSP weak d1_cacheflush / d1_cacheblockflush for DRW/Dave2D.
 *
 * The FSP r_drw_memory.c implementations are BSP_WEAK_REFERENCE stubs that
 * return immediately without any cache maintenance.  This means Dave2D's
 * AXI-bus writes to SDRAM are never made visible to the CPU or GLCDC — the
 * GPU renders correctly but nothing appears on screen.
 *
 * These overrides perform proper D-Cache clean+invalidate so that:
 *   1. CPU dirty cache lines are written back to SDRAM (clean) before
 *      Dave2D overwrites them via AXI.
 *   2. Stale cache lines are discarded (invalidate) after Dave2D finishes,
 *      so subsequent CPU/GLCDC reads go to SDRAM and see the new data.
 ******************************************************************************
 */

#include "r_drw_base.h"     /* d1_device, d1_int_t, d1_uint_t              */
#include "bsp_api.h"        /* BSP_CFG_DCACHE_ENABLED                      */

/* ---- CMSIS D-Cache intrinsics ---- */
extern void SCB_CleanInvalidateDCache(void);
extern void SCB_CleanInvalidateDCache_by_Addr(volatile void *addr,
                                              int32_t dsize);

/* ======================================================================== */
/*  Override: full cache flush                                               */
/* ======================================================================== */

d1_int_t d1_cacheflush(d1_device *handle, d1_int_t memtype)
{
    (void)handle;
    (void)memtype;

#if (BSP_CFG_DCACHE_ENABLED == 1)
    SCB_CleanInvalidateDCache();
    __DSB();
    __ISB();
#endif

    return 1;
}

/* ======================================================================== */
/*  Override: block-level cache flush                                        */
/* ======================================================================== */

d1_int_t d1_cacheblockflush(d1_device *handle, d1_int_t memtype,
                            const void *ptr, d1_uint_t size)
{
    (void)handle;
    (void)memtype;

#if (BSP_CFG_DCACHE_ENABLED == 1)
    if (ptr != NULL && size > 0) {
        SCB_CleanInvalidateDCache_by_Addr((volatile void *)ptr,
                                          (int32_t)size);
        __DSB();
    }
#endif

    return 1;
}
