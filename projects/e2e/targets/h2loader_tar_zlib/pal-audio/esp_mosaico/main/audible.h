#ifndef H2_MOSAICO_AUDIO_AUDIBLE_H
#define H2_MOSAICO_AUDIO_AUDIBLE_H
#include "h2/pal/hal/h2_pal_audio.h"
/* Serialized artifact caller only. API success never proves audible sound. */
typedef struct {
  int work_rc, close_rc, stop_rc, restore_rc;
  uint32_t saved_volume, applied_volume;
  unsigned blocks_written;
} h2_mosaico_audible_result_t;
int h2_mosaico_audible_cue(const h2_pal_audio_api_t *audio,
                           h2_mosaico_audible_result_t *result);
#endif
