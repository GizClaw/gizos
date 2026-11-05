/* Minimal provider double for the board's borrowed lazy audio interface. */
#ifndef H2_TEST_LAZY_AUDIO_H
#define H2_TEST_LAZY_AUDIO_H
#include "h2/pal/hal/h2_pal_audio.h"
typedef h2_pal_audio_t* (*h2_esp_lazy_audio_resolve_fn)(void*);
typedef struct {
  void* user;
  h2_esp_lazy_audio_resolve_fn resolve;
  h2_pal_audio_t api;
} h2_esp_lazy_audio_t;
int h2_esp_lazy_audio_init(h2_esp_lazy_audio_t*, void*,
                           h2_esp_lazy_audio_resolve_fn);
h2_pal_audio_t* h2_esp_lazy_audio_api(h2_esp_lazy_audio_t*);
#endif
