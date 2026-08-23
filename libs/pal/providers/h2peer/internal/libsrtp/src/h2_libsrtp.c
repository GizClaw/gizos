#include "config.h"
#include "h2_libsrtp_internal.h"

#include <limits.h>
#include <string.h>

#define H2_LIBSRTP_ANY_OPERATION_ARENA_SIZE 8192u

h2_libsrtp_state_t h2_libsrtp_state;
static _Thread_local h2_pal_result_t h2_libsrtp_crypto_result;

void h2_libsrtp_secure_zero(void *data, size_t len) {
    volatile uint8_t *bytes = data;
    while (len > 0u) {
        *bytes++ = 0u;
        --len;
    }
}

void h2_libsrtp_record_crypto_result(h2_pal_result_t result) {
    if (result != H2_PAL_OK) {
        h2_libsrtp_crypto_result = result;
    }
}

h2_pal_result_t h2_libsrtp_take_crypto_result(void) {
    h2_pal_result_t result = h2_libsrtp_crypto_result;
    h2_libsrtp_crypto_result = H2_PAL_OK;
    return result;
}

static int h2_libsrtp_mem_complete(const h2_pal_mem_api_t *mem) {
    return mem != NULL && mem->vtable != NULL &&
           mem->vtable->alloc != NULL && mem->vtable->realloc != NULL &&
           mem->vtable->free != NULL;
}

static int h2_libsrtp_crypto_complete(const h2_pal_crypto_api_t *crypto) {
    const h2_pal_crypto_vtable_t *vtable;
    if (crypto == NULL || crypto->vtable == NULL) {
        return 0;
    }
    vtable = crypto->vtable;
    return vtable->random != NULL &&
           vtable->x25519_keypair_generate != NULL &&
           vtable->x25519_public_key_from_private != NULL &&
           vtable->x25519_shared_secret != NULL &&
           vtable->hkdf_sha256 != NULL && vtable->aead_seal != NULL &&
           vtable->aead_open != NULL && vtable->aes_ctr_xor != NULL &&
           vtable->md5 != NULL && vtable->hmac_sha1 != NULL &&
           vtable->p256_keypair_from_private != NULL &&
           vtable->p256_keypair_generate != NULL &&
           vtable->p256_public_key_validate != NULL &&
           vtable->ecdsa_p256_sha256_sign != NULL &&
           vtable->ecdsa_p256_sha256_verify != NULL;
}

