/*
 * rpmsg_core.h - CPU1 RPMsg 实例管理（纯实例，不含日志）
 */

#ifndef RPMSG_CORE_H_
#define RPMSG_CORE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RPMSG_LITE_SHMEM_BASE  0x220E2000UL
#define SH_MEM_TOTAL_SIZE      (64UL * 1024UL)

void *rpmsg_core_init(void);
void *rpmsg_core_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif /* RPMSG_CORE_H_ */
