#ifndef H2_FM17660K_H
#define H2_FM17660K_H

#include "h2_fm17660k_transport.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/hal/h2_pal_nfc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_fm17660k h2_fm17660k_t;

typedef struct h2_fm17660k_config {
    h2_pal_periph_id_t periph_id;
    const h2_pal_mem_api_t *mem;
    h2_fm17660k_transport_t transport;
    uint32_t operation_timeout_ms;
} h2_fm17660k_config_t;

h2_pal_result_t h2_fm17660k_init(
    const h2_fm17660k_config_t *config,
    h2_fm17660k_t **out_fm17660k);

void h2_fm17660k_deinit(h2_fm17660k_t *fm17660k);

const h2_pal_nfc_api_t *h2_fm17660k_reader_api(
    h2_fm17660k_t *fm17660k);

const h2_pal_nfc_card_emulation_api_t *h2_fm17660k_card_emulation_api(
    h2_fm17660k_t *fm17660k);

/** Reset and revalidate a controller after a transport or controller fault. */
h2_pal_result_t h2_fm17660k_recover(h2_fm17660k_t *fm17660k);

#ifdef __cplusplus
}
#endif

#endif
