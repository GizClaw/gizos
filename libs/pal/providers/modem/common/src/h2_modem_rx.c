#include "h2_modem_rx.h"

h2_pal_result_t h2_modem_rx_feed(
    h2_modem_rx_t *receiver,
    uint64_t offset,
    const uint8_t *data,
    size_t length,
    h2_modem_rx_line_fn handler,
    void *user) {
    if (receiver == NULL || handler == NULL || (data == NULL && length != 0u) ||
        length > UINT64_MAX - offset) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (offset > receiver->next_offset) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    const uint64_t end = offset + length;
    if (end <= receiver->next_offset) {
        return H2_PAL_OK;
    }
    size_t cursor = (size_t)(receiver->next_offset - offset);
    h2_pal_result_t result = H2_PAL_OK;
    for (; cursor < length; cursor++) {
        const uint8_t value = data[cursor];
        receiver->next_offset++;
        if (value == '\r' || value == '\n') {
            if (receiver->discarding == 0u && receiver->length != 0u) {
                receiver->line[receiver->length] = '\0';
                h2_pal_result_t rc = handler(user, receiver->line);
                if (result == H2_PAL_OK) {
                    result = rc;
                }
            }
            receiver->length = 0u;
            receiver->discarding = 0u;
        } else if (receiver->discarding == 0u) {
            if (value == 0u || receiver->length + 1u >= sizeof(receiver->line)) {
                receiver->discarding = 1u;
                receiver->length = 0u;
                if (result == H2_PAL_OK) {
                    result = value == 0u ? H2_PAL_ERR_FORMAT : H2_PAL_ERR_TRUNCATED;
                }
            } else {
                receiver->line[receiver->length++] = (char)value;
            }
        }
    }
    return result;
}
