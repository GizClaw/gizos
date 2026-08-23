#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_nfc_scan_nfc_reader(void *p0, h2_pal_periph_id_t p1, h2_pal_nfc_scan_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_nfc_read_nfc_data(void *p0, h2_pal_periph_id_t p1, const uint8_t *p2, uint8_t p3, h2_pal_nfc_data_type_t p4, const h2_pal_mem_api_t *p5, h2_pal_nfc_data_read_t *p6) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    (void)p5;
    (void)p6;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_nfc_vtable_t unsupported_nfc_vtable = {
    .scan_nfc_reader = unsupported_nfc_scan_nfc_reader,
    .read_nfc_data = unsupported_nfc_read_nfc_data,
};
static const h2_pal_nfc_api_t unsupported_nfc_api = { .user = NULL, .vtable = &unsupported_nfc_vtable };
const h2_pal_nfc_api_t *h2_pal_unsupported_nfc_api(void) { return &unsupported_nfc_api; }
