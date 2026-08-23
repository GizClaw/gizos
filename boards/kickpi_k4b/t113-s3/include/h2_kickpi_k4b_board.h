#ifndef H2_KICKPI_K4B_BOARD_H
#define H2_KICKPI_K4B_BOARD_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_KICKPI_K4B_DISPLAY_WIDTH 1024u
#define H2_KICKPI_K4B_DISPLAY_HEIGHT 600u

typedef enum h2_kickpi_k4b_periph_id {
    H2_KICKPI_K4B_PERIPH_ACTION_BUTTON = 1u,
} h2_kickpi_k4b_periph_id_t;

typedef struct h2_kickpi_k4b_board_providers {
    const h2_pal_audio_t *audio;
    const h2_pal_audio_decoder_api_t *audio_decoder;
    const h2_pal_video_decoder_api_t *video_decoder;
} h2_kickpi_k4b_board_providers_t;

h2_pal_result_t h2_kickpi_k4b_board_runtime_config(
    h2_runtime_config_t *out_config,
    const h2_kickpi_k4b_board_providers_t *providers);

#ifdef __cplusplus
}
#endif

#endif
