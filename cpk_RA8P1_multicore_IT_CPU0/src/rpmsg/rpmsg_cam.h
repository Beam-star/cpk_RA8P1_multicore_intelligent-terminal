/*
 * rpmsg_cam.h - CPU0/CPU1 摄像头帧传输共享内存协议
 *
 * 数据流架构:
 *   CPU0 (摄像头采集)                    CPU1 (LCD 显示)
 *   ┌──────────────┐                    ┌──────────────┐
 *   │ VIN DMA      │                    │ GLCDC        │
 *   │ (OV5640→SDRAM)│                    │ (framebuffer)│
 *   │       │      │   cam_shmem_t      │       ▲      │
 *   │       ▼      │   (同步标志)        │       │      │
 *   │ D-Cache刷新  │──────────────────→│ 旋转+色彩转换│
 *   │ + 设置flag   │                    │ + 写入fb     │
 *   └──────────────┘                    └──────────────┘
 *
 * 共享内存布局 (RPMsg SDRAM 区域, 基地址 RPMSG_LITE_SHMEM_BASE, 总共 2MB):
 *   [0x000000 ~ ...]     RPMsg 协议栈 (vring, 环形缓冲区等)
 *   [...  ~ 0x1FDFFF]    RPMsg Bulk 数据区 (8KB, rpmsg_bulk.h)
 *   [0x1FF000 ~ 0x1FFFFF] 摄像头同步结构 cam_shmem_t (本文件)
 *
 * 帧数据本身不在共享内存内 — VIN DMA 直接写入 SDRAM 的 vin_image_buffer_N
 * (由 FSP 在 common_data.c 中定义, 位于 .sdram_noinit 段)
 * cam_shmem_t 仅传递帧地址指针和同步标志
 */

#ifndef RPMSG_CAM_H_
#define RPMSG_CAM_H_

#include <stdint.h>
#include "rpmsg_core.h"

/*
 * 摄像头同步结构地址 — SDRAM 中 RPMsg 区域之前的 4KB 处
 *
 * 地址: 0x69DFF000 (= 0x69E00000 - 0x1000)
 * 位于 RPMsg 共享内存区域之外, 不会被 RPMsg 协议栈覆盖
 * 注意: 此地址不能放在 RPMsg 区域内部 (0x69E00000~0x69FFFFFF),
 *       因为 RPMsg 初始化时会清零或覆写该区域
 */
#define CAM_SHMEM_ADDR  (RPMSG_LITE_SHMEM_BASE - 0x1000UL)

/* OV5640 VGA 输出尺寸 (由 camera_layer_config.h 和寄存器表 0x3808/0x380a 配置) */
#define CAM_IMAGE_WIDTH     640     /* 水平像素数 */
#define CAM_IMAGE_HEIGHT    480     /* 垂直像素数 */
#define CAM_IMAGE_BPP       2       /* RGB565: 2 字节/像素 */
#define CAM_FRAME_SIZE      (CAM_IMAGE_WIDTH * CAM_IMAGE_HEIGHT * CAM_IMAGE_BPP)  /* 614400 字节 */

/**
 * @brief CPU0/CPU1 摄像头帧同步结构 (放置在 RPMsg 共享内存中)
 *
 * 同步协议 (帧计数器方案, 避免 D-Cache 一致性问题):
 *
 *   CPU0 (生产者):
 *     1. VIN DMA 完成 → Invalidate D-Cache (VIN 缓冲区)
 *     2. 写入 frame_addr + 帧参数
 *     3. __DMB()
 *     4. 递增 frame_id (单调递增, CPU1 通过比较检测新帧)
 *     5. Clean D-Cache (共享内存区域, 推送到 SDRAM)
 *
 *   CPU1 (消费者):
 *     1. 轮询 frame_id != last_frame_id (检测新帧)
 *     2. 读取 frame_addr, 从 SDRAM 复制+旋转+转换到 framebuffer
 *     3. 记录 last_frame_id = frame_id
 *
 * 为什么用 frame_id 而不是 frame_ready 标志:
 *   frame_ready 方案需要 CPU1 写 0 清除, CPU0 写 1 设置。
 *   CPU0 的 D-Cache 可能持有旧值, 导致 CPU1 看到过期的 ready=1。
 *   frame_id 单调递增, 只有 CPU0 写, CPU1 只读, 无需清除操作。
 */
typedef struct {
    volatile uint32_t frame_id;      /* 帧序号 (CPU0 每帧递增, CPU1 比较检测新帧) */
    volatile uint32_t frame_addr;    /* 帧数据在 SDRAM 中的地址 (vin_image_buffer_N) */
    volatile uint32_t frame_width;   /* 图像宽度 (像素), 固定 640 */
    volatile uint32_t frame_height;  /* 图像高度 (像素), 固定 480 */
    volatile uint32_t frame_stride;  /* 行跨度 (字节), 固定 1280 (640×2) */
    /* ---- 人脸检测结果 (AI 坐标空间 192×192, CPU0 写入, CPU1 读取) ---- */
    volatile uint32_t detection_id;      /* 检测结果序号 (每帧递增, CPU1 比较检测新结果) */
    volatile uint32_t detection_count;   /* 检测到的人脸数量 */
    volatile int16_t  detection_x[20];   /* 检测框左上角 X (AI 空间) */
    volatile int16_t  detection_y[20];   /* 检测框左上角 Y (AI 空间) */
    volatile int16_t  detection_w[20];   /* 检测框宽度 (AI 空间) */
    volatile int16_t  detection_h[20];   /* 检测框高度 (AI 空间) */
} cam_shmem_t;

#endif /* RPMSG_CAM_H_ */
