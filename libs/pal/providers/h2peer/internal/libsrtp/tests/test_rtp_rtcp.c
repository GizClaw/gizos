#include "h2_libsrtp.h"

#include "h2_wolfcrypt_crypto.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_memory {
    size_t allocation_calls;
    size_t live_allocations;
    size_t fail_on_call;
} test_memory_t;

static const uint8_t test_default_key[H2_LIBSRTP_MASTER_KEY_SIZE] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};

static void *test_alloc(void *user, size_t len) {
    test_memory_t *memory = user;
    ++memory->allocation_calls;
    if (memory->fail_on_call == memory->allocation_calls) {
        return NULL;
    }
    void *pointer = malloc(len);
    if (pointer != NULL) {
        ++memory->live_allocations;
    }
    return pointer;
}

static void *test_realloc(void *user, void *ptr, size_t len) {
    test_memory_t *memory = user;
    ++memory->allocation_calls;
    if (memory->fail_on_call == memory->allocation_calls) {
        return NULL;
    }
    void *pointer = realloc(ptr, len);
    if (ptr == NULL && pointer != NULL) {
        ++memory->live_allocations;
    }
    return pointer;
}

static void test_free(void *user, void *ptr) {
    test_memory_t *memory = user;
    if (ptr != NULL) {
        assert(memory->live_allocations > 0u);
        --memory->live_allocations;
    }
    free(ptr);
}

static int test_entropy(void *user, uint8_t *out, size_t len) {
    uint32_t *state = user;
    size_t index;
    for (index = 0u; index < len; ++index) {
        *state = *state * 1664525u + 1013904223u;
        out[index] = (uint8_t)(*state >> 24u);
    }
    return H2_PAL_OK;
}

static void create_pair(
    h2_libsrtp_profile_t profile,
    h2_libsrtp_ssrc_policy_t ssrc_policy,
    const uint8_t *salt,
    size_t salt_len,
    h2_libsrtp_session_t **out_sender,
    h2_libsrtp_session_t **out_receiver) {
    h2_libsrtp_session_config_t config = {
        .profile = profile,
        .direction = H2_LIBSRTP_DIRECTION_OUTBOUND,
        .ssrc_policy = ssrc_policy,
        .ssrc = 0x11223344u,
        .master_key = test_default_key,
        .master_key_len = sizeof(test_default_key),
        .master_salt = salt,
        .master_salt_len = salt_len,
    };
    assert(h2_libsrtp_session_create(&config, out_sender) == H2_PAL_OK);
    config.direction = H2_LIBSRTP_DIRECTION_INBOUND;
    assert(h2_libsrtp_session_create(&config, out_receiver) == H2_PAL_OK);
}

