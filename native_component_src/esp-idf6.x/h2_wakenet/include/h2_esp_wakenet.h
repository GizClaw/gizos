#ifndef H2_ESP_WAKENET_H
#define H2_ESP_WAKENET_H

#include "h2/pal/os/h2_pal_fs.h"
#include "h2/pal/os/h2_pal_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_esp_wakenet h2_esp_wakenet_t;

typedef struct h2_esp_wakenet_config {
    /** Borrowed providers/strings outlive destroy. Allocator should use PSRAM. */
    const h2_pal_mem_api_t *allocator;
    const h2_pal_fs_api_t *fs;
    const char *model_path;
    const char *model_name;
} h2_esp_wakenet_config_t;

/** Allocate an unopened detector. Failure clears output. No SDK type escapes. */
h2_pal_result_t h2_esp_wakenet_create(const h2_esp_wakenet_config_t *config,
                                     h2_esp_wakenet_t **out_detector);
/** Load a bounded, packaged ESP-SR model from FS into allocator-owned memory.
 * The adapter aligns its model view and requires payload offsets aligned to 16.
 * ESP-SR has one global model registry; an occupied registry returns
 * INVALID_STATE. All lifecycle/process calls have one caller and never race.
 * On failure retain the object and call close/destroy to release partial state.
 */
h2_pal_result_t h2_esp_wakenet_open(h2_esp_wakenet_t *detector);
/** Feed borrowed 16 kHz mono S16LE samples. Handles arbitrary chunk boundaries;
 * output is zero on failure and nonzero only for WAKENET_DETECTED. The caller
 * supplies its existing microphone stream; no second mic/task is created. */
h2_pal_result_t h2_esp_wakenet_process(h2_esp_wakenet_t *detector,
                                      const int16_t *pcm, size_t samples,
                                      int *out_detected);
/** Forget partial PCM and neural history; unopened objects return INVALID_STATE. */
h2_pal_result_t h2_esp_wakenet_reset(h2_esp_wakenet_t *detector);
/** Idempotent. Failed FS close retains the handle for retry. Never races process. */
h2_pal_result_t h2_esp_wakenet_close(h2_esp_wakenet_t *detector);
/** Close then free. On failure retains *detector, on success clears it. */
h2_pal_result_t h2_esp_wakenet_destroy(h2_esp_wakenet_t **detector);

#ifdef __cplusplus
}
#endif

#endif
