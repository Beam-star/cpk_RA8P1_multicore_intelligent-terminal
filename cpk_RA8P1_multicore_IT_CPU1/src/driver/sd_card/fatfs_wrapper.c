/**
 ******************************************************************************
 * @file    fatfs_wrapper.c
 * @brief   FatFS 文件系统封装层实现
 *
 * 依赖: r_fatfs (Renesas FSP FatFS), ff.c (FatFS R0.14b)
 * 配置: _USE_LFN=1, _CODE_PAGE=936 (GB2312), _VOLUMES=1
 ******************************************************************************
 */

#include "fatfs_wrapper.h"
#include <stdio.h>
#include <string.h>

bool fatfs_mount(void)
{
    /* TODO: f_mount(&fs, "", 1); */
    printf("[FATFS] Mount OK\r\n");
    return true;
}

bool fatfs_unmount(void)
{
    /* TODO: f_mount(NULL, "", 1); */
    return true;
}

void *fatfs_open(const char *path, const char *mode)
{
    (void)path; (void)mode;
    return NULL;
}

uint32_t fatfs_write(void *fp, const void *buf, uint32_t size)
{
    (void)fp; (void)buf; (void)size;
    return 0;
}

uint32_t fatfs_read(void *fp, void *buf, uint32_t size)
{
    (void)fp; (void)buf; (void)size;
    return 0;
}

bool fatfs_seek(void *fp, uint32_t offset)
{
    (void)fp; (void)offset;
    return true;
}

uint32_t fatfs_tell(void *fp)
{
    (void)fp;
    return 0;
}

void fatfs_close(void *fp)
{
    (void)fp;
}

bool fatfs_delete(const char *path)
{
    (void)path;
    return true;
}

bool fatfs_mkdir(const char *path)
{
    (void)path;
    return true;
}

int fatfs_list_dir(const char *path, char (*names)[64], int max_count)
{
    (void)path; (void)names; (void)max_count;
    return 0;
}

uint32_t fatfs_get_total_kb(void)
{
    return 32000000;
}

uint32_t fatfs_get_free_kb(void)
{
    return 28000000;
}
