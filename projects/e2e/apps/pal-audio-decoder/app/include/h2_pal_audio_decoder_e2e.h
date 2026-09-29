#ifndef H2_PAL_AUDIO_DECODER_E2E_H
#define H2_PAL_AUDIO_DECODER_E2E_H

#include "h2/pal/hal/h2_pal_audio_decoder.h"
#include "h2/pal/os/h2_pal_time.h"
#include "h2/pal/os/h2_pal_sync.h"

enum {
    H2_PAL_ADEC_CASE_COUNT = 0
#define H2_PAL_ADEC_CASE(id, function) + 1
#include "h2_pal_audio_decoder_cases.inc"
#undef H2_PAL_ADEC_CASE
};

typedef enum h2_pal_adec_case_status {
    H2_PAL_ADEC_NOT_RUN, H2_PAL_ADEC_PASS,
    H2_PAL_ADEC_FAIL, H2_PAL_ADEC_BLOCKED
} h2_pal_adec_case_status_t;

typedef struct h2_pal_adec_case_result {
    const char *id;
    h2_pal_adec_case_status_t status;
    h2_pal_result_t detail;
    unsigned line;
} h2_pal_adec_case_result_t;

typedef struct h2_pal_adec_result {
    size_t passed, failed, blocked, retained;
    uint64_t frames, pcm_bytes;
    int qualified;
    h2_pal_adec_case_result_t cases[H2_PAL_ADEC_CASE_COUNT];
} h2_pal_adec_result_t;

typedef struct h2_pal_adec_config {
    const h2_pal_audio_decoder_api_t *decoder;
    const h2_pal_mem_api_t *mem;
    const h2_pal_time_api_t *time;
    const h2_pal_sync_api_t *sync;
    void (*pump)(void *user);
    void *pump_user;
    void (*report)(void *user, const h2_pal_adec_case_result_t *item);
    void *report_user;
} h2_pal_adec_config_t;

/** Borrow the real provider, memory and clock through the entire bounded run. */
h2_pal_result_t h2_pal_audio_decoder_e2e_run(
    const h2_pal_adec_config_t *config, h2_pal_adec_result_t *result);

#endif