static void test_profile(
    h2_libsrtp_profile_t profile,
    h2_libsrtp_ssrc_policy_t ssrc_policy,
    const uint8_t *salt,
    size_t salt_len,
    const test_memory_t *memory) {
    static const uint8_t rtp_plain[] = {
        0x80, 0x60, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x11, 0x22, 0x33, 0x44, 0xde, 0xad, 0xbe, 0xef,
    };
    static const uint8_t rtcp_plain[] = {
        0x80, 0xc8, 0x00, 0x01, 0x11, 0x22, 0x33, 0x44,
    };
    h2_libsrtp_session_t *sender = NULL;
    h2_libsrtp_session_t *receiver = NULL;
    uint8_t packet[128];
    uint8_t protected_copy[128];
    uint8_t rejected_copy[128];
    size_t len;
    size_t protected_len;
    size_t allocation_calls;

    create_pair(
        profile, ssrc_policy, salt, salt_len, &sender, &receiver);
    allocation_calls = memory->allocation_calls;
    memcpy(packet, rtp_plain, sizeof(rtp_plain));
    len = sizeof(rtp_plain);
    assert(h2_libsrtp_protect_rtp(
               sender, packet, sizeof(packet), &len) == H2_PAL_OK);
    assert(len > sizeof(rtp_plain));
    protected_len = len;
    memcpy(protected_copy, packet, len);
    assert(h2_libsrtp_unprotect_rtp(
               receiver, packet, sizeof(packet), &len) == H2_PAL_OK);
    assert(len == sizeof(rtp_plain));
    assert(memcmp(packet, rtp_plain, len) == 0);

    memcpy(packet, protected_copy, protected_len);
    memcpy(rejected_copy, packet, protected_len);
    len = protected_len;
    assert(h2_libsrtp_unprotect_rtp(
               receiver, packet, sizeof(packet), &len) == H2_PAL_ERR_FORMAT);
    assert(len == protected_len);
    assert(memcmp(packet, rejected_copy, len) == 0);
    assert(memory->allocation_calls == allocation_calls);

    memcpy(packet, rtcp_plain, sizeof(rtcp_plain));
    len = sizeof(rtcp_plain);
    assert(h2_libsrtp_protect_rtcp(
               sender, packet, sizeof(packet), &len) == H2_PAL_OK);
    assert(len > sizeof(rtcp_plain));
    packet[len - 1u] ^= 0x01u;
    memcpy(rejected_copy, packet, len);
    protected_len = len;
    assert(h2_libsrtp_unprotect_rtcp(
               receiver, packet, sizeof(packet), &len) == H2_PAL_ERR_FORMAT);
    assert(len == protected_len);
    assert(memcmp(packet, rejected_copy, len) == 0);
    assert(memory->allocation_calls == allocation_calls);

    assert(h2_libsrtp_protect_rtp(
               receiver, packet, sizeof(packet), &len) ==
           H2_PAL_ERR_INVALID_STATE);
    h2_libsrtp_session_destroy(&sender);
    h2_libsrtp_session_destroy(&receiver);
}

static void test_known_vector(
    h2_libsrtp_profile_t profile,
    const uint8_t key[H2_LIBSRTP_MASTER_KEY_SIZE],
    const uint8_t *salt,
    size_t salt_len,
    const uint8_t *rtp_plain,
    size_t rtp_plain_len,
    const uint8_t *rtp_protected,
    size_t rtp_protected_len,
    const uint8_t *rtcp_plain,
    size_t rtcp_plain_len,
    const uint8_t *rtcp_protected,
    size_t rtcp_protected_len,
    const test_memory_t *memory) {
    h2_libsrtp_session_config_t config = {
        .profile = profile,
        .direction = H2_LIBSRTP_DIRECTION_OUTBOUND,
        .ssrc_policy = H2_LIBSRTP_SSRC_SPECIFIC,
        .ssrc = 0xcafebabeu,
        .master_key = key,
        .master_key_len = H2_LIBSRTP_MASTER_KEY_SIZE,
        .master_salt = salt,
        .master_salt_len = salt_len,
    };
    h2_libsrtp_session_t *sender = NULL;
    h2_libsrtp_session_t *receiver = NULL;
    uint8_t packet[64];
    size_t len;
    size_t allocation_calls;

    assert(h2_libsrtp_session_create(&config, &sender) == H2_PAL_OK);
    config.direction = H2_LIBSRTP_DIRECTION_INBOUND;
    assert(h2_libsrtp_session_create(&config, &receiver) == H2_PAL_OK);
    allocation_calls = memory->allocation_calls;

    memcpy(packet, rtp_plain, rtp_plain_len);
    len = rtp_plain_len;
    assert(h2_libsrtp_protect_rtp(
               sender, packet, sizeof(packet), &len) == H2_PAL_OK);
    assert(len == rtp_protected_len);
    assert(memcmp(packet, rtp_protected, len) == 0);
    memcpy(packet, rtp_protected, rtp_protected_len);
    len = rtp_protected_len;
    assert(h2_libsrtp_unprotect_rtp(
               receiver, packet, sizeof(packet), &len) == H2_PAL_OK);
    assert(len == rtp_plain_len);
    assert(memcmp(packet, rtp_plain, len) == 0);

    memcpy(packet, rtcp_plain, rtcp_plain_len);
    len = rtcp_plain_len;
    assert(h2_libsrtp_protect_rtcp(
               sender, packet, sizeof(packet), &len) == H2_PAL_OK);
    assert(len == rtcp_protected_len);
    assert(memcmp(packet, rtcp_protected, len) == 0);
    memcpy(packet, rtcp_protected, rtcp_protected_len);
    len = rtcp_protected_len;
    assert(h2_libsrtp_unprotect_rtcp(
               receiver, packet, sizeof(packet), &len) == H2_PAL_OK);
    assert(len == rtcp_plain_len);
    assert(memcmp(packet, rtcp_plain, len) == 0);
    assert(memory->allocation_calls == allocation_calls);

    h2_libsrtp_session_destroy(&sender);
    h2_libsrtp_session_destroy(&receiver);
}

