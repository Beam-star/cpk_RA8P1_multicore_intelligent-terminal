/**
 ******************************************************************************
 * @file    rpmsg_pdm.h
 * @brief   PDM 音频数据双核共享内存结构
 *
 * 数据流:
 *   CPU1 PDM 驱动 → SDRAM pdm_shmem_t (写入) → CPU0 声源定位 (读取)
 *                   → CPU1 录制模块 (读取 Mic0 用于 WAV 编码)
 *
 * 同步机制: 单调递增 write_counter, 写入后 DSB 屏障
 * 位置:     SDRAM, RPMSG_LITE_SHMEM_BASE - 0x2000 (cam_shmem_t 前 4KB)
 *
 * 帧率:     31.25Hz (512 采样点/帧 @ 16kHz → 32ms/帧)
 ******************************************************************************
 */

#ifndef RPMSG_PDM_H_
#define RPMSG_PDM_H_

#include <stdint.h>
#include "rpmsg_core.h"

/* ---- PDM 参数 ---- */
#define PDM_SAMPLE_RATE         (16000)
#define PDM_CHANNELS            (3)        /* Mic0/Mic1/Mic2 */
#define PDM_FRAME_SIZE          (512)      /* 每帧采样点数 (32ms) */

/* ---- PDM 共享内存基址 (SDRAM, RPMsg 共享区之前 8KB) ---- */
#define PDM_SHMEM_BASE          (RPMSG_LITE_SHMEM_BASE - 0x2000UL)
#define PDM_SHMEM_SIZE          (4096)

/* ---- PDM 共享内存结构 ---- */
typedef struct {
    volatile uint32_t write_counter;    /* 写入计数器 (CPU1 递增, CPU0 轮询) */
    volatile uint32_t frame_id;         /* 帧序号 */
    volatile uint32_t timestamp_ms;     /* 时间戳 (FreeRTOS tick) */
    volatile int16_t  pcm_data[PDM_CHANNELS][PDM_FRAME_SIZE]; /* 3ch × 512 */
} pdm_shmem_t;

/**
 * @brief 获取 PDM 共享内存指针 (映射到 SDRAM 固定地址)
 */
static inline pdm_shmem_t *pdm_shmem_get(void)
{
    return (pdm_shmem_t *)PDM_SHMEM_BASE;
}

#endif /* RPMSG_PDM_H_ */
