#ifndef H2_ESP_BOARD_H
#define H2_ESP_BOARD_H
#include "h2_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*h2_esp_board_entry_task_fn)(void* user);
/** Run a blocking image entry on a dedicated 64 KiB PSRAM task. Call from
 * task context after installing the target policy. The callback owns Runtime
 * teardown; board resources are released after it returns. */
h2_pal_result_t h2_esp_board_start_entry_task(const char* name,
                                              h2_esp_board_entry_task_fn entry,
                                              void* user);
/** Configure the board's borrowed Runtime providers from task context.
 * Serialize startup and teardown. Shared BSP power/I2C resources live for the
 * boot lifetime. Returns a PAL error and clears output on failure.
 */
h2_pal_result_t h2_esp_board_runtime_config(h2_runtime_config_t* out_config);
/** Release Runtime-owned board resources after all borrowers have stopped.
 * The caller must first deinitialize Runtime and finish outstanding HTTP work.
 * Repeated calls are allowed. Shared BSP resources remain owned by the board.
 */
h2_pal_result_t h2_esp_board_runtime_deinit(void);
#ifdef __cplusplus
}
#endif
#endif
