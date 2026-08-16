/*
 * Copyright 2018-2021 NXP
 * All rights reserved.
 */
#include <platform/ra8p1/rpmsg_config.h>
#include <platform/ra8p1/rpmsg_platform.h>
#include <stdio.h>
#include <string.h>

#include "rpmsg_env.h"
#include "rpmsg_lite.h"
#include "hal_data.h"

#if defined(RL_USE_MCMGR_IPC_ISR_HANDLER) && (RL_USE_MCMGR_IPC_ISR_HANDLER == 1)
#include "mcmgr.h"
#endif

#if defined(RL_USE_ENVIRONMENT_CONTEXT) && (RL_USE_ENVIRONMENT_CONTEXT == 1)
#error "This RPMsg-Lite port requires RL_USE_ENVIRONMENT_CONTEXT set to 0"
#endif

static int32_t isr_counter     = 0;
static int32_t disable_counter = 0;
static void *platform_lock;

#if defined(RL_USE_STATIC_API) && (RL_USE_STATIC_API == 1)
static LOCK_STATIC_CONTEXT platform_lock_static_ctxt;
#endif

#if defined(RL_USE_MCMGR_IPC_ISR_HANDLER) && (RL_USE_MCMGR_IPC_ISR_HANDLER == 1)
static void mcmgr_event_handler(uint16_t vring_idx, void *context)
{
    env_isr((uint32_t)vring_idx);
}
#else

// RX callback for CPU1 (IPC1 receives from CPU0)
void g_ipc1_callback(ipc_callback_args_t *p_args)
{
    if (IPC_EVENT_MESSAGE_RECEIVED & p_args->event)
    {
        env_isr(p_args->message);
    }
    else if (p_args->event & 0xFFU)
    {
        uint32_t vq = (p_args->event & 1U) ? 0U : 1U;
        env_isr(vq);
    }
}
#endif

static void platform_global_isr_disable(void)  { __asm volatile("cpsid i"); }
static void platform_global_isr_enable(void)   { __asm volatile("cpsie i"); }

int32_t platform_init_interrupt(uint32_t vector_id, void *isr_data)
{
    if (platform_lock != ((void *)0))
    {
        env_register_isr(vector_id, isr_data);
        env_lock_mutex(platform_lock);
        RL_ASSERT(0 <= isr_counter);
        if (isr_counter == 0) NVIC_SetPriority(IPC_IRQ1_IRQn, 10);
        isr_counter++;
        env_unlock_mutex(platform_lock);
        return 0;
    }
    return -1;
}

int32_t platform_deinit_interrupt(uint32_t vector_id)
{
    if (platform_lock != ((void *)0))
    {
        env_lock_mutex(platform_lock);
        RL_ASSERT(0 < isr_counter);
        isr_counter--;
        if (isr_counter == 0) NVIC_DisableIRQ(IPC_IRQ1_IRQn);
        env_unregister_isr(vector_id);
        env_unlock_mutex(platform_lock);
        return 0;
    }
    return -1;
}

void platform_notify(uint32_t vector_id)
{
    env_lock_mutex(platform_lock);

    uint32_t msg = (uint32_t)(RL_GET_Q_ID(vector_id));

    // CPU1 sends to CPU0 via IPC0 (only EventGenerate, no FIFO/RXD)
    R_IPC_EventGenerate(&g_ipc0_ctrl, (ipc_generate_event_t)(1U << msg));

    env_unlock_mutex(platform_lock);
}

void platform_time_delay(uint32_t num_msec)
{
    uint32_t loop;
    SystemCoreClockUpdate();
    loop = SystemCoreClock / 3U / 1000U * num_msec;
    while (loop > 0U) { __NOP(); loop--; }
}

int32_t platform_in_isr(void)
{
    return (((SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk) != 0UL) ? 1 : 0);
}

int32_t platform_interrupt_enable(uint32_t vector_id)
{
    RL_ASSERT(0 < disable_counter);
    platform_global_isr_disable();
    disable_counter--;
    if (disable_counter == 0) NVIC_EnableIRQ(IPC_IRQ1_IRQn);
    platform_global_isr_enable();
    return ((int32_t)vector_id);
}

int32_t platform_interrupt_disable(uint32_t vector_id)
{
    RL_ASSERT(0 <= disable_counter);
    platform_global_isr_disable();
    if (disable_counter == 0) NVIC_DisableIRQ(IPC_IRQ1_IRQn);
    disable_counter++;
    platform_global_isr_enable();
    return ((int32_t)vector_id);
}

void platform_map_mem_region(uint32_t vrt_addr, uint32_t phy_addr, uint32_t size, uint32_t flags) {}
void platform_cache_all_flush_invalidate(void) {}
void platform_cache_disable(void) {}
uint32_t platform_vatopa(void *addr) { return ((uintptr_t)(char *)addr); }
void *platform_patova(uintptr_t addr)    { return ((void *)(char *)addr); }

int32_t platform_init(void)
{
#if defined(RL_USE_MCMGR_IPC_ISR_HANDLER) && (RL_USE_MCMGR_IPC_ISR_HANDLER == 1)
    mcmgr_status_t retval = kStatus_MCMGR_Error;
    retval = MCMGR_RegisterEvent(kMCMGR_RemoteRPMsgEvent, mcmgr_event_handler, ((void *)0));
    if (kStatus_MCMGR_Success != retval) return -1;
#else
    R_IPC_Open(&g_ipc0_ctrl, &g_ipc0_cfg);
    R_IPC_Open(&g_ipc1_ctrl, &g_ipc1_cfg);
#endif
#if defined(RL_USE_STATIC_API) && (RL_USE_STATIC_API == 1)
    if (0 != env_create_mutex(&platform_lock, 1, &platform_lock_static_ctxt))
#else
    if (0 != env_create_mutex(&platform_lock, 1))
#endif
    { return -1; }
    return 0;
}

int32_t platform_deinit(void)
{
    R_IPC_Close(&g_ipc0_ctrl);
    R_IPC_Close(&g_ipc1_ctrl);
    env_delete_mutex(platform_lock);
    platform_lock = ((void *)0);
    return 0;
}
