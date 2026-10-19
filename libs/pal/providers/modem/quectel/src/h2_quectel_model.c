#include "h2_quectel_internal.h"

#include <string.h>

h2_pal_result_t h2_quectel_resolve_model(h2_quectel_modem_t *modem) {
    if (modem == NULL) { return H2_PAL_ERR_INVALID_ARG; }
    if (!modem->model_checked) {
        const uint32_t generation = modem->reset_generation;
        h2_quectel_response_t response;
        h2_pal_result_t rc = h2_quectel_at_exchange(modem, "AT+CGMM", &response, 0);
        if (rc != H2_PAL_OK) { return rc; }
        if (generation != modem->reset_generation) { return H2_PAL_ERR_INVALID_STATE; }
        if (h2_quectel_response_find(&response, "ERROR") != NULL ||
            h2_quectel_response_find(&response, "+CME ERROR:") != NULL ||
            h2_quectel_response_find(&response, "+CMS ERROR:") != NULL) {
            return H2_PAL_ERR_IO;
        }
        if (response.count == 0u && !response.truncated) { return H2_PAL_ERR_UNSUPPORTED; }
        if (response.truncated || response.count != 1u) { return H2_PAL_ERR_FORMAT; }
        const char *model = response.lines[0];
        modem->family = H2_QUECTEL_MODEM_FAMILY_UNKNOWN;
        if (strcmp(model, "EC25") == 0 || strncmp(model, "EC25-", 5u) == 0) {
            modem->family = H2_QUECTEL_MODEM_FAMILY_EC25;
        } else if (strcmp(model, "EC800M") == 0 || strncmp(model, "EC800M-", 7u) == 0) {
            modem->family = H2_QUECTEL_MODEM_FAMILY_EC800M;
        }
        modem->model_checked = 1u;
        if (modem->family == H2_QUECTEL_MODEM_FAMILY_UNKNOWN) {
            modem->capabilities &= ~(uint32_t)(H2_PAL_MODEM_CAPABILITY_EMERGENCY_NUMBERS |
                H2_PAL_MODEM_CAPABILITY_CELL_LOCATE | H2_PAL_MODEM_CAPABILITY_OTA);
        } else {
            modem->capabilities |= H2_PAL_MODEM_CAPABILITY_EMERGENCY_NUMBERS;
            modem->capabilities |= H2_PAL_MODEM_CAPABILITY_OTA;
            if (modem->family == H2_QUECTEL_MODEM_FAMILY_EC800M) {
                modem->capabilities &= ~(uint32_t)H2_PAL_MODEM_CAPABILITY_CELL_LOCATE;
            } else if (h2_quectel_cell_locate_token_valid(modem->config.cell_locate_token)) {
                modem->capabilities |= H2_PAL_MODEM_CAPABILITY_CELL_LOCATE;
            }
        }
    }
    if (modem->config.profile == H2_QUECTEL_MODEM_PROFILE_EC800M_UART &&
        modem->family != H2_QUECTEL_MODEM_FAMILY_EC800M) {
        modem->capabilities &= ~(uint32_t)(H2_PAL_MODEM_CAPABILITY_EMERGENCY_NUMBERS |
            H2_PAL_MODEM_CAPABILITY_CELL_LOCATE | H2_PAL_MODEM_CAPABILITY_LOW_POWER |
            H2_PAL_MODEM_CAPABILITY_OTA);
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return modem->family == H2_QUECTEL_MODEM_FAMILY_UNKNOWN
        ? H2_PAL_ERR_UNSUPPORTED : H2_PAL_OK;
}
