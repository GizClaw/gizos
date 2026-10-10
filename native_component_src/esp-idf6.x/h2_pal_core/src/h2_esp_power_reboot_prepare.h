#ifndef H2_ESP_POWER_REBOOT_PREPARE_H
#define H2_ESP_POWER_REBOOT_PREPARE_H

#include "h2/pal/core/h2_pal_errors.h"
#include <stdbool.h>
#include <stdint.h>

/* Private native preparation; ordinary task callers only. No-op on targets
 * without S31's local NimBLE controller. Errors prevent the SDK restart. */
h2_pal_result_t h2_esp_power_reboot_prepare(void);

/* Skip the SDK write only when both execution and next-boot selection already
 * refer to the target. Restoring a running App after failed preparation must
 * still undo a previously selected Loader. Callers validate all partitions. */
static inline bool h2_esp_power_boot_selection_is_current(
    uint32_t running, uint32_t selected, uint32_t target) {
    return running == target && selected == target;
}

#endif