h2_pal_result_t h2_libsrtp_init(const h2_libsrtp_config_t *config) {
    srtp_err_status_t status;
    h2_pal_result_t crypto_result;
    if (config == NULL || !h2_libsrtp_mem_complete(&config->mem) ||
        !h2_libsrtp_crypto_complete(&config->crypto) ||
        config->max_packet_size == 0u ||
        config->max_packet_size > (size_t)INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (h2_libsrtp_state.ready) {
        if (h2_libsrtp_state.mem.user != config->mem.user ||
            h2_libsrtp_state.mem.vtable != config->mem.vtable ||
            h2_libsrtp_state.crypto.user != config->crypto.user ||
            h2_libsrtp_state.crypto.vtable != config->crypto.vtable ||
            h2_libsrtp_state.max_packet_size != config->max_packet_size) {
            return H2_PAL_ERR_INVALID_STATE;
        }
        if (h2_libsrtp_state.owner_refs == SIZE_MAX) {
            return H2_PAL_ERR_FULL;
        }
        ++h2_libsrtp_state.owner_refs;
        return H2_PAL_OK;
    }

    memset(&h2_libsrtp_state, 0, sizeof(h2_libsrtp_state));
    h2_libsrtp_state.mem = config->mem;
    h2_libsrtp_state.crypto = config->crypto;
    h2_libsrtp_state.max_packet_size = config->max_packet_size;
    h2_libsrtp_state.owner_refs = 1u;
    h2_libsrtp_state.ready = 1;
    h2_libsrtp_take_crypto_result();
    status = srtp_init();
    crypto_result = h2_libsrtp_take_crypto_result();
    if (status == srtp_err_status_ok) {
        return H2_PAL_OK;
    }
    (void)srtp_shutdown();
    memset(&h2_libsrtp_state, 0, sizeof(h2_libsrtp_state));
    return crypto_result != H2_PAL_OK ? crypto_result : H2_PAL_ERR_IO;
}

h2_pal_result_t h2_libsrtp_deinit(void) {
    if (!h2_libsrtp_state.ready) {
        return H2_PAL_OK;
    }
    if (h2_libsrtp_state.owner_refs > 1u) {
        --h2_libsrtp_state.owner_refs;
        return H2_PAL_OK;
    }
    if (h2_libsrtp_state.live_sessions != 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (srtp_shutdown() != srtp_err_status_ok) {
        return H2_PAL_ERR_IO;
    }
    memset(&h2_libsrtp_state, 0, sizeof(h2_libsrtp_state));
    h2_libsrtp_take_crypto_result();
    return H2_PAL_OK;
}

static h2_pal_result_t h2_libsrtp_validate_session_config(
    const h2_libsrtp_session_config_t *config,
    size_t *out_salt_len) {
    if (config == NULL || config->master_key == NULL ||
        config->master_salt == NULL ||
        config->master_key_len != H2_LIBSRTP_MASTER_KEY_SIZE ||
        (config->direction != H2_LIBSRTP_DIRECTION_INBOUND &&
         config->direction != H2_LIBSRTP_DIRECTION_OUTBOUND) ||
        (config->ssrc_policy != H2_LIBSRTP_SSRC_SPECIFIC &&
         config->ssrc_policy != H2_LIBSRTP_SSRC_ANY)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    switch (config->profile) {
    case H2_LIBSRTP_PROFILE_AES128_CM_SHA1_80:
        *out_salt_len = H2_LIBSRTP_AES_CM_SALT_SIZE;
        break;
    case H2_LIBSRTP_PROFILE_AEAD_AES_128_GCM:
        *out_salt_len = H2_LIBSRTP_AEAD_SALT_SIZE;
        break;
    default:
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return config->master_salt_len == *out_salt_len
               ? H2_PAL_OK
               : H2_PAL_ERR_INVALID_ARG;
}

static void h2_libsrtp_set_policy(
    const h2_libsrtp_session_config_t *config,
    h2_libsrtp_session_t *session,
    srtp_policy_t *policy) {
    memset(policy, 0, sizeof(*policy));
    if (config->profile == H2_LIBSRTP_PROFILE_AES128_CM_SHA1_80) {
        srtp_crypto_policy_set_rtp_default(&policy->rtp);
        srtp_crypto_policy_set_rtcp_default(&policy->rtcp);
    } else {
        srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy->rtp);
        srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy->rtcp);
    }
    if (config->ssrc_policy == H2_LIBSRTP_SSRC_SPECIFIC) {
        policy->ssrc.type = ssrc_specific;
        policy->ssrc.value = config->ssrc;
    } else {
        policy->ssrc.type =
            config->direction == H2_LIBSRTP_DIRECTION_INBOUND
                ? ssrc_any_inbound
                : ssrc_any_outbound;
    }
    policy->key = session->key_material;
    policy->window_size = 128u;
    policy->allow_repeat_tx = 0;
}

h2_pal_result_t h2_libsrtp_session_create(
    const h2_libsrtp_session_config_t *config,
    h2_libsrtp_session_t **out_session) {
    h2_libsrtp_session_t *session;
    srtp_policy_t policy;
    size_t salt_len = 0u;
    h2_pal_result_t result;
    srtp_err_status_t status;
    if (out_session != NULL) {
        *out_session = NULL;
    }
    if (out_session == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!h2_libsrtp_state.ready) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    result = h2_libsrtp_validate_session_config(config, &salt_len);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (h2_libsrtp_state.live_sessions == SIZE_MAX) {
        return H2_PAL_ERR_FULL;
    }
    session = h2_pal_mem_alloc(&h2_libsrtp_state.mem, sizeof(*session));
    if (session == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(session, 0, sizeof(*session));
    session->scratch = h2_pal_mem_alloc(
        &h2_libsrtp_state.mem, h2_libsrtp_state.max_packet_size);
    if (session->scratch == NULL) {
        h2_pal_mem_free(&h2_libsrtp_state.mem, session);
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (config->ssrc_policy == H2_LIBSRTP_SSRC_ANY) {
        session->operation_arena = h2_pal_mem_alloc(
            &h2_libsrtp_state.mem, H2_LIBSRTP_ANY_OPERATION_ARENA_SIZE);
        if (session->operation_arena == NULL) {
            h2_pal_mem_free(&h2_libsrtp_state.mem, session->scratch);
            h2_pal_mem_free(&h2_libsrtp_state.mem, session);
            return H2_PAL_ERR_NO_MEMORY;
        }
        session->operation_arena_capacity =
            H2_LIBSRTP_ANY_OPERATION_ARENA_SIZE;
    }
    session->direction = config->direction;
    session->profile = config->profile;
    memcpy(
        session->key_material, config->master_key,
        H2_LIBSRTP_MASTER_KEY_SIZE);
    memcpy(
        session->key_material + H2_LIBSRTP_MASTER_KEY_SIZE,
        config->master_salt, salt_len);
    h2_libsrtp_set_policy(config, session, &policy);
    h2_libsrtp_take_crypto_result();
    status = srtp_create(&session->upstream, &policy);
    result = h2_libsrtp_take_crypto_result();
    if (status != srtp_err_status_ok) {
        h2_libsrtp_secure_zero(
            session->operation_arena, session->operation_arena_capacity);
        h2_pal_mem_free(
            &h2_libsrtp_state.mem, session->operation_arena);
        h2_libsrtp_secure_zero(
            session->scratch, h2_libsrtp_state.max_packet_size);
        h2_pal_mem_free(&h2_libsrtp_state.mem, session->scratch);
        h2_libsrtp_secure_zero(session, sizeof(*session));
        h2_pal_mem_free(&h2_libsrtp_state.mem, session);
        if (result != H2_PAL_OK) {
            return result;
        }
        return status == srtp_err_status_alloc_fail
                   ? H2_PAL_ERR_NO_MEMORY
                   : H2_PAL_ERR_IO;
    }
    ++h2_libsrtp_state.live_sessions;
    *out_session = session;
    return H2_PAL_OK;
}

void h2_libsrtp_session_destroy(h2_libsrtp_session_t **session) {
    h2_libsrtp_session_t *owned;
    if (session == NULL || *session == NULL) {
        return;
    }
    owned = *session;
    *session = NULL;
    if (owned->upstream != NULL) {
        h2_libsrtp_allocator_enter(owned);
        (void)srtp_dealloc(owned->upstream);
        h2_libsrtp_allocator_leave();
    }
    h2_libsrtp_secure_zero(
        owned->operation_arena, owned->operation_arena_capacity);
    h2_pal_mem_free(&h2_libsrtp_state.mem, owned->operation_arena);
    h2_libsrtp_secure_zero(
        owned->scratch, h2_libsrtp_state.max_packet_size);
    h2_pal_mem_free(&h2_libsrtp_state.mem, owned->scratch);
    h2_libsrtp_secure_zero(owned, sizeof(*owned));
    h2_pal_mem_free(&h2_libsrtp_state.mem, owned);
    if (h2_libsrtp_state.live_sessions > 0u) {
        --h2_libsrtp_state.live_sessions;
    }
}

static h2_pal_result_t h2_libsrtp_translate_operation_error(
    srtp_err_status_t status) {
    h2_pal_result_t crypto_result = h2_libsrtp_take_crypto_result();
    if (crypto_result != H2_PAL_OK) {
        return crypto_result;
    }
    switch (status) {
    case srtp_err_status_auth_fail:
    case srtp_err_status_replay_fail:
    case srtp_err_status_replay_old:
    case srtp_err_status_no_ctx:
    case srtp_err_status_parse_err:
    case srtp_err_status_bad_mki:
    case srtp_err_status_pkt_idx_old:
    case srtp_err_status_pkt_idx_adv:
    case srtp_err_status_bad_param:
        return H2_PAL_ERR_FORMAT;
    case srtp_err_status_alloc_fail:
        return H2_PAL_ERR_NO_MEMORY;
    case srtp_err_status_no_such_op:
        return H2_PAL_ERR_UNSUPPORTED;
    default:
        return H2_PAL_ERR_IO;
    }
}

typedef srtp_err_status_t (*h2_libsrtp_operation_fn)(
    srtp_t session, void *packet, int *len);

static h2_pal_result_t h2_libsrtp_operate(
    h2_libsrtp_session_t *session,
    h2_libsrtp_direction_t required_direction,
    h2_libsrtp_operation_fn operation,
    size_t overhead,
    uint8_t *packet,
    size_t capacity,
    size_t *inout_len) {
    srtp_err_status_t status;
    int packet_len;
    if (session == NULL || packet == NULL || inout_len == NULL ||
        *inout_len > capacity || *inout_len > (size_t)INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!h2_libsrtp_state.ready || session->direction != required_direction) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (*inout_len > h2_libsrtp_state.max_packet_size ||
        overhead > h2_libsrtp_state.max_packet_size - *inout_len) {
        return H2_PAL_ERR_NO_SPACE;
    }
    if (capacity < *inout_len + overhead) {
        return H2_PAL_ERR_NO_SPACE;
    }
    memcpy(session->scratch, packet, *inout_len);
    packet_len = (int)*inout_len;
    h2_libsrtp_take_crypto_result();
    h2_libsrtp_allocator_enter(session);
    status = operation(session->upstream, session->scratch, &packet_len);
    h2_libsrtp_allocator_leave();
    if (status != srtp_err_status_ok) {
        h2_libsrtp_secure_zero(
            session->scratch, h2_libsrtp_state.max_packet_size);
        return h2_libsrtp_translate_operation_error(status);
    }
    h2_libsrtp_take_crypto_result();
    if (packet_len < 0 || (size_t)packet_len > capacity ||
        (size_t)packet_len > h2_libsrtp_state.max_packet_size) {
        h2_libsrtp_secure_zero(
            session->scratch, h2_libsrtp_state.max_packet_size);
        return H2_PAL_ERR_IO;
    }
    memcpy(packet, session->scratch, (size_t)packet_len);
    *inout_len = (size_t)packet_len;
    h2_libsrtp_secure_zero(
        session->scratch, h2_libsrtp_state.max_packet_size);
    return H2_PAL_OK;
}

static size_t h2_libsrtp_rtp_overhead(const h2_libsrtp_session_t *session) {
    return session->profile == H2_LIBSRTP_PROFILE_AES128_CM_SHA1_80
               ? 10u
               : 16u;
}

static size_t h2_libsrtp_rtcp_overhead(const h2_libsrtp_session_t *session) {
    return h2_libsrtp_rtp_overhead(session) + 4u;
}

h2_pal_result_t h2_libsrtp_protect_rtp(
    h2_libsrtp_session_t *session,
    uint8_t *packet,
    size_t capacity,
    size_t *inout_len) {
    return h2_libsrtp_operate(
        session, H2_LIBSRTP_DIRECTION_OUTBOUND, srtp_protect,
        session != NULL ? h2_libsrtp_rtp_overhead(session) : 0u,
        packet, capacity, inout_len);
}

h2_pal_result_t h2_libsrtp_unprotect_rtp(
    h2_libsrtp_session_t *session,
    uint8_t *packet,
    size_t capacity,
    size_t *inout_len) {
    return h2_libsrtp_operate(
        session, H2_LIBSRTP_DIRECTION_INBOUND, srtp_unprotect, 0u,
        packet, capacity, inout_len);
}

h2_pal_result_t h2_libsrtp_protect_rtcp(
    h2_libsrtp_session_t *session,
    uint8_t *packet,
    size_t capacity,
    size_t *inout_len) {
    return h2_libsrtp_operate(
        session, H2_LIBSRTP_DIRECTION_OUTBOUND, srtp_protect_rtcp,
        session != NULL ? h2_libsrtp_rtcp_overhead(session) : 0u,
        packet, capacity, inout_len);
}

h2_pal_result_t h2_libsrtp_unprotect_rtcp(
    h2_libsrtp_session_t *session,
    uint8_t *packet,
    size_t capacity,
    size_t *inout_len) {
    return h2_libsrtp_operate(
        session, H2_LIBSRTP_DIRECTION_INBOUND, srtp_unprotect_rtcp, 0u,
        packet, capacity, inout_len);
}
