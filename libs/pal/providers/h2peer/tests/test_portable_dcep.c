#include "peer_connection.h"
#include "sctp.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void test_channel_type(DecpChannelType channel_type) {
    uint8_t message[64];
    size_t message_len = 0u;
    memset(message, 0xa5, sizeof(message));

    assert(peer_connection_encode_datachannel_open(
               message,
               sizeof(message),
               channel_type,
               0x1234u,
               0x01020304u,
               "packet",
               "h2",
               &message_len) == 0);
    static const uint8_t expected_tail[] = {
        0x12u, 0x34u,
        0x01u, 0x02u, 0x03u, 0x04u,
        0x00u, 0x06u,
        0x00u, 0x02u,
        'p', 'a', 'c', 'k', 'e', 't',
        'h', '2',
    };
    assert(message_len == 20u);
    assert(message[0] == DATA_CHANNEL_OPEN);
    assert(message[1] == (uint8_t)channel_type);
    assert(memcmp(message + 2, expected_tail, sizeof(expected_tail)) == 0);
}

static void test_oversized_incoming_label_is_rejected(void) {
    Sctp sctp;
    uint8_t message[12u + 128u];
    memset(&sctp, 0, sizeof(sctp));
    memset(message, 'x', sizeof(message));
    message[0] = DATA_CHANNEL_OPEN;
    message[8] = 0u;
    message[9] = 128u;
    message[10] = 0u;
    message[11] = 0u;

    sctp_parse_data_channel_open(
        &sctp, 7u, (char *)message, sizeof(message));

    assert(sctp.stream_count == 0u);
    assert(sctp.stream_table == NULL);
}

int main(void) {
    static const DecpChannelType channel_types[] = {
        DATA_CHANNEL_RELIABLE,
        DATA_CHANNEL_RELIABLE_UNORDERED,
        DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT,
        DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED,
        DATA_CHANNEL_PARTIAL_RELIABLE_TIMED,
        DATA_CHANNEL_PARTIAL_RELIABLE_TIMED_UNORDERED,
    };
    for (size_t i = 0u; i < sizeof(channel_types) / sizeof(channel_types[0]);
         ++i) {
        test_channel_type(channel_types[i]);
    }
    test_oversized_incoming_label_is_rejected();
    return 0;
}
