#ifndef H2_ESP_BOARD_APPLICATION_HOST_H
#define H2_ESP_BOARD_APPLICATION_HOST_H
#include "h2_esp_application_host.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Complete default PAL vtable for the linked BSP. prepare selects its Runtime
 * config; release tears down board providers after checked Runtime destruction.
 * No external producer/stop source is attached. The ordinary SDK provider adds
 * startup-task/ISR checks and yield. Keep provider and owner task on retention.
 */
h2_pal_result_t
h2_esp_board_application_host_init(h2_esp_application_host_t *provider);
#ifdef __cplusplus
}
#endif
#endif
