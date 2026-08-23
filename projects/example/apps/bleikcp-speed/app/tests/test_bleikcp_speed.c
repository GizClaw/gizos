#include "internal.h"

#include "h2/pal/core/h2_pal_errors.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static void test_request_header(void) {
    const uint64_t session_id = UINT64_C(0x0123456789abcdef);
    uint8_t header[H2_SPEED_HEADER_SIZE];
    uint64_t decoded_session = 0u;
    h2_speed_make_header(
        header, false, session_id, H2_SPEED_CHUNK_SIZE, H2_PAL_OK);
    assert(memcmp(header, "H2BS", 4u) == 0);
    assert(h2_speed_validate_header(
               header, false, &decoded_session) == H2_PAL_OK);
    assert(decoded_session == session_id);
    assert(header[8] == 0xefu);
    assert(header[15] == 0x01u);
    assert(header[16] == 0x00u);
    assert(header[17] == 0x10u);

    header[4] = 2u;
    assert(h2_speed_validate_header(
               header, false, &decoded_session) == H2_PAL_ERR_UNSUPPORTED);
    header[6] = 0u;
    header[5] = 1u;
    assert(h2_speed_validate_header(
               header, false, &decoded_session) == H2_PAL_ERR_UNSUPPORTED);
    header[5] = 0u;
    header[16] = 1u;
    assert(h2_speed_validate_header(
               header, false, &decoded_session) == H2_PAL_ERR_UNSUPPORTED);
    header[16] = 0u;
    header[20] = 1u;
    assert(h2_speed_validate_header(
               header, false, &decoded_session) == H2_PAL_ERR_UNSUPPORTED);
    header[20] = 0u;
    header[0] = 'X';
    assert(h2_speed_validate_header(
               header, false, &decoded_session) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_speed_validate_header(
               NULL, false, &decoded_session) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_speed_validate_header(
               header, false, NULL) == H2_PAL_ERR_INVALID_ARG);
    header[4] = H2_SPEED_PROTOCOL_VERSION;
    header[6] = 1u;
    assert(h2_speed_validate_header(
               header, false, &decoded_session) == H2_PAL_ERR_UNSUPPORTED);
}

static void test_response_header(void) {
    const uint64_t session_id = UINT64_C(0xfedcba9876543210);
    uint8_t header[H2_SPEED_HEADER_SIZE];
    uint64_t decoded_session = 0u;
    h2_speed_make_header(
        header, true, session_id, H2_SPEED_CHUNK_SIZE, H2_PAL_OK);
    assert(memcmp(header, "H2BR", 4u) == 0);
    assert(h2_speed_validate_header(
               header, true, &decoded_session) == H2_PAL_OK);
    assert(decoded_session == session_id);

    h2_speed_make_header(
        header, true, session_id, H2_SPEED_CHUNK_SIZE, H2_PAL_ERR_IO);
    assert(header[5] == 1u);
    assert(header[20] == (uint8_t)H2_PAL_ERR_IO);
    assert(h2_speed_validate_header(
               header, true, &decoded_session) == H2_PAL_ERR_UNSUPPORTED);

    h2_speed_make_header(
        header, true, session_id, H2_SPEED_CHUNK_SIZE, H2_PAL_OK);
    header[7] = 1u;
    assert(h2_speed_validate_header(
               header, true, &decoded_session) == H2_PAL_ERR_UNSUPPORTED);
}

static void test_payload(void) {
    uint8_t payload[H2_SPEED_CHUNK_SIZE];
    h2_speed_fill_payload(
        payload, sizeof(payload), UINT64_C(0x1122334455667788), 1u, 4096u);
    assert(h2_speed_verify_payload(
               payload, sizeof(payload), UINT64_C(0x1122334455667788),
               1u, 4096u) == H2_PAL_OK);
    payload[2048] ^= 1u;
    assert(h2_speed_verify_payload(
               payload, sizeof(payload), UINT64_C(0x1122334455667788),
               1u, 4096u) == H2_PAL_ERR_FORMAT);
    payload[2048] ^= 1u;
    assert(h2_speed_verify_payload(
               payload, sizeof(payload), UINT64_C(0x1122334455667788),
               1u, 4095u) == H2_PAL_ERR_FORMAT);
    assert(h2_speed_verify_payload(
               NULL, 1u, UINT64_C(0), 0u, 0u) == H2_PAL_ERR_INVALID_ARG);
}

static void test_statistics(void) {
    assert(fabs(h2_speed_rate_kib_s(5120u, 0u, 1000u) - 5.0) < 0.0001);
    assert(fabs(h2_speed_rate_kib_s(15360u, 5120u, 5000u) - 2.0) < 0.0001);
    assert(h2_speed_rate_kib_s(100u, 200u, 1000u) == 0.0);
    assert(h2_speed_rate_kib_s(100u, 0u, 0u) == 0.0);
    assert(fabs(h2_speed_rate_kib_s(
                    UINT64_MAX, UINT64_MAX - 1024u, 1000u) - 1.0) < 0.0001);
}

static void test_reconnect_backoff(void) {
    uint32_t backoff_ms = 0u;
    const uint32_t expected[] = {
        250u, 500u, 1000u, 2000u, 4000u, 5000u, 5000u,
    };
    for (size_t i = 0u; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        backoff_ms = h2_speed_next_backoff_ms(backoff_ms);
        assert(backoff_ms == expected[i]);
    }
}

static void test_io_retry_classification(void) {
    assert(h2_speed_io_should_retry(H2_PAL_ERR_WOULD_BLOCK));
    assert(!h2_speed_io_should_retry(H2_PAL_OK));
    assert(!h2_speed_io_should_retry(H2_PAL_ERR_CLOSED));
    assert(!h2_speed_io_should_retry(H2_PAL_ERR_FORMAT));
}

int main(void) {
    test_request_header();
    test_response_header();
    test_payload();
    test_statistics();
    test_reconnect_backoff();
    test_io_retry_classification();
    return 0;
}
