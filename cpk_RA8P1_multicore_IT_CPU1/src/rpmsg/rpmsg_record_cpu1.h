/*
 * rpmsg_record_cpu1.h — CPU1-side record-control channel
 *
 * A dedicated FreeRTOS task listens on REC_EPT_CPU1 for START/STOP commands
 * and drives av_recorder_start() / av_recorder_stop().
 */

#ifndef RPMSG_RECORD_CPU1_H_
#define RPMSG_RECORD_CPU1_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Initialise the record-control channel (CPU1 side).
 *
 * Creates a receive endpoint + queue and launches the dispatcher task.
 * Must be called after rpmsg_core_init() and av_recorder_init().
 *
 * @return true on success.
 */
bool rpmsg_record_cpu1_init(void);

#ifdef __cplusplus
}
#endif

#endif /* RPMSG_RECORD_CPU1_H_ */
