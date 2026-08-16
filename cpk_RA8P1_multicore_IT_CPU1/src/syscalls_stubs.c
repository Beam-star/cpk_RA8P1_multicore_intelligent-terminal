/*
 * syscalls_stubs.c — tinystdio stdout for CPU1 (Cortex-M33, bare-metal)
 *
 * Provides a write-only stdout that feeds printf() output into the RPMsg log
 * channel so all legacy debug prints appear in CPU0's SEGGER RTT console.
 *
 * Lines are buffered and sent as a single RPMsg message (avoids per-char
 * [CPU1] prefix spam).
 */

#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "rpmsg_log.h"

#define LINE_BUF_SZ  256

/* ---- Per-char put callback (tinystdio calls this for every character) ---- */
static int stdout_put(char c, struct __file *fp)
{
    (void)fp;
    static char buf[LINE_BUF_SZ];
    static int  pos = 0;

    buf[pos++] = c;

    /* Flush on newline or buffer full */
    if (c == '\n' || pos >= LINE_BUF_SZ - 1) {
        buf[pos] = '\0';
        rpmsg_log_cpu1_printf("%s", buf);
        pos = 0;
    }
    return 0;
}

/* ---- FILE object (write-only) ---- */
static FILE __stdout_file = FDEV_SETUP_STREAM(
    stdout_put, NULL, NULL, _FDEV_SETUP_WRITE);

FILE *const stdout = &__stdout_file;
FILE *const stdin  = &__stdout_file;
FILE *const stderr = &__stdout_file;

/* 栈溢出钩子：configCHECK_FOR_STACK_OVERFLOW=1 时，某任务栈写穿会回调这里。
 * 打印出是哪个任务，然后停机（关中断 + 死循环），方便调试器现场定位。 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("\r\n[CPU1-STACK] OVERFLOW detected in task: %s\r\n", pcTaskName);
    taskDISABLE_INTERRUPTS();
    for (;;) { }
}
