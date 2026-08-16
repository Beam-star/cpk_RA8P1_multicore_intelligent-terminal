/**
 ******************************************************************************
 * @file    sdhi_driver.c
 * @brief   SDHI SD卡驱动 — FreeRTOS+Fat 文件系统
 *
 * FSP 栈: SDMMC → rm_block_media_sdmmc → rm_freertos_plus_fat → FreeRTOS+FAT
 *
 * 目录结构:
 *   /meeting/
 *   ├── audio/          ← WAV 录音文件
 *   ├── video/          ← 视频占位符 (.avi_dummy)
 *   └── sounds/         ← 按钮提示音 (.wav)
 ******************************************************************************
 */

#include "sdhi_driver.h"
#include "hal_data.h"
#include "common_data.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "ff_stdio.h"
#include "ff_headers.h"
#include "ff_file.h"
#include "event_groups.h"
#include <stdio.h>
#include <string.h>

/* ---- 聚合写缓冲区 (64KB SDRAM) ---- */
#define SD_WRITE_BUF_SIZE   (64 * 1024)
static uint8_t g_sd_write_buf[SD_WRITE_BUF_SIZE] __attribute__((section(".sdram_noinit")));
static uint32_t g_sd_write_buf_pos = 0;

/* ---- 运行时状态 ---- */
static bool              g_sd_ready   = false;
static FF_Disk_t        *g_sd_disk    = NULL;
static SemaphoreHandle_t g_sd_mutex   = NULL;

/* ======================================================================== */
/*  内部: 聚合缓冲 flush                                                     */
/* ======================================================================== */

static void flush_write_buf(FF_FILE *fp)
{
    if (g_sd_write_buf_pos > 0 && fp) {
        ff_fwrite(g_sd_write_buf, 1, g_sd_write_buf_pos, fp);
        g_sd_write_buf_pos = 0;
    }
}

/* ======================================================================== */
/*  内部: 设备忙等待回调                                                     */
/* ======================================================================== */

static fsp_err_t sd_busy_callback(void *ctx)
{
    (void)ctx;
    /* FreeRTOS+Fat 内部会轮询; 返回 FSP_SUCCESS 表示继续等待 */
    return FSP_SUCCESS;
}

/* ======================================================================== */
/*  Public API                                                               */
/* ======================================================================== */

