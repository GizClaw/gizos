#ifndef FAKE_ESP_AEC_H
#define FAKE_ESP_AEC_H

#include "esp_aec.h"

#include <stdint.h>

#define FAKE_ESP_AEC_MAX_SAMPLES 16

void fake_esp_aec_reset(int chunk_samples);
const aec_config_t *fake_esp_aec_config(void);
const int16_t *fake_esp_aec_mic(void);
const int16_t *fake_esp_aec_ref(void);
int fake_esp_aec_process_count(void);
int fake_esp_aec_destroy_count(void);

#endif
