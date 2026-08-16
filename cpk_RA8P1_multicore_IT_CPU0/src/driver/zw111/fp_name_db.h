/**
 ******************************************************************************
 * @file    fp_name_db.h
 * @brief   指纹 ID ↔ 名字映射库（W25Q256 分区，键为 ZW111 指纹库 ID）
 *
 * 每个 ZW111 指纹占用一个 1..100 的 page_id，本模块把 page_id 映射到录入时
 * 输入的人员名字（英文 ≤31 字符）。数据直接按 page_id 索引存一张表：
 *
 *   [magic 4B] + names[100][32]  = 3204 B  →  落在 1 个 4KB sector 内
 *
 * 分区见 w25q256_partition.h: PART_FP_NAME_OFFSET (0xBD0000) / SIZE (0x1000)。
 *
 * save/clear **只更新 RAM**（打卡立即可查到名字）；把「写回 flash」交给
 * zw111_fp worker 执行（调 fp_name_db_flush()）。写 flash 用 w25q256_write()
 * 且不做回读校验（回读 memory-mapped 在运行时写之后会崩溃）。
 ******************************************************************************
 */
#ifndef FP_NAME_DB_H_
#define FP_NAME_DB_H_

#include <stdint.h>
#include <stdbool.h>

#define FP_NAME_MAX    100          /* ZW111 指纹库容量（ID 1..100） */
#define FP_NAME_LEN    32           /* 名字最大长度（含 '\0'） */

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化：从 W25Q256 分区加载到 RAM；magic 无效则视为空库。 */
void fp_name_db_init(void);

/** 保存/更新某指纹 ID 的名字（只更新 RAM，快速）。 */
bool fp_name_db_save(uint16_t id, const char *name);

/** 读取某指纹 ID 的名字（直接读 RAM 副本）；无记录返回空串 ""。 */
const char *fp_name_db_get(uint16_t id);

/** 清空 RAM 副本（快速，配合 ZW111 PS_Empty）。 */
bool fp_name_db_clear(void);

/** 把 RAM 副本写回 flash（擦 4KB + 写整块）。应由 zw111_fp worker 调用。 */
bool fp_name_db_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* FP_NAME_DB_H_ */
