/**
 * @file    syscalls_stubs.c
 * @brief   Minimal newlib/syscalls stubs for embedded C++ (Clang ARM)
 *
 * The C++ runtime (libcpp) references abort() → _exit() and raise() → _exit().
 * In a bare-metal FreeRTOS environment, these must be provided by the application.
 */
#include "face_detection_build_mode.h"

#if !FACE_DETECT_FLASH_PROGRAMMER

#include <stdlib.h>

void _exit(int status)
{
    (void)status;
    /* Halt the system — in FreeRTOS this should never be reached */
    while (1) {
        __asm volatile("wfi");
    }
}

#endif /* !FACE_DETECT_FLASH_PROGRAMMER */
