#ifndef __SUB_0000_INVOKE_H__
#define __SUB_0000_INVOKE_H__

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// NPU arena — pointer to SDRAM (not a static array).
// Initialised in sub_0000_invoke.c to FACE_MODEL_ARENA_ADDR.
extern uint8_t *sub_0000_arena;

// Fast scratch — reuses arena pointer (not allocated separately).
extern uint8_t *sub_0000_fast_scratch;

int sub_0000_invoke(bool clean_outputs);


#endif // __SUB_0000_INVOKE_H__
