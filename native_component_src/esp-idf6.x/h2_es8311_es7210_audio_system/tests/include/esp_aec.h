#ifndef ESP_AEC_TEST_STUB_H
#define ESP_AEC_TEST_STUB_H

#include <stdint.h>

#define AEC_MODE_FD_LOW_COST 1
#define AEC_NLP_LEVEL_NORMAL 2

typedef struct aec_config {
    int mic_num;
    int ref_num;
    int out_num;
    int filter_length;
    int sample_rate;
    unsigned caps;
    int mode;
    int nlp_level;
} aec_config_t;

typedef struct aec_handle aec_handle_t;

aec_handle_t *aec_create_from_config(const aec_config_t *config);
int aec_get_chunksize(aec_handle_t *handle);
void aec_process(
    aec_handle_t *handle,
    const int16_t *mic,
    const int16_t *ref,
    int16_t *out);
void aec_destroy(aec_handle_t *handle);

#endif
