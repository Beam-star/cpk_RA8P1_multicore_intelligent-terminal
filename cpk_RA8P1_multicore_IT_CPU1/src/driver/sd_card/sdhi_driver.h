/**
 ******************************************************************************
 * @file    sdhi_driver.h
 * @brief   SDHI SD卡驱动头文件 (SD Host Interface)
 *
 * 硬件: RA8P1 SDHI 模块 (SD 2.0, 4-bit, 50MHz, DMA)
 * 存储: Micro SDHC, 最大 32GB, FAT32 文件系统
 *
 * 目录结构:
 *   /MEETING/
 *   ├── 20250708_143000/
 *   │   ├── video.mjpeg      (MJPEG 视频流)
 *   │   ├── audio.wav        (PCM 音频)
 *   │   └── info.json        (会议元信息)
 *   └── ...
 ******************************************************************************
 */

#ifndef SDHI_DRIVER_H_
#define SDHI_DRIVER_H_

#include <stdint.h>
#include <stdbool.h>

/* ---- SD卡信息 ---- */
typedef struct {
    uint32_t total_mb;         /* 总容量 (MB) */
    uint32_t free_mb;          /* 剩余容量 (MB) */
    uint8_t  card_type;        /* SDSC / SDHC / SDXC */
    char     fat_type[8];      /* "FAT32" / "exFAT" */
} sd_info_t;

/* ---- 文件信息 (FAT32 目录项) ---- */
typedef struct {
    char     name[64];         /* 文件名 */
    uint32_t size_bytes;       /* 文件大小 */
    uint32_t timestamp;        /* 修改时间 (FAT 格式) */
    bool     is_directory;     /* 是否为目录 */
} sd_file_info_t;

/* ---- API ---- */

/**
 * @brief 初始化 SD 卡
 *
 * 流程:
 *   1. R_SDHI_Open() — 打开 SDHI 模块
 *   2. SD 卡检测与初始化 (CMD0→CMD8→ACMD41→CMD2→CMD3)
 *   3. 挂载 FAT32 文件系统 (f_mount)
 *   4. 创建 /MEETING/ 根目录
 */
bool sd_card_init(void);

/**
 * @brief 获取 SD 卡信息
 */
bool sd_card_get_info(sd_info_t *info);

/**
 * @brief 扫描目录获取文件列表
 * @param dir_path  目录路径 (如 "/MEETING")
 * @param files     [out] 文件信息数组
 * @param max_count 最大文件数
 * @return 实际文件数
 */
int sd_card_list_dir(const char *dir_path, sd_file_info_t *files, int max_count);

/**
 * @brief 创建新目录
 * @param path  目录路径 (如 "/MEETING/20250708_143000")
 */
bool sd_card_mkdir(const char *path);

/**
 * @brief 打开/创建文件
 * @param path  文件路径
 * @param mode  "r"=只读, "w"=写(创建), "a"=追加
 * @return 文件句柄 (NULL=失败)
 */
void *sd_card_fopen(const char *path, const char *mode);

/**
 * @brief 写入数据到文件 (通过 64KB 聚合缓冲)
 */
uint32_t sd_card_fwrite(void *file, const void *data, uint32_t size);

/**
 * @brief 从文件读取数据
 */
uint32_t sd_card_fread(void *file, void *buf, uint32_t size);

/**
 * @brief 文件内定位 (seek)
 */
bool sd_card_fseek(void *file, uint32_t offset);

/**
 * @brief 获取当前文件偏移
 */
uint32_t sd_card_ftell(void *file);

/**
 * @brief 关闭文件
 */
void sd_card_fclose(void *file);

/**
 * @brief 删除文件
 */
bool sd_card_delete(const char *path);

/**
 * @brief 强制释放 FreeRTOS+FAT 的 FAT/DIR/BUF 锁 (卡死自愈)
 *
 * FAT 锁是 IO manager 上的事件组位: 锁定=清除位+记录持有者句柄, 解锁=置位。
 * 若某个任务在持有锁期间崩溃/挂死, 锁位永远不恢复, 后续所有 SD 文件操作
 * 都会在 FF_LockFAT 里永久阻塞 (表现为"播放一半崩溃后 cancel 也无法再播")。
 * 本函数直接重置于位并清空锁持有者, 让 FAT 子系统立刻恢复可用。
 *
 * 仅在确认持有锁的任务已挂死时调用 (参见 rpmsg_record_cpu1.c 的看门狗)。
 */
void sd_card_force_unlock(void);

#endif /* SDHI_DRIVER_H_ */
