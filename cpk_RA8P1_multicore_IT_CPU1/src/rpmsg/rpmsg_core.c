/*
 * rpmsg_core.c - CPU1 RPMsg 实例管理（纯实例）
 */

#include "rpmsg_core.h"
#include <platform/ra8p1/rpmsg_platform.h>
#include "rpmsg_lite.h"
#include <stdio.h>

static struct rpmsg_lite_instance *rpmsg_instance = NULL;

void *rpmsg_core_get_instance(void)
{
    return rpmsg_instance;
}

void *rpmsg_core_init(void)
{
    if (rpmsg_instance != NULL) return rpmsg_instance;

    rpmsg_instance = rpmsg_lite_remote_init(
        (void *)RPMSG_LITE_SHMEM_BASE,
        RL_PLATFORM_RA8P1_LINK_ID,
        RL_NO_FLAGS
    );
    return rpmsg_instance;
}
