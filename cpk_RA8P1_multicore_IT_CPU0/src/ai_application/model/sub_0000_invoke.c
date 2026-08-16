/**
 * sub_0000_invoke.c — Titan-mini board (SDRAM-based), v3 model
 *
 * v3 face detection model: 96×96×1 INT8 grayscale input, anchor-free.
 * Same architecture as v2 but wider (width 0.75, mid=32).
 *
 * Output tensors (both INT8, in arena):
 *   PartitionedCall_0_70200: [1, 4, 180]  720 B — box distances (ltrb)
 *   PartitionedCall_1_70211: [1, 1, 180]  180 B — objectness scores
 *
 * Sizes (v3 is ~2× larger than v2):
 *   Model data:      250,416 B  (v2: 134,336 B)
 *   Command stream:   11,980 B  (v2:  11,684 B)
 *   Tensor Arena:    221,184 B  (v2: 147,456 B)
 *   MACs:             30.06 M   (v2:  14.58 M)
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "common_data.h"

#include "sub_0000_tensors.h"
#include "sub_0000_invoke.h"
#include "../face_detection_config.h"

#include "ethosu_driver.h"

extern const uint8_t *face_detect_get_model_data(void);
extern size_t          face_detect_get_model_data_size(void);
extern const uint8_t *face_detect_get_command_stream(void);
extern size_t          face_detect_get_command_stream_size(void);

uint8_t *sub_0000_arena       = (uint8_t *)FACE_MODEL_ARENA_ADDR;
uint8_t *sub_0000_fast_scratch = NULL;

volatile bool g_npu_inferencing = false;

int sub_0000_invoke(bool clean_outputs)
{
    uint64_t base_addrs[6]      = {0};
    size_t   base_addrs_size[6] = {0};
    int      num_base_addrs     = 6;

    const uint8_t *model_data      = face_detect_get_model_data();
    size_t         model_data_size = face_detect_get_model_data_size();

    sub_0000_fast_scratch = sub_0000_arena;

    uint8_t *cms_data = (uint8_t *)face_detect_get_command_stream();
    int      cms_size = (int)face_detect_get_command_stream_size();

    /* ---- 6 base-address regions (v3 offsets) ---- */

    /* [0] Model weights in SDRAM (250,416 B) */
    base_addrs[0]      = (uint64_t)(uintptr_t)model_data;
    base_addrs_size[0] = model_data_size;

    /* [1] Arena region 0 (221,184 B) */
    base_addrs[1]      = (uint64_t)(uintptr_t)(sub_0000_arena + 0);
    base_addrs_size[1] = 221184;

    /* [2] Fast scratch (same as arena) */
    base_addrs[2]      = (uint64_t)(uintptr_t)(sub_0000_arena + 0);
    base_addrs_size[2] = 221184;

    /* [3] Input tensor: 96×96×1 INT8 = 9,216 B, at arena offset 73728 (v3) */
    base_addrs[3]      = (uint64_t)(uintptr_t)(sub_0000_arena + 73728);
    base_addrs_size[3] = 9216;

    /* [4] Score output [1,1,180] = 180 B, at arena offset 0 */
    if (clean_outputs) {
        memset(sub_0000_arena + 0, 0, 180);
    }
    base_addrs[4]      = (uint64_t)(uintptr_t)(sub_0000_arena + 0);
    base_addrs_size[4] = 180;

    /* [5] Box output [1,4,180] = 720 B, at arena offset 192 */
    if (clean_outputs) {
        memset(sub_0000_arena + 192, 0, 720);
    }
    base_addrs[5]      = (uint64_t)(uintptr_t)(sub_0000_arena + 192);
    base_addrs_size[5] = 720;

    /* ---- Optimizer ID patch ---- */
    {
        uint32_t hw_id  = *(const uint32_t *)((volatile const uint32_t *)R_NPU_BASE);
        uint32_t opt_id = *(const uint32_t *)(cms_data + 12);
        if (hw_id != opt_id) {
            *(uint32_t *)(cms_data + 12) = hw_id;
#if (BSP_CFG_DCACHE_ENABLED == 1)
            SCB_CleanDCache_by_Addr((volatile void *)(cms_data + 12), 4);
            __DSB();
#endif
        }
    }

    /* ---- D-Cache clean: input tensor area (9,216 B, offset 73728) ---- */
#if (BSP_CFG_DCACHE_ENABLED == 1)
    SCB_CleanDCache_by_Addr((volatile void *)(sub_0000_arena + 73728), 9216);
    __DSB();
#endif

    if (num_base_addrs > 8) {
        num_base_addrs = 8;
    }

    int result = ethosu_invoke_v3(&g_ethosu0, cms_data, cms_size,
                                   base_addrs, base_addrs_size,
                                   num_base_addrs, NULL);

    /* ---- D-Cache invalidate: output tensor areas ---- */
#if (BSP_CFG_DCACHE_ENABLED == 1)
    SCB_InvalidateDCache_by_Addr((volatile void *)(sub_0000_arena + 0), 180);
    SCB_InvalidateDCache_by_Addr((volatile void *)(sub_0000_arena + 192), 720);
    __DSB();
#endif

    if (result != 0) {
        return -1;
    }

    return 0;
}