bool sd_card_init(void)
{
    fsp_err_t err;
    printf("[SD CARD] Initializing SDHI + FreeRTOS+FAT...\r\n");

    g_sd_mutex = xSemaphoreCreateMutex();
    if (!g_sd_mutex) {
        printf("[SD CARD] Mutex create failed\r\n");
        return false;
    }

    /* 1. Open FreeRTOS+FAT module */
    err = g_rm_freertos_plus_fat0.p_api->open(
            g_rm_freertos_plus_fat0.p_ctrl,
            g_rm_freertos_plus_fat0.p_cfg);
    if (err != FSP_SUCCESS) {
        printf("[SD CARD] FAT open failed: %ld\r\n", (long)err);
        return false;
    }

    /* 2. Initialize media (probes SD card, gets sector count/size) */
    rm_freertos_plus_fat_device_t device = {0};
    err = g_rm_freertos_plus_fat0.p_api->mediaInit(
            g_rm_freertos_plus_fat0.p_ctrl, &device);
    if (err != FSP_SUCCESS) {
        printf("[SD CARD] Media init failed: %ld (check SD card!)\r\n", (long)err);
        return false;
    }
    printf("[SD CARD] Media: %lu sectors × %lu bytes = %lu MB\r\n",
           (unsigned long)device.sector_count,
           (unsigned long)device.sector_size_bytes,
           (unsigned long)((uint64_t)device.sector_count *
                            device.sector_size_bytes / 1024 / 1024));

    /* 3. Initialize FAT disk on this media */
    static FF_Disk_t disk;
    g_rm_freertos_plus_fat0_disk_cfg.device.sector_count      = device.sector_count;
    g_rm_freertos_plus_fat0_disk_cfg.device.sector_size_bytes = device.sector_size_bytes;
    err = g_rm_freertos_plus_fat0.p_api->diskInit(
            g_rm_freertos_plus_fat0.p_ctrl,
            &g_rm_freertos_plus_fat0_disk_cfg, &disk);
    if (err != FSP_SUCCESS) {
        printf("[SD CARD] Disk init failed: %ld\r\n", (long)err);
        return false;
    }

    /* 4. Mount the FAT partition.
     *    关键: FSP 的 DiskInit 只创建 IO manager, 并不挂载分区!
     *    漏掉 FF_Mount 时 FF_FS_Add 照样"成功", 但之后所有文件操作
     *    都返回 ENXIO(6)/EBADF(9)。(FSP 示例: DiskInit → FF_Mount → FF_FS_Add) */
    FF_Error_t ff_err = FF_Mount(&disk, disk.xStatus.bPartitionNumber);
    if (FF_isERR(ff_err)) {
        printf("[SD CARD] FF_Mount failed: 0x%08lX\r\n", (unsigned long)ff_err);
        return false;
    }
    printf("[SD CARD] Partition %d mounted\r\n", (int)disk.xStatus.bPartitionNumber);

    /* 5. Add to FreeRTOS+FAT virtual filesystem
     *    FF_FS_Add returns pdTRUE (1) on success, pdFALSE (0) on failure */
    if (FF_FS_Add("/", &disk) == pdFALSE) {
        printf("[SD CARD] FF_FS_Add failed (table full?)\r\n");
        return false;
    }

    /* 6. Create working directories (EEXIST=17 是正常的) */
    static const char *dirs[] = {
        "/meeting", "/meeting/audio", "/meeting/video", "/meeting/sounds",
        "/meeting/notes"
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        int r = ff_mkdir(dirs[i]);
        if (r != 0) {
            int e = stdioGET_ERRNO();
            printf("[SD CARD] mkdir %s: ret=%d errno=%d%s\r\n",
                   dirs[i], r, e, (e == pdFREERTOS_ERRNO_EEXIST) ? " (exists, OK)" : "");
        }
    }

    g_sd_disk  = &disk;
    g_sd_ready = true;

    /* 7. 写入自检: 建文件→写→读回→删除, 一次性验证读写链路 */
    {
        const char *tp = "/meeting/sd_test.txt";
        char rb[8] = {0};
        FF_FILE *tf = ff_fopen(tp, "w");
        if (tf) {
            ff_fwrite("SDOK", 1, 4, tf);
            ff_fclose(tf);
            tf = ff_fopen(tp, "r");
            if (tf) {
                ff_fread(rb, 1, 4, tf);
                ff_fclose(tf);
            }
            ff_remove(tp);
            printf("[SD CARD] Write self-test: %s\r\n",
                   (memcmp(rb, "SDOK", 4) == 0) ? "PASS" : "FAIL (readback mismatch)");
        } else {
            printf("[SD CARD] Write self-test: FAIL (create errno=%d)\r\n",
                   stdioGET_ERRNO());
        }
    }

    /* 8. 根目录内容一览 (诊断: 确认卡上实际有什么) */
    {
        FF_FindData_t fd;
        memset(&fd, 0, sizeof(fd));
        int n = 0;
        if (ff_findfirst("/", &fd) == 0) {
            do {
                printf("[SD CARD]   root: %s%s\r\n", fd.pcFileName,
                       (fd.ucAttributes & FF_FA_DIREC) ? "/" : "");
            } while (++n < 10 && ff_findnext(&fd) == 0);
        } else {
            printf("[SD CARD]   root listing failed, errno=%d\r\n", stdioGET_ERRNO());
        }
    }

    printf("[SD CARD] Ready — FreeRTOS+FAT mounted at /\r\n");
    return true;
}

bool sd_card_get_info(sd_info_t *info)
{
    if (!g_sd_ready || !info) return false;
    memset(info, 0, sizeof(sd_info_t));

    rm_freertos_plus_fat_info_t fat_info;
    fsp_err_t err = g_rm_freertos_plus_fat0.p_api->infoGet(
            g_rm_freertos_plus_fat0.p_ctrl, g_sd_disk, &fat_info);
    if (err != FSP_SUCCESS) return false;

    info->total_mb = (uint32_t)((uint64_t)fat_info.total_sectors *
                                 fat_info.sector_size / 1024 / 1024);
    info->free_mb  = (uint32_t)((uint64_t)fat_info.free_sectors *
                                 fat_info.sector_size / 1024 / 1024);
    switch (fat_info.type) {
        case RM_FREERTOS_PLUS_FAT_TYPE_FAT32: strcpy(info->fat_type, "FAT32"); break;
        case RM_FREERTOS_PLUS_FAT_TYPE_FAT16: strcpy(info->fat_type, "FAT16"); break;
        default: strcpy(info->fat_type, "?"); break;
    }
    return true;
}