static void test_official_vectors(
    const uint8_t aes_cm_salt[H2_LIBSRTP_AES_CM_SALT_SIZE],
    const uint8_t gcm_salt[H2_LIBSRTP_AEAD_SALT_SIZE],
    const test_memory_t *memory) {
    static const uint8_t aes_cm_key[H2_LIBSRTP_MASTER_KEY_SIZE] = {
        0xe1, 0xf9, 0x7a, 0x0d, 0x3e, 0x01, 0x8b, 0xe0,
        0xd6, 0x4f, 0xa3, 0x2c, 0x06, 0xde, 0x41, 0x39,
    };
    static const uint8_t rtp_plain[28] = {
        0x80, 0x0f, 0x12, 0x34, 0xde, 0xca, 0xfb, 0xad,
        0xca, 0xfe, 0xba, 0xbe, 0xab, 0xab, 0xab, 0xab,
        0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab,
        0xab, 0xab, 0xab, 0xab,
    };
    static const uint8_t rtcp_plain[24] = {
        0x81, 0xc8, 0x00, 0x0b, 0xca, 0xfe, 0xba, 0xbe,
        0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab,
        0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab,
    };
    static const uint8_t aes_cm_rtp[38] = {
        0x80, 0x0f, 0x12, 0x34, 0xde, 0xca, 0xfb, 0xad,
        0xca, 0xfe, 0xba, 0xbe, 0x4e, 0x55, 0xdc, 0x4c,
        0xe7, 0x99, 0x78, 0xd8, 0x8c, 0xa4, 0xd2, 0x15,
        0x94, 0x9d, 0x24, 0x02, 0xb7, 0x8d, 0x6a, 0xcc,
        0x99, 0xea, 0x17, 0x9b, 0x8d, 0xbb,
    };
    static const uint8_t aes_cm_rtcp[38] = {
        0x81, 0xc8, 0x00, 0x0b, 0xca, 0xfe, 0xba, 0xbe,
        0x71, 0x28, 0x03, 0x5b, 0xe4, 0x87, 0xb9, 0xbd,
        0xbe, 0xf8, 0x90, 0x41, 0xf9, 0x77, 0xa5, 0xa8,
        0x80, 0x00, 0x00, 0x01, 0x99, 0x3e, 0x08, 0xcd,
        0x54, 0xd6, 0xc1, 0x23, 0x07, 0x98,
    };
    static const uint8_t gcm_rtp[44] = {
        0x80, 0x0f, 0x12, 0x34, 0xde, 0xca, 0xfb, 0xad,
        0xca, 0xfe, 0xba, 0xbe, 0xc5, 0x00, 0x2e, 0xde,
        0x04, 0xcf, 0xdd, 0x2e, 0xb9, 0x11, 0x59, 0xe0,
        0x88, 0x0a, 0xa0, 0x6e, 0xd2, 0x97, 0x68, 0x26,
        0xf7, 0x96, 0xb2, 0x01, 0xdf, 0x31, 0x31, 0xa1,
        0x27, 0xe8, 0xa3, 0x92,
    };
    static const uint8_t gcm_rtcp[44] = {
        0x81, 0xc8, 0x00, 0x0b, 0xca, 0xfe, 0xba, 0xbe,
        0xc9, 0x8b, 0x8b, 0x5d, 0xf0, 0x39, 0x2a, 0x55,
        0x85, 0x2b, 0x6c, 0x21, 0xac, 0x8e, 0x70, 0x25,
        0xc5, 0x2c, 0x6f, 0xbe, 0xa2, 0xb3, 0xb4, 0x46,
        0xea, 0x31, 0x12, 0x3b, 0xa8, 0x8c, 0xe6, 0x1e,
        0x80, 0x00, 0x00, 0x01,
    };

    test_known_vector(
        H2_LIBSRTP_PROFILE_AES128_CM_SHA1_80, aes_cm_key,
        aes_cm_salt, H2_LIBSRTP_AES_CM_SALT_SIZE,
        rtp_plain, sizeof(rtp_plain), aes_cm_rtp, sizeof(aes_cm_rtp),
        rtcp_plain, sizeof(rtcp_plain), aes_cm_rtcp, sizeof(aes_cm_rtcp),
        memory);
    test_known_vector(
        H2_LIBSRTP_PROFILE_AEAD_AES_128_GCM, test_default_key,
        gcm_salt, H2_LIBSRTP_AEAD_SALT_SIZE,
        rtp_plain, sizeof(rtp_plain), gcm_rtp, sizeof(gcm_rtp),
        rtcp_plain, sizeof(rtcp_plain), gcm_rtcp, sizeof(gcm_rtcp),
        memory);
}

