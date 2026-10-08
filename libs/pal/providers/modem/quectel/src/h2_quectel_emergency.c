#include "h2_quectel_internal.h"

h2_pal_result_t h2_quectel_get_emergency_numbers(void *user, uint32_t timeout_ms,
    h2_pal_modem_emergency_number_t *out_numbers, size_t capacity, size_t *out_count) {
    h2_quectel_modem_t *modem = user;
    if (out_count != NULL) { *out_count = 0u; }
    if (out_numbers == NULL || out_count == NULL || capacity == 0u ||
        capacity > SIZE_MAX / sizeof(*out_numbers)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_quectel_operation_begin(modem);
    if (rc != H2_PAL_OK) { return rc; }
    if (!modem->opened) { return h2_quectel_operation_end(modem, H2_PAL_ERR_CLOSED); }
    rc = h2_quectel_modem_prepare(modem);
    if (rc == H2_PAL_OK) { rc = h2_quectel_resolve_model(modem); }
    if (rc == H2_PAL_OK) {
        const uint32_t reset_generation = modem->reset_generation;
        const uint32_t sim_generation = modem->sim_generation;
        rc = modem->family == H2_QUECTEL_MODEM_FAMILY_EC800M
            ? h2_quectel_ec800m_emergency_numbers(modem, timeout_ms, out_numbers, capacity, out_count)
            : h2_quectel_ec25_emergency_numbers(modem, timeout_ms, out_numbers, capacity, out_count);
        /* Also reject invalidation URCs embedded in synchronous reply text. */
        if (rc == H2_PAL_OK && (reset_generation != modem->reset_generation ||
            sim_generation != modem->sim_generation)) { rc = H2_PAL_ERR_INVALID_STATE; }
    }
    rc = h2_quectel_operation_end(modem, rc);
    if (rc != H2_PAL_OK) { *out_count = 0u; }
    return rc;
}
