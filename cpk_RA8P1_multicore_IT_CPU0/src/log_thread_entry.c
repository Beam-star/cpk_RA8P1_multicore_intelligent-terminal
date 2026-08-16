#include "log_thread.h"
#include "rpmsg_log.h"
/* LOG Thread entry function */
/* pvParameters contains TaskHandle_t */
void log_thread_entry(void *pvParameters) {
	FSP_PARAMETER_NOT_USED(pvParameters);

	/* TODO: add your own code here */

	while (1) {
		rpmsg_log_cpu0_process();
		vTaskDelay(100);
	}
}
