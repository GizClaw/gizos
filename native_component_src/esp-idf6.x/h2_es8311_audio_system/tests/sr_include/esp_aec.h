#ifndef ESP_AEC_TEST_H
#define ESP_AEC_TEST_H
#include <stdint.h>
typedef struct {
  int mic_num, ref_num, out_num, filter_length, sample_rate;
  uint32_t caps;
  int mode, nlp_level;
} aec_config_t;
typedef struct aec_handle aec_handle_t;
#define AEC_MODE_FD_LOW_COST 5
#define AEC_MODE_VOIP_LOW_COST 3
#define AEC_NLP_LEVEL_NORMAL 0
#define AEC_NLP_LEVEL_AGGR 1
aec_handle_t* aec_create_from_config(aec_config_t* config);
int aec_get_chunksize(const aec_handle_t* handle);
void aec_process(const aec_handle_t* handle, int16_t* mic, int16_t* ref,
                 int16_t* out);
void aec_linear_process(const aec_handle_t* handle, int16_t* mic, int16_t* ref,
                        int16_t* out);
void aec_destroy(aec_handle_t* handle);
#endif