int sd_card_list_dir(const char *dir_path, sd_file_info_t *files, int max_count)
{
    if (!g_sd_ready || !files || max_count < 1) return 0;

    FF_FindData_t fd;
    memset(&fd, 0, sizeof(fd));

    /* 注意: FreeRTOS+FAT 的 ff_findfirst 接受的是**目录路径**,
     * 不是通配符模式 —— 传 "path/*" 会把 '*' 当成路径组件而失败 */
    int count = 0;
    int ret = ff_findfirst(dir_path, &fd);
    if (ret != 0) {
        printf("[SD CARD] findfirst '%s' failed, errno=%d\r\n",
               dir_path, stdioGET_ERRNO());
        return 0;
    }
    while (ret == 0 && count < max_count) {
        /* Skip "." and ".." */
        if (strcmp(fd.pcFileName, ".") != 0 &&
            strcmp(fd.pcFileName, "..") != 0) {
            strncpy(files[count].name, fd.pcFileName,
                    sizeof(files[count].name) - 1);
            files[count].size_bytes  = (uint32_t)fd.ulFileSize;
            files[count].is_directory = ((fd.ucAttributes & FF_FA_DIREC) != 0);
            files[count].timestamp    = 0;  /* FreeRTOS+FAT timestamp via ff_time */
            count++;
        }
        ret = ff_findnext(&fd);
    }

    return count;
}

bool sd_card_mkdir(const char *path)
{
    if (!g_sd_ready) return false;
    return (ff_mkdir(path) == 0);
}

void *sd_card_fopen(const char *path, const char *mode)
{
    if (!g_sd_ready) return NULL;
    FF_FILE *fp = ff_fopen(path, mode);
    if (!fp) {
        printf("[SD CARD] fopen('%s','%s') failed, errno=%d\r\n",
               path, mode, stdioGET_ERRNO());
    }
    return fp;
}

uint32_t sd_card_fwrite(void *file, const void *data, uint32_t size)
{
    if (!file || !data || size == 0) return 0;
    return (uint32_t)ff_fwrite(data, 1, size, (FF_FILE *)file);
}

uint32_t sd_card_fread(void *file, void *buf, uint32_t size)
{
    if (!file || !buf || size == 0) return 0;
    return (uint32_t)ff_fread(buf, 1, size, (FF_FILE *)file);
}

bool sd_card_fseek(void *file, uint32_t offset)
{
    if (!file) return false;
    return (ff_fseek((FF_FILE *)file, (long)offset, FF_SEEK_SET) == 0);
}

uint32_t sd_card_ftell(void *file)
{
    if (!file) return 0;
    return (uint32_t)ff_ftell((FF_FILE *)file);
}

void sd_card_fclose(void *file)
{
    if (file) ff_fclose((FF_FILE *)file);
}

bool sd_card_delete(const char *path)
{
    if (!g_sd_ready) return false;
    /* ff_remove works for files */
    FF_FILE *f = ff_fopen(path, "r");
    if (f) {
        ff_fclose(f);
        return (ff_remove(path) == 0);
    }
    return false;  /* file not found */
}

const char *sd_card_err_str(void)
{
    return "OK";  /* TODO: map ff_error to strings */
}

/* ======================================================================== */
/*  卡死自愈: 强制释放 FAT 锁                                                  */
/* ======================================================================== */

/*
 * FreeRTOS+FAT 的 FAT/DIR/BUF 锁是 IO manager 上的事件组位 (ff_ioman.h):
 *   FF_FAT_LOCK=0x01, FF_DIR_LOCK=0x02, FF_BUF_LOCK=0x04
 *   锁定 = 清除对应位 + 在 pvFATLockHandle 记录当前任务; 解锁 = 置位 + 清空句柄。
 *
 * 若持有锁的任务崩溃/挂死, 锁位永远不恢复, 后续所有文件操作都会在
 * FF_LockFAT/FF_LockDirectory 里永久阻塞。这里直接重置于位并清空持有者,
 * 使 FAT 子系统立刻恢复可用。仅在播放任务被看门狗判定为挂死时调用。
 */
void sd_card_force_unlock(void)
{
    if (!g_sd_disk || !g_sd_disk->pxIOManager) return;

    FF_IOManager_t *io = g_sd_disk->pxIOManager;
    if (io->xEventGroup) {
        xEventGroupSetBits((EventGroupHandle_t)io->xEventGroup,
                           FF_FAT_LOCK | FF_DIR_LOCK | FF_BUF_LOCK);
    }
    io->pvFATLockHandle = NULL;

    printf("[SD CARD] FAT lock force-released (recovery)\r\n");
}
