#ifndef H2_MOSAICO_AUDIO_H
#define H2_MOSAICO_AUDIO_H
#include "h2_pal.h"
int h2_mosaico_audio_init(void);
int h2_mosaico_audio_deinit(void);
const h2_pal_audio_api_t *h2_mosaico_audio(void);
#endif