static void test_rollover(
    const uint8_t salt[H2_LIBSRTP_AES_CM_SALT_SIZE],
    const test_memory_t *memory) {
    static const uint16_t sequences[5] = {65534u, 65535u, 0u, 1u, 2u};
    static const size_t receive_order[5] = {0u, 2u, 4u, 3u, 1u};
    h2_libsrtp_session_t *sender = NULL;
    h2_libsrtp_session_t *receiver = NULL;
    uint8_t packets[5][64];
    uint8_t plaintext[5][16];
    size_t lengths[5];
    size_t allocation_calls;
    size_t index;

    create_pair(
        H2_LIBSRTP_PROFILE_AES128_CM_SHA1_80,
        H2_LIBSRTP_SSRC_SPECIFIC, salt, H2_LIBSRTP_AES_CM_SALT_SIZE,
        &sender, &receiver);
    allocation_calls = memory->allocation_calls;
    for (index = 0u; index < 5u; ++index) {
        memset(packets[index], 0xab, 16u);
        packets[index][0] = 0x80u;
        packets[index][1] = 0x60u;
        packets[index][2] = (uint8_t)(sequences[index] >> 8u);
        packets[index][3] = (uint8_t)sequences[index];
        packets[index][4] = 0u;
        packets[index][5] = 0u;
        packets[index][6] = 0u;
        packets[index][7] = (uint8_t)index;
        packets[index][8] = 0x11u;
        packets[index][9] = 0x22u;
        packets[index][10] = 0x33u;
        packets[index][11] = 0x44u;
        memcpy(plaintext[index], packets[index], sizeof(plaintext[index]));
        lengths[index] = sizeof(plaintext[index]);
        assert(h2_libsrtp_protect_rtp(
                   sender, packets[index], sizeof(packets[index]),
                   &lengths[index]) == H2_PAL_OK);
    }
    for (index = 0u; index < 5u; ++index) {
        size_t packet_index = receive_order[index];
        assert(h2_libsrtp_unprotect_rtp(
                   receiver, packets[packet_index], sizeof(packets[packet_index]),
                   &lengths[packet_index]) == H2_PAL_OK);
        assert(lengths[packet_index] == sizeof(plaintext[packet_index]));
        assert(memcmp(
                   packets[packet_index], plaintext[packet_index],
                   lengths[packet_index]) == 0);
    }
    assert(memory->allocation_calls == allocation_calls);
    h2_libsrtp_session_destroy(&sender);
    h2_libsrtp_session_destroy(&receiver);
}

