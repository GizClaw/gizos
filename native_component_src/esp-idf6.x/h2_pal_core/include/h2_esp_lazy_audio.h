#ifndef H2_ESP_LAZY_AUDIO_H
#define H2_ESP_LAZY_AUDIO_H

#include "h2/pal/hal/h2_pal_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef h2_pal_audio_t *(*h2_esp_lazy_audio_resolve_fn)(void *user);

typedef struct h2_esp_lazy_audio {
    void *user;
    h2_esp_lazy_audio_resolve_fn resolve;
    h2_pal_audio_t api;
} h2_esp_lazy_audio_t;

int h2_esp_lazy_audio_init(
    h2_esp_lazy_audio_t *lazy_audio,
    void *user,
    h2_esp_lazy_audio_resolve_fn resolve);
h2_pal_audio_t *h2_esp_lazy_audio_api(h2_esp_lazy_audio_t *lazy_audio);

#ifdef __cplusplus
}
#endif

#endif
