/**
 ******************************************************************************
 * @file    fp_name_db.c
 * @brief   指纹 ID ↔ 名字映射库实现（W25Q256 分区直存）
 *
 * 数据模型：一份 RAM 副本 + 整块回写 flash。
 *   - save/clear 只更新 RAM；写回 flash 由 zw111_fp worker 调 fp_name_db_flush()。
 *   - flush 只用 w25q256_write()，**不做回读校验**——回读 memory-mapped 在运行时
 *     写之后会触发崩溃（见 w25q256_read 里被 #if 0 的 D-Cache 失效）。
 *   - init 读 flash，magic 不匹配则视为空库（不写 flash；首次 flush 会写入）。
 ******************************************************************************
 */

#include "fp_name_db.h"
#include "driver/w25q256/w25q256.h"
#include "driver/w25q256/w25q256_partition.h"
#include <string.h>
#include <stdio.h>

#define FP_NAME_MAGIC   0x46504E31UL   /* "FPN1" */

typedef struct {
    uint32_t magic;
    char     names[FP_NAME_MAX][FP_NAME_LEN];   /* index = id - 1 */
} fp_name_db_t;

static fp_name_db_t g_db;
static bool         g_inited = false;

void fp_name_db_init(void)
{
    memset(&g_db, 0, sizeof(g_db));

    w25q256_err_t e = w25q256_read(PART_FP_NAME_OFFSET,
                                   (uint8_t *)&g_db, sizeof(g_db));
    if (e != W25Q256_OK) {
        printf("[FPNAME] read failed: %s\r\n", w25q256_err_str(e));
    } else if (g_db.magic != FP_NAME_MAGIC) {
        /* 从未初始化：空库。首次 flush 会写入 magic + 名字。 */
        memset(&g_db, 0, sizeof(g_db));
    }

    g_inited = true;
    printf("[FPNAME] init done\r\n");
}

bool fp_name_db_save(uint16_t id, const char *name)
{
    if (!g_inited || id == 0 || id > FP_NAME_MAX || name == NULL) {
        printf("[FPNAME] save rejected (inited=%d id=%u)\r\n",
               (int)g_inited, (unsigned)id);
        return false;
    }

    strncpy(g_db.names[id - 1], name, FP_NAME_LEN - 1);
    g_db.names[id - 1][FP_NAME_LEN - 1] = '\0';
    printf("[FPNAME] save id=%u name=%s\r\n", (unsigned)id, name);
    return true;
}

const char *fp_name_db_get(uint16_t id)
{
    if (!g_inited || id == 0 || id > FP_NAME_MAX) {
        return "";
    }
    return g_db.names[id - 1];
}

bool fp_name_db_clear(void)
{
    if (!g_inited) {
        return false;
    }

    memset(&g_db, 0, sizeof(g_db));
    g_db.magic = FP_NAME_MAGIC;
    return true;
}

bool fp_name_db_flush(void)
{
    w25q256_err_t e;

    /* 重新建立 OSPI_B 状态（SPI 模式 + 配置），与 PCDC 下载的 open→erase→write
     * 一致。启动后 OSPI_B 做过一堆 XIP 读取，直接 erase 会卡住。 */
    w25q256_open();

    e = w25q256_erase_sector(PART_FP_NAME_OFFSET);
    if (e != W25Q256_OK) {
        printf("[FPNAME] erase failed: %s\r\n", w25q256_err_str(e));
        return false;
    }
    e = w25q256_write(PART_FP_NAME_OFFSET, (const uint8_t *)&g_db, sizeof(g_db));
    if (e != W25Q256_OK) {
        printf("[FPNAME] write failed: %s\r\n", w25q256_err_str(e));
        return false;
    }

    printf("[FPNAME] flush OK\r\n");
    return true;
}
