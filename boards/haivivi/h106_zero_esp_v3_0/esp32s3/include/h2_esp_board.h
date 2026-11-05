#ifndef H2_ESP_BOARD_H
#define H2_ESP_BOARD_H

#include "h2_runtime.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Poll RI and normalize calls from the serialized App modem task. */
void h2_esp_board_modem_poll(void);
/** Nonblocking Main-thread read of real RI edges; no AT or power changes. */
bool h2_esp_board_modem_ri_pending(void);

typedef void (*h2_esp_board_entry_task_fn)(void* user);

/** Nonblocking atomic scan policy, default off; enumeration is unconditional.
 * The next scan initializes or powers down the reader. MFG enables explicitly.
 */
void h2_esp_board_set_nfc_enabled(bool enabled);

/** Borrow the image-lifetime arena, or NULL if early reservation failed.
 * Runtime teardown does not invalidate this API; callers free their own blocks.
 */
const h2_pal_mem_api_t* h2_esp_board_arena_mem(void);

struct h2_esp_task_policy_config;
/** Install generated task policy after reserving the image-lifetime arena.
 * Called once by the target installer before any PAL task starts. The config
 * is borrowed for this call; its resolver and user are preserved. */
h2_pal_result_t h2_esp_board_task_configure(
    const struct h2_esp_task_policy_config* config);

h2_pal_result_t h2_esp_board_runtime_config(h2_runtime_config_t* out_config);

/**
 * Tears down board-owned Runtime providers after h2_runtime_deinit().
 *
 * All HTTP requests must have returned before this call. The teardown destroys
 * the HTTP provider before releasing its filesystem dependency. A caller whose
 * h2_runtime_init() fails after runtime_config() succeeds must also call this
 * function. Repeated calls after teardown return H2_PAL_OK.
 */
h2_pal_result_t h2_esp_board_runtime_deinit(void);

/**
 * Starts the board image entry point on a detached PSRAM-backed task.
 *
 * The caller must return from app_main after this function succeeds. The
 * supplied user pointer must remain valid until the entry function returns.
 * The entry function must release Runtime before returning; the task wrapper
 * then calls h2_esp_board_runtime_deinit() before deleting itself.
 */
h2_pal_result_t h2_esp_board_start_entry_task(const char* name,
                                              h2_esp_board_entry_task_fn entry,
                                              void* user);
h2_pal_result_t h2_esp_board_prepare_audio(void);
const h2_pal_power_api_t* h2_esp_board_power(void);
/** Observe the retained/tracked modem-enable state, without AT or GPIO writes.
 */
bool h2_esp_board_modem_is_powered(void);
/** Normal H106 preparation; untouched wakes skip modem transactions. May wait
 * for owned Modem/audio/Flash resources. No final power commit. */
h2_pal_result_t h2_esp_board_shutdown_prepare(void* user, bool physical_off);
/**
 * Irreversible H106 commit from a dedicated internal-stack task. Does not
 * access App state, log, Flash, or Modem/audio APIs; never returns. Keeps RI
 * wake/Modem power only for !physical_off. GPIO/SDK failure is fatal.
 */
void h2_esp_board_shutdown_commit(void* user, bool physical_off);
/** Direct power-button level for pre-runtime boot gating (1 = pressed). */
int h2_esp_board_power_button_pressed(void);
/** Reads the raw external-power detect input used by the logical-off gate. */
h2_pal_result_t h2_esp_board_external_power_present(int* out_present);
/**
 * @brief Read unclassified button ADC counts and calibrated mV for diagnostics.
 * @param[out] out_raw Required caller-owned count output, zero on failure.
 * @param[out] out_millivolts Required caller-owned mV output, -1 on failure or
 * unsupported calibration.
 * @return OK for a valid raw sample, including unsupported calibration;
 * INVALID_ARG for missing outputs, or an ADC initialization/read error.
 *
 * Task context only. Board Runtime must remain initialized. Initialize from
 * one caller while Runtime input is stopped and serialize against teardown.
 * Counts and mV use adjacent conversions. No production threshold or debounce
 * is applied; output storage is borrowed only for this call.
 */
h2_pal_result_t h2_esp_board_button_adc_read(int* out_raw, int* out_millivolts);
/** Actively drives non-wake peripheral pins low for the charging-idle state. */
void h2_esp_board_quiesce_peripherals(void);
/**
 * @brief Format a best-effort display DMA/SPI2 observation without waiting for
 * SPI.
 * @param[out] out Caller-owned text buffer, NUL-terminated when capacity is
 * nonzero.
 * @param[in] capacity Buffer size in bytes, including the terminator.
 * @return Bytes stored excluding the terminator, or zero for NULL/zero
 * capacity. Task context only. Fields may advance independently during capture;
 * counters wrap at 32 bits; timestamps retain 64-bit uptime. Reading does not
 * acknowledge interrupts.
 */
size_t h2_esp_board_display_diagnostics(char* out, size_t capacity);

/* Optional task-context tap of completed SPI DMA bytes, not panel readback.
 * Rect/pixels are borrowed for this call; RGB565 words use panel byte order.
 * Callback must not wait, allocate or reenter Display. Configure/unregister
 * only while display callers are quiescent. Returns real board geometry without
 * opening hardware; NULL unregisters the tap and may be used to query caps. */
typedef void (*h2_esp_board_display_transfer_capture_fn)(
    void* user, const h2_display_rect_t* rect, const uint16_t* pixels);
h2_display_info_t h2_esp_board_display_set_transfer_capture(
    h2_esp_board_display_transfer_capture_fn capture, void* user);

int h2_esp_board_fs_mount(const char* path);
/** Reads allocation on the mounted external filesystem. Outputs are cleared
 * on error; returns a PAL result. Caller serializes against mount/unmount. */
int h2_esp_board_storage_usage(size_t* out_total, size_t* out_used);

#ifdef __cplusplus
}
#endif

#endif
