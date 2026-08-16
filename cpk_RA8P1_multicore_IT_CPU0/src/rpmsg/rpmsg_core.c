/*
 * rpmsg_core.c - CPU0 RPMsg 实例管理（纯实例）
 *
 * 共享内存使用内部 RAM (0x220E2000, 64KB)，而非 SDRAM。
 * 这是因为 SDRAM 总线宽度在 CPK 上为 16-bit，而 Titan 用的 2MB SDRAM 方案
 * 对 CPU1（仅 1MB SDRAM）过大，且 SDRAM 数据完整性依赖正确总线配置。
 *
 * 关键：D-Cache 在 main 入口处重新开启 (SCB_EnableDCache)。
 * 必须通过 MPU 将共享内存区设为非缓存，否则 IPC RX 中断中的
 * cache invalidate 会丢弃 CPU0 尚未写回的 TX 消息（表现为 command=0 被静默丢弃）。
 *
 * 同时将 SDRAM framebuffer 区域 (0x68000000-0x687FFFFF, 8MB) 也设为 non-cacheable,
 * 因为 GLCDC 直接从 SDRAM 读取帧缓冲 (绕过 D-Cache)，CPU 写和 GLCDC 读之间需要
 * 数据一致性。使用 non-cacheable 区域意味着 CPU 写直达 SDRAM，GLCDC 始终看到最新数据。
 */

#include "rpmsg_core.h"
#include <platform/ra8p1/rpmsg_platform.h>
#include "rpmsg_lite.h"
#include "hal_data.h"
#include <stdio.h>

static struct rpmsg_lite_instance *rpmsg_instance = NULL;

void *rpmsg_core_get_instance(void)
{
    return rpmsg_instance;
}

/*
 * 用 MPU 把跨核共享区设为非缓存 (Normal Non-cacheable):
 *   0x220E0000  PDM 音频 shmem (CPU1 写)
 *   0x220E1000  cam_shmem_t (预留)
 *   0x220E2000  RPMsg vring + 缓冲 (64KB)
 *
 * 区域大小向上取整到 128KB (0x20000) 满足 ARMv8-M MPU 的对齐要求。
 */
#define SHARED_NC_BASE   (RPMSG_LITE_SHMEM_BASE - 0x2000UL)      /* 0x220E0000 */
#define SHARED_NC_SIZE   0x20000UL                                /* 128KB */
#define SHARED_NC_LIMIT  (SHARED_NC_BASE + SHARED_NC_SIZE - 1UL) /* 0x220FFFFF */
#define SHARED_NC_MPU_REGION    0U
#define SHARED_NC_MPU_ATTR_IDX  0U

/*
 * SDRAM framebuffer 区域也设为 non-cacheable,
 * GLCDC 读 SDRAM 绕过 D-Cache，CPU 写直达 SDRAM 保证数据一致。
 *
 * 0x68000000-0x687FFFFF (8MB): 覆盖 fb_background, fb_foreground,
 * VIN DMA 缓冲区, NPU tensor arena 和模型数据。
 */
#define SDRAM_NC_BASE         0x68000000UL
#define SDRAM_NC_SIZE         0x00800000UL  /* 8MB */
#define SDRAM_NC_MPU_REGION   1U
#define SDRAM_NC_MPU_ATTR_IDX 1U

static void rpmsg_shmem_mpu_setup(void)
{
    ARM_MPU_Disable();

    /* Attr 0: Normal memory, outer+inner non-cacheable (for shared memory) */
    ARM_MPU_SetMemAttr(SHARED_NC_MPU_ATTR_IDX,
                       ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE,
                                    ARM_MPU_ATTR_NON_CACHEABLE));

    /* Attr 1: Normal memory, outer+inner non-cacheable (for SDRAM framebuffer) */
    ARM_MPU_SetMemAttr(SDRAM_NC_MPU_ATTR_IDX,
                       ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE,
                                    ARM_MPU_ATTR_NON_CACHEABLE));

    /* Region 0: Internal RAM shared memory — RW, XN */
    ARM_MPU_SetRegion(SHARED_NC_MPU_REGION,
                      ARM_MPU_RBAR(SHARED_NC_BASE, ARM_MPU_SH_OUTER, 0U, 1U, 1U),
                      ARM_MPU_RLAR(SHARED_NC_LIMIT, SHARED_NC_MPU_ATTR_IDX));

    /* Region 1: SDRAM framebuffer area — RW, XN, non-cacheable so GLCDC sees CPU writes */
    ARM_MPU_SetRegion(SDRAM_NC_MPU_REGION,
                      ARM_MPU_RBAR(SDRAM_NC_BASE, ARM_MPU_SH_OUTER, 0U, 1U, 1U),
                      ARM_MPU_RLAR(SDRAM_NC_BASE + SDRAM_NC_SIZE - 1UL,
                                   SDRAM_NC_MPU_ATTR_IDX));

    /* PRIVDEFENA: 其余地址走默认内存映射 */
    ARM_MPU_Enable(MPU_CTRL_PRIVDEFENA_Msk | MPU_CTRL_HFNMIENA_Msk);

    printf("[RPMSG] MPU: 0x%08lX-0x%08lX non-cacheable (shared mem)\r\n",
           (unsigned long)SHARED_NC_BASE, (unsigned long)SHARED_NC_LIMIT);
    printf("[RPMSG] MPU: 0x%08lX-0x%08lX non-cacheable (SDRAM framebuf)\r\n",
           (unsigned long)SDRAM_NC_BASE,
           (unsigned long)(SDRAM_NC_BASE + SDRAM_NC_SIZE - 1UL));
}

void *rpmsg_core_init(void)
{
    if (rpmsg_instance != NULL) return rpmsg_instance;

    /*
     * 禁用 D-Cache 以确保 MPU 配置生效前无残留脏行。
     * main 会在 RPMsg 初始化后重新开启 (SCB_EnableDCache)，
     * 届时 MPU 已保护共享内存区域不被缓存。
     */
    SCB_DisableDCache();

    rpmsg_shmem_mpu_setup();

    rpmsg_instance = rpmsg_lite_master_init(
        (void *)RPMSG_LITE_SHMEM_BASE, SH_MEM_TOTAL_SIZE,
        RL_PLATFORM_RA8P1_LINK_ID, RL_NO_FLAGS
    );
    if (rpmsg_instance == NULL) {
        printf("[RPMSG] ERROR: master init failed\r\n");
        return NULL;
    }

    printf("[RPMSG] CPU0 initialized (IRAM @ 0x%08lX, %lu KB, %d buffers)\r\n",
           (unsigned long)RPMSG_LITE_SHMEM_BASE,
           (unsigned long)(SH_MEM_TOTAL_SIZE / 1024),
           RL_BUFFER_COUNT);
    return rpmsg_instance;
}
