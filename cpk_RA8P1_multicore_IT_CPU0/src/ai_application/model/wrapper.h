#ifndef MODEL_WRAPPER_H
#define MODEL_WRAPPER_H

#include "model.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Input tensor pointer: 96×96×1 INT8 grayscale, inside NPU arena. */
static inline uint8_t* mera_input_ptr(void) {
    return (uint8_t*) GetModelInputPtr_serving_default_images_0();
}

/** Output tensor 0: box distances [1, 4, 180] INT8, inside NPU arena. */
static inline uint8_t* mera_output1_ptr(void) {
    return (uint8_t*) GetModelOutputPtr_PartitionedCall_0_70200();
}

/** Output tensor 1: objectness scores [1, 1, 180] INT8, inside NPU arena. */
static inline uint8_t* mera_output2_ptr(void) {
    return (uint8_t*) GetModelOutputPtr_PartitionedCall_1_70211();
}

/** Trigger one full NPU inference pass. */
static inline void mera_invoke(void) {
    RunModel(false);   /* false = don't memset output areas (caller manages) */
}

#ifdef __cplusplus
}
#endif

#endif // MODEL_WRAPPER_H