static void test_allocation_failures(
    const uint8_t salt[H2_LIBSRTP_AES_CM_SALT_SIZE],
    test_memory_t *memory) {
    h2_libsrtp_session_config_t config = {
        .profile = H2_LIBSRTP_PROFILE_AES128_CM_SHA1_80,
        .direction = H2_LIBSRTP_DIRECTION_OUTBOUND,
        .ssrc_policy = H2_LIBSRTP_SSRC_ANY,
        .ssrc = 0u,
        .master_key = test_default_key,
        .master_key_len = sizeof(test_default_key),
        .master_salt = salt,
        .master_salt_len = H2_LIBSRTP_AES_CM_SALT_SIZE,
    };
    size_t baseline = memory->live_allocations;
    size_t offset;

    for (offset = 1u; offset <= 4u; ++offset) {
        h2_libsrtp_session_t *session = (h2_libsrtp_session_t *)1;
        memory->fail_on_call = memory->allocation_calls + offset;
        assert(h2_libsrtp_session_create(&config, &session) ==
               H2_PAL_ERR_NO_MEMORY);
        assert(session == NULL);
        memory->fail_on_call = 0u;
        assert(memory->live_allocations == baseline);
    }
}

int main(void) {
    static const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = test_alloc,
        .realloc = test_realloc,
        .free = test_free,
    };
    static const uint8_t aes_cm_salt[H2_LIBSRTP_AES_CM_SALT_SIZE] = {
        0x0e, 0xc6, 0x75, 0xad, 0x49, 0x8a, 0xfe,
        0xeb, 0xb6, 0x96, 0x0b, 0x3a, 0xab, 0xe6,
    };
    static const uint8_t gcm_salt[H2_LIBSRTP_AEAD_SALT_SIZE] = {
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5,
        0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab,
    };
    uint32_t entropy_state = 3u;
    test_memory_t memory = {0};
    h2_wolfcrypt_crypto_config_t crypto_config = {
        .entropy_user = &entropy_state,
        .entropy = test_entropy,
    };
    h2_libsrtp_config_t config;

    assert(h2_wolfcrypt_crypto_init(&crypto_config) == H2_PAL_OK);
    config.mem.user = &memory;
    config.mem.vtable = &mem_vtable;
    config.crypto = *h2_wolfcrypt_crypto_api();
    config.max_packet_size = 2048u;
    assert(h2_libsrtp_init(&config) == H2_PAL_OK);
    assert(h2_libsrtp_init(&config) == H2_PAL_OK);
    ++config.max_packet_size;
    assert(h2_libsrtp_init(&config) == H2_PAL_ERR_INVALID_STATE);
    --config.max_packet_size;
    test_allocation_failures(aes_cm_salt, &memory);
    test_official_vectors(aes_cm_salt, gcm_salt, &memory);

    test_profile(
        H2_LIBSRTP_PROFILE_AES128_CM_SHA1_80,
        H2_LIBSRTP_SSRC_SPECIFIC,
        aes_cm_salt, sizeof(aes_cm_salt), &memory);
    test_profile(
        H2_LIBSRTP_PROFILE_AEAD_AES_128_GCM,
        H2_LIBSRTP_SSRC_SPECIFIC,
        gcm_salt, sizeof(gcm_salt), &memory);
    test_profile(
        H2_LIBSRTP_PROFILE_AES128_CM_SHA1_80,
        H2_LIBSRTP_SSRC_ANY,
        aes_cm_salt, sizeof(aes_cm_salt), &memory);
    test_profile(
        H2_LIBSRTP_PROFILE_AEAD_AES_128_GCM,
        H2_LIBSRTP_SSRC_ANY,
        gcm_salt, sizeof(gcm_salt), &memory);
    test_rollover(aes_cm_salt, &memory);

    assert(h2_libsrtp_deinit() == H2_PAL_OK);
    {
        h2_libsrtp_session_t *sender = NULL;
        h2_libsrtp_session_t *receiver = NULL;
        create_pair(
            H2_LIBSRTP_PROFILE_AES128_CM_SHA1_80,
            H2_LIBSRTP_SSRC_SPECIFIC,
            aes_cm_salt, sizeof(aes_cm_salt), &sender, &receiver);
        assert(h2_libsrtp_deinit() == H2_PAL_ERR_INVALID_STATE);
        h2_libsrtp_session_destroy(&sender);
        h2_libsrtp_session_destroy(&receiver);
    }
    assert(h2_libsrtp_deinit() == H2_PAL_OK);
    assert(h2_libsrtp_deinit() == H2_PAL_OK);
    assert(memory.live_allocations == 0u);
    h2_wolfcrypt_crypto_deinit();
    return 0;
}
