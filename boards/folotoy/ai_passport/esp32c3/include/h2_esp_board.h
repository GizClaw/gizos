#ifndef H2_ESP_BOARD_H
#define H2_ESP_BOARD_H
#include "h2_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif

h2_pal_result_t h2_esp_board_runtime_config(h2_runtime_config_t* out);
/** Call only after Runtime and every borrowed provider user have stopped. */
h2_pal_result_t h2_esp_board_runtime_deinit(void);
/** Loader mount points share the board's data filesystem. */
int h2_esp_board_fs_mount(const char* path);
/** Select raw microphone capture or real AEC after stopping all audio users.
 * Failure retains the previous mode and dependencies for a later retry. */
int h2_esp_board_audio_set_aec_enabled(int enabled);
/** Releases codec, I2S and DSP memory after every audio user has stopped. */
int h2_esp_board_audio_release(void);

#ifdef __cplusplus
}
#endif
#endif
