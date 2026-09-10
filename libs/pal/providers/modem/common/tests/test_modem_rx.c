#include "h2_modem_rx.h"

#include <assert.h>
#include <string.h>

typedef struct received {
    unsigned count;
    h2_pal_result_t result;
} received_t;

static h2_pal_result_t receive_line(void *user, const char *line) {
    received_t *received = user;
    assert(strcmp(line, "RING") == 0);
    received->count++;
    return received->result;
}

int main(void) {
    h2_modem_rx_t receiver = {0};
    received_t received = {0};
    const uint8_t batch[] = "RING\r\nRING\r\n";
    for (size_t length = 1u; length < sizeof(batch); length++) {
        assert(h2_modem_rx_feed(&receiver, 0u, batch, length,
            receive_line, &received) == H2_PAL_OK);
    }
    assert(received.count == 2u);
    assert(h2_modem_rx_feed(&receiver, 0u, batch, sizeof(batch) - 1u,
        receive_line, &received) == H2_PAL_OK);
    assert(received.count == 2u);
    for (size_t cursor = 0u; cursor < sizeof(batch) - 1u; cursor++) {
        assert(h2_modem_rx_feed(&receiver, receiver.next_offset, batch + cursor,
            1u, receive_line, &received) == H2_PAL_OK);
    }
    assert(received.count == 4u);
    assert(h2_modem_rx_feed(&receiver, receiver.next_offset + 1u, batch,
        1u, receive_line, &received) == H2_PAL_ERR_INVALID_STATE);
    received.result = H2_PAL_ERR_FULL;
    uint64_t offset = receiver.next_offset;
    assert(h2_modem_rx_feed(&receiver, offset, batch, sizeof(batch) - 1u,
        receive_line, &received) == H2_PAL_ERR_FULL);
    assert(received.count == 6u);
    assert(h2_modem_rx_feed(&receiver, offset, batch, sizeof(batch) - 1u,
        receive_line, &received) == H2_PAL_OK);
    assert(received.count == 6u);
    received.result = H2_PAL_OK;
    uint8_t oversized[H2_MODEM_URC_LINE_MAX + 6u];
    memset(oversized, 'X', sizeof(oversized));
    memcpy(oversized + H2_MODEM_URC_LINE_MAX, "\nRING\n", 6u);
    assert(h2_modem_rx_feed(&receiver, receiver.next_offset, oversized,
        sizeof(oversized), receive_line, &received) == H2_PAL_ERR_TRUNCATED);
    assert(received.count == 7u);
    const uint8_t malformed[] = {'X', 0u, 'X', '\n', 'R', 'I', 'N', 'G', '\n'};
    assert(h2_modem_rx_feed(&receiver, receiver.next_offset, malformed,
        sizeof(malformed), receive_line, &received) == H2_PAL_ERR_FORMAT);
    assert(received.count == 8u);
    return 0;
}
