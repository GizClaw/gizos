#ifndef H2_ESP_BOARD_H
#define H2_ESP_BOARD_H

#include "h2_runtime.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*h2_esp_board_entry_task_fn)(void *user);

/**
 * Optional workload-specific display transport tuning. A zero pixel clock
 * keeps the SH8601 component default. Configure this before the first display
 * or panel power operation.
 */
typedef struct h2_esp_board_display_config {
    uint32_t pclk_hz;
} h2_esp_board_display_config_t;

h2_pal_result_t h2_esp_board_display_configure(
    const h2_esp_board_display_config_t *config);

#define H2_ESP_BOARD_AUDIO_TASK_CORE_UNPINNED (-1)

/**
 * Optional workload-specific tuning for the board-owned ES8311 audio system.
 * Configure this before h2_esp_board_runtime_config(). Zero DMA values select
 * the ESP-IDF defaults; H2_ESP_BOARD_AUDIO_TASK_CORE_UNPINNED leaves a task
 * unpinned.
 */
typedef struct h2_esp_board_audio_config {
    uint32_t i2s_dma_desc_num;
    uint32_t i2s_dma_frame_num;
    uint32_t mic_gain_db;
    uint8_t mic_queue_frames;
    uint32_t mic_task_priority;
    int32_t mic_task_core_id;
    uint32_t speaker_task_priority;
    int32_t speaker_task_core_id;
    int aggressive_aec_nlp;
} h2_esp_board_audio_config_t;

h2_pal_result_t h2_esp_board_audio_configure(
    const h2_esp_board_audio_config_t *config);

h2_pal_result_t h2_esp_board_runtime_config(h2_runtime_config_t *out_config);

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
h2_pal_result_t h2_esp_board_start_entry_task(
    const char *name,
    h2_esp_board_entry_task_fn entry,
    void *user);

#ifdef __cplusplus
}
#endif

#endif
