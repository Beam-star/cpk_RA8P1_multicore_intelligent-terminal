/**
 ******************************************************************************
 * @file    fatfs_wrapper.h
 * @brief   FatFS 文件系统封装层
 *
 * 封装 FatFS (r_fatfs) 的底层 API, 提供应用层友好的文件操作接口。
 * 支持长文件名 (LFN), 多任务互斥访问。
 ******************************************************************************
 */

#ifndef FATFS_WRAPPER_H_
#define FATFS_WRAPPER_H_

#include <stdint.h>
#include <stdbool.h>

/* ---- 文件系统初始化 ---- */
bool fatfs_mount(void);
bool fatfs_unmount(void);

/* ---- 文件操作 (封装 ff.c) ---- */
void *fatfs_open(const char *path, const char *mode);
uint32_t fatfs_write(void *fp, const void *buf, uint32_t size);
uint32_t fatfs_read(void *fp, void *buf, uint32_t size);
bool fatfs_seek(void *fp, uint32_t offset);
uint32_t fatfs_tell(void *fp);
void fatfs_close(void *fp);
bool fatfs_delete(const char *path);
bool fatfs_mkdir(const char *path);

/* ---- 目录遍历 ---- */
int fatfs_list_dir(const char *path, char (*names)[64], int max_count);

/* ---- 磁盘信息 ---- */
uint32_t fatfs_get_total_kb(void);
uint32_t fatfs_get_free_kb(void);

#endif /* FATFS_WRAPPER_H_ */
