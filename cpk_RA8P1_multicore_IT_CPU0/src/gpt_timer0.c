/*
 * gpt_timer0.c
 *
 *  Created on: 2026年5月2日
 *      Author: Beam
 */
#include "hal_data.h"
#include "FreeRTOS.h"
#include "task.h"

/*用GPT定时器代替Systick定时器成为FreeRTOS时基*/
__attribute__((used, interrupt_save_fp)) void gpt_timer0_callback(timer_callback_args_t *p_args)
{
#if configUSE_TICKLESS_IDLE

    /* Reset the SysTick reload value if it was reconfigured for a long sleep in tickless idle. */
    if (g_reset_systick)
    {
        uint32_t completed_ticks         = (g_reset_systick / ulTimerCountsForOneTick);
        uint32_t tick_fraction_remaining = g_reset_systick - SysTick->VAL;
        rm_freertos_port_reset_systick(ulTimerCountsForOneTick - tick_fraction_remaining, completed_ticks);
    }
#endif

    /* Increment the RTOS tick. This must be done in a critical section because
     * it accesses the delayed and ready lists, which can also be modified in
     * critical sections in FromISR functions (reference xTaskRemoveFromEventList,
     * for example). */
    if(p_args->event == TIMER_EVENT_CYCLE_END) {
    uint32_t ulPreviousMask = portSET_INTERRUPT_MASK_FROM_ISR();
    if (xTaskIncrementTick() != pdFALSE)
    {
        /* A context switch is required.  Context switching is performed in
         * the PendSV interrupt.  Pend the PendSV interrupt. */
        SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    }

    portCLEAR_INTERRUPT_MASK_FROM_ISR(ulPreviousMask);
    }

}

/*GPT定时器初始化*/
void gpt_timer0_init(void)
{
   g_timer0.p_api->open(g_timer0.p_ctrl, g_timer0.p_cfg);
//   if(FSP_SUCCESS != err)
//       printf("Function:%s\tLine:%d\r\n", __FUNCTION__, __LINE__);
   g_timer0.p_api->start(g_timer0.p_ctrl);
//   if(FSP_SUCCESS != err)
//       printf("Function:%s\tLine:%d\r\n", __FUNCTION__, __LINE__);
}

/*重定义vPortSetupTimerInterrupt*/
void vPortSetupTimerInterrupt (void)
{
    /* Calculate the constants required to configure the tick interrupt. */
#if (configUSE_TICKLESS_IDLE == 1)
    {
        ulTimerCountsForOneTick         = (configSYSTICK_CLOCK_HZ / configTICK_RATE_HZ);
        xMaximumPossibleSuppressedTicks = portMAX_24_BIT_NUMBER / ulTimerCountsForOneTick;
    }
#endif

    gpt_timer0_init();
    //SysTick_Config(SystemCoreClock / configTICK_RATE_HZ);
}
