#ifndef H2_LIBSRTP_INTERNAL_H
#define H2_LIBSRTP_INTERNAL_H

#include "h2_libsrtp.h"

#include "srtp.h"

typedef struct h2_libsrtp_state {
    h2_pal_mem_api_t mem;
    h2_pal_crypto_api_t crypto;
    size_t max_packet_size;
    size_t owner_refs;
    size_t live_sessions;
    int ready;
} h2_libsrtp_state_t;

struct h2_libsrtp_session {
    srtp_t upstream;
    h2_libsrtp_direction_t direction;
    h2_libsrtp_profile_t profile;
    uint8_t key_material[
        H2_LIBSRTP_MASTER_KEY_SIZE + H2_LIBSRTP_AES_CM_SALT_SIZE];
    uint8_t *scratch;
    uint8_t *operation_arena;
    size_t operation_arena_capacity;
    size_t operation_arena_used;
};

extern h2_libsrtp_state_t h2_libsrtp_state;

void h2_libsrtp_secure_zero(void *data, size_t len);
void h2_libsrtp_record_crypto_result(h2_pal_result_t result);
h2_pal_result_t h2_libsrtp_take_crypto_result(void);
void h2_libsrtp_allocator_enter(h2_libsrtp_session_t *session);
void h2_libsrtp_allocator_leave(void);

#endif
