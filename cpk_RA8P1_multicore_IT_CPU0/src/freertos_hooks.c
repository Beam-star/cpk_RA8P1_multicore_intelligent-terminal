/**
 ******************************************************************************
 * @file    freertos_hooks.c
 * @brief   FreeRTOS 应用侧钩子（CPU0）
 *
 * 栈溢出钩子：configCHECK_FOR_STACK_OVERFLOW=1 时，某任务栈写穿会回调这里，
 * 打印出是哪个任务后停机（关中断 + 死循环），方便调试器现场定位。
 ******************************************************************************
 */

#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("\r\n[OS] STACK OVERFLOW in task: %s\r\n", pcTaskName);
    taskDISABLE_INTERRUPTS();
    for (;;) { }
}
