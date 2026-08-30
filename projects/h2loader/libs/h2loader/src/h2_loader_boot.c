#include "h2_loader_boot.h"
#include "h2_loader_stage.h"

#include "h2_loader_status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

static int config_accepts_board(
    const h2_loader_config_t *config,
    const char *board) {
    return config != NULL && config->board != NULL && board != NULL &&
        (strcmp(board, config->board) == 0 ||
         (config->accepted_board_alias != NULL &&
          strcmp(board, config->accepted_board_alias) == 0));
}

#define H2_LOADER_REBOOT_REASON_DEFAULT 0u
#define H2_LOADER_UPGRADE_KEY "loader_upgrade"
#define H2_LOADER_UPGRADE_STEP_KEY "loader_upgrade_step"
#define H2_LOADER_UPGRADE_RECORD_MAX 384u
#define H2_LOADER_MFG_KEY "mfg"
#define H2_LOADER_MFG_ACCEPTANCE_REVISION_KEY "mfg_acceptance_revision"
#define H2_LOADER_MFG_RECORD_V1_FORMAT 1u
#define H2_LOADER_MFG_RECORD_V1_SIZE 16u
#define H2_LOADER_MFG_RECORD_V2_FORMAT 2u
#define H2_LOADER_MFG_RECORD_V2_SIZE 24u
#define H2_LOADER_MFG_RECORD_FORMAT 3u
#define H2_LOADER_MFG_RECORD_SIZE (4u + H2_LOADER_MFG_STEP_TOTAL)
#define H2_LOADER_COMMAND_AVAILABILITY_INITIALIZED (UINT32_C(1) << 30)

static int mfg_gate_bypass_load(const h2_loader_atomic_flag_t *value) {
#if defined(_MSC_VER)
    return (int)_InterlockedCompareExchange((volatile long *)value, 0, 0);
#else
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}

static void mfg_gate_bypass_store(h2_loader_atomic_flag_t *value, int enabled) {
#if defined(_MSC_VER)
    (void)_InterlockedExchange(value, (long)enabled);
#else
    __atomic_store_n(value, enabled, __ATOMIC_RELEASE);
#endif
}

static uint32_t atomic_availability_load(
    const h2_loader_atomic_flag_t *value,
    uint32_t initialized_flag,
    uint32_t all_flags) {
    uint32_t stored;
#if defined(_MSC_VER)
    stored = (uint32_t)_InterlockedCompareExchange(
        (volatile long *)value, 0, 0);
#else
    stored = (uint32_t)__atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
    return (stored & initialized_flag) != 0u
        ? stored & all_flags
        : all_flags;
}

static void atomic_availability_store(
    h2_loader_atomic_flag_t *value,
    uint32_t flags) {
#if defined(_MSC_VER)
    (void)_InterlockedExchange((volatile long *)value, (long)flags);
#else
    __atomic_store_n(value, (int)flags, __ATOMIC_RELEASE);
#endif
}

static void atomic_availability_update(
    h2_loader_atomic_flag_t *value,
    uint32_t flags,
    uint32_t initialized_flag,
    uint32_t all_flags,
    bool available) {
    uint32_t next;

#if defined(_MSC_VER)
    uint32_t current;
    current = (uint32_t)_InterlockedCompareExchange(
        (volatile long *)value, 0, 0);
    for (;;) {
        uint32_t base = current;
        uint32_t observed;
        if ((base & initialized_flag) == 0u) {
            base = initialized_flag | all_flags;
        }
        next = available ? base | flags : base & ~flags;
        next |= initialized_flag;
        observed = (uint32_t)_InterlockedCompareExchange(
            (volatile long *)value, (long)next, (long)current);
        if (observed == current) {
            break;
        }
        current = observed;
    }
#else
    int expected = __atomic_load_n(value, __ATOMIC_ACQUIRE);
    for (;;) {
        uint32_t base = (uint32_t)expected;
        if ((base & initialized_flag) == 0u) {
            base = initialized_flag | all_flags;
        }
        next = available ? base | flags : base & ~flags;
        next |= initialized_flag;
        if (__atomic_compare_exchange_n(
                value,
                &expected,
                (int)next,
                0,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE)) {
            break;
        }
    }
#endif
}

static int mfg_gate_satisfied(
    const h2_loader_t *loader,
    const h2_loader_mfg_summary_t *summary) {
    return loader != NULL &&
        (mfg_gate_bypass_load(&loader->mfg_gate_bypass) != 0 ||
         loader->config.mfg_required_total == 0u ||
         h2_loader_mfg_summary_is_passed(
             summary, loader->config.mfg_required_total));
}

int h2_loader_set_mfg_gate_bypass(h2_loader_t *loader, int enabled) {
    if (loader == NULL || (enabled != 0 && enabled != 1)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    mfg_gate_bypass_store(&loader->mfg_gate_bypass, enabled);
    return H2_PAL_OK;
}

int h2_loader_set_implemented_commands(
    h2_loader_t *loader,
    uint32_t commands) {
    if (loader == NULL ||
        (commands & ~H2_LOADER_COMMAND_AVAILABILITY_ALL) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    atomic_availability_store(
        &loader->implemented_commands,
        H2_LOADER_COMMAND_AVAILABILITY_INITIALIZED | commands);
    return H2_PAL_OK;
}

int h2_loader_set_command_availability(
    h2_loader_t *loader,
    uint32_t flags,
    bool available) {
    if (loader == NULL || flags == 0u ||
        (flags & ~H2_LOADER_COMMAND_AVAILABILITY_ALL) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    atomic_availability_update(
        &loader->command_availability,
        flags,
        H2_LOADER_COMMAND_AVAILABILITY_INITIALIZED,
        H2_LOADER_COMMAND_AVAILABILITY_ALL,
        available);
    return H2_PAL_OK;
}

uint32_t h2_loader_get_command_availability(
    const h2_loader_t *loader,
    const h2_loader_status_t *status) {
    uint32_t public_available;

    if (loader == NULL || status == NULL) {
        return 0u;
    }
    public_available = atomic_availability_load(
        &loader->implemented_commands,
        H2_LOADER_COMMAND_AVAILABILITY_INITIALIZED,
        H2_LOADER_COMMAND_AVAILABILITY_ALL);
    if (!mfg_gate_satisfied(loader, &status->mfg)) {
        public_available &= ~H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP;
    }
    if (!status->stage.valid && !status->staged.valid) {
        public_available &= ~H2_LOADER_COMMAND_AVAILABLE_STAGE_ABORT;
    }
    return public_available & atomic_availability_load(
        &loader->command_availability,
        H2_LOADER_COMMAND_AVAILABILITY_INITIALIZED,
        H2_LOADER_COMMAND_AVAILABILITY_ALL);
}

static int require_command_available(
    const h2_loader_t *loader,
    const h2_loader_status_t *status,
    uint32_t flag) {
    return (h2_loader_get_command_availability(loader, status) & flag) != 0u
        ? H2_PAL_OK
        : H2_PAL_ERR_INVALID_STATE;
}

static uint32_t states_field(uint64_t states, uint64_t mask, uint32_t shift) {
    return (uint32_t)((states & mask) >> shift);
}

uint32_t h2_loader_states_active_role(uint64_t states) {
    return states_field(states, H2_LOADER_STATES_ACTIVE_ROLE_MASK,
                        H2_LOADER_STATES_ACTIVE_ROLE_SHIFT);
}

uint32_t h2_loader_states_boot_intent(uint64_t states) {
    return states_field(states, H2_LOADER_STATES_BOOT_INTENT_MASK,
                        H2_LOADER_STATES_BOOT_INTENT_SHIFT);
}

uint32_t h2_loader_states_install_state(uint64_t states) {
    return states_field(states, H2_LOADER_STATES_INSTALL_STATE_MASK,
                        H2_LOADER_STATES_INSTALL_STATE_SHIFT);
}

uint32_t h2_loader_states_upgrade_phase(uint64_t states) {
    return states_field(states, H2_LOADER_STATES_UPGRADE_PHASE_MASK,
                        H2_LOADER_STATES_UPGRADE_PHASE_SHIFT);
}

int h2_loader_states_flags_known(uint64_t states) {
    return (states & H2_LOADER_STATES_FLAGS_KNOWN) != 0u;
}

int h2_loader_states_app_confirmed(uint64_t states) {
    return (states & H2_LOADER_STATES_APP_CONFIRMED) != 0u;
}

int h2_loader_states_manual_hold(uint64_t states) {
    return (states & H2_LOADER_STATES_MANUAL_HOLD) != 0u;
}

int h2_loader_states_installed_valid(uint64_t states) {
    return (states & H2_LOADER_STATES_INSTALLED_VALID) != 0u;
}

int h2_loader_states_staged_valid(uint64_t states) {
    return (states & H2_LOADER_STATES_STAGED_VALID) != 0u;
}

uint32_t h2_loader_states_mfg_mode(uint64_t states) {
    return states_field(states, H2_LOADER_STATES_MFG_MODE_MASK,
                        H2_LOADER_STATES_MFG_MODE_SHIFT);
}

uint32_t h2_loader_states_mfg_step(uint64_t states, uint32_t index) {
    return index < H2_LOADER_MFG_STEP_TOTAL
        ? states_field(states, H2_LOADER_STATES_MFG_STEP_MASK(index),
                       H2_LOADER_STATES_MFG_STEP_SHIFT(index))
        : UINT32_MAX;
}

int h2_loader_states_validate(uint64_t states) {
    const uint32_t role = h2_loader_states_active_role(states);
    const uint32_t intent = h2_loader_states_boot_intent(states);
    const uint32_t install = h2_loader_states_install_state(states);
    const uint32_t upgrade = h2_loader_states_upgrade_phase(states);
    const uint32_t mfg_mode = h2_loader_states_mfg_mode(states);
    const uint64_t lifecycle = H2_LOADER_STATES_APP_CONFIRMED |
        H2_LOADER_STATES_MANUAL_HOLD | H2_LOADER_STATES_INSTALLED_VALID |
        H2_LOADER_STATES_STAGED_VALID;
    if ((states & H2_LOADER_STATES_RESERVED_MASK) != 0u ||
        role == H2_LOADER_ACTIVE_ROLE_UNKNOWN || role == 3u ||
        intent == 3u || install > 9u || upgrade > 6u ||
        mfg_mode == H2_LOADER_MFG_MODE_UNKNOWN || mfg_mode == 3u ||
        (!h2_loader_states_flags_known(states) && (states & lifecycle) != 0u)) {
        return H2_PAL_ERR_FORMAT;
    }
    if (mfg_mode != H2_LOADER_MFG_MODE_ENABLED) {
        for (uint32_t i = 0u; i < H2_LOADER_MFG_STEP_TOTAL; ++i) {
            if (h2_loader_states_mfg_step(states, i) != 0u) {
                return H2_PAL_ERR_FORMAT;
            }
        }
    }
    return H2_PAL_OK;
}

int h2_loader_states_pack(
    const h2_loader_status_t *status,
    uint64_t *out_states) {
    uint64_t states;
    if (status == NULL || out_states == NULL ||
        status->active_role > H2_LOADER_ACTIVE_ROLE_APP ||
        status->boot_intent > H2_LOADER_BOOT_INTENT_AUTO ||
        status->install_state > H2_LOADER_INSTALL_STATE_MAIN_FAILED ||
        status->loader_upgrade.phase > H2_LOADER_UPGRADE_PHASE_CORRUPT ||
        h2_loader_mfg_summary_validate(&status->mfg) != H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    states = ((uint64_t)status->active_role << H2_LOADER_STATES_ACTIVE_ROLE_SHIFT) |
        ((uint64_t)status->boot_intent << H2_LOADER_STATES_BOOT_INTENT_SHIFT) |
        ((uint64_t)(status->install_state + 1u) << H2_LOADER_STATES_INSTALL_STATE_SHIFT) |
        ((uint64_t)(status->upgrade_phase_known
            ? status->loader_upgrade.phase + 1u : 0u)
            << H2_LOADER_STATES_UPGRADE_PHASE_SHIFT) |
        H2_LOADER_STATES_FLAGS_KNOWN;
    if (status->app_confirmed) states |= H2_LOADER_STATES_APP_CONFIRMED;
    if (status->manual_hold) states |= H2_LOADER_STATES_MANUAL_HOLD;
    if (status->installed.valid) states |= H2_LOADER_STATES_INSTALLED_VALID;
    if (status->staged.valid) states |= H2_LOADER_STATES_STAGED_VALID;
    states |= (uint64_t)(status->mfg.total == 0u
        ? H2_LOADER_MFG_MODE_DISABLED : H2_LOADER_MFG_MODE_ENABLED)
        << H2_LOADER_STATES_MFG_MODE_SHIFT;
    for (uint32_t i = 0u; i < H2_LOADER_MFG_STEP_TOTAL; ++i) {
        states |= (uint64_t)status->mfg.step_status[i]
            << H2_LOADER_STATES_MFG_STEP_SHIFT(i);
    }
    *out_states = states;
    return h2_loader_states_validate(states);
}

typedef struct h2_loader_stage_pref_snapshot {
    int has_version;
    int has_checksum;
    int has_size;
    char version[H2_LOADER_IDENTITY_TEXT_MAX];
    char checksum[H2_LOADER_IDENTITY_TEXT_MAX];
    uint32_t size;
} h2_loader_stage_pref_snapshot_t;

static const char *default_if_empty(const char *value, const char *fallback) {
    return value != NULL && value[0] != '\0' ? value : fallback;
}

static void copy_text(char *dst, size_t dst_len, const char *src) {
    size_t len;

    if (dst == NULL || dst_len == 0u) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    len = strlen(src);
    if (len >= dst_len) {
        len = dst_len - 1u;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int is_sha256_hex(const char *text) {
    size_t i;

    if (text == NULL || strlen(text) != 64u) {
        return 0;
    }
    for (i = 0u; i < 64u; ++i) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return 0;
        }
    }
    return 1;
}

const char *h2_loader_boot_intent_name(h2_loader_boot_intent_t intent) {
    switch (intent) {
    case H2_LOADER_BOOT_INTENT_LOADER:
        return "loader";
    case H2_LOADER_BOOT_INTENT_AUTO:
        return "auto";
    default:
        return "unknown";
    }
}

const char *h2_loader_install_state_name(h2_loader_install_state_t state) {
    switch (state) {
    case H2_LOADER_INSTALL_STATE_IDLE:
        return "idle";
    case H2_LOADER_INSTALL_STATE_STAGED:
        return "staged";
    case H2_LOADER_INSTALL_STATE_INSTALL_REQUESTED:
        return "install-requested";
    case H2_LOADER_INSTALL_STATE_INSTALLING:
        return "installing";
    case H2_LOADER_INSTALL_STATE_INSTALLED_PENDING_CONFIRM:
        return "installed-pending-confirm";
    case H2_LOADER_INSTALL_STATE_CONFIRMED:
        return "confirmed";
    case H2_LOADER_INSTALL_STATE_INSTALL_FAILED:
        return "install-failed";
    case H2_LOADER_INSTALL_STATE_RETURN_REQUESTED:
        return "return-requested";
    case H2_LOADER_INSTALL_STATE_MAIN_FAILED:
        return "main-failed";
    default:
        return "unknown";
    }
}

int h2_loader_install_requires_app_confirmation(
    int app_written,
    int app_confirmed) {
    return app_written || !app_confirmed;
}

const char *h2_loader_upgrade_phase_name(h2_loader_upgrade_phase_t phase) {
    switch (phase) {
    case H2_LOADER_UPGRADE_PHASE_IDLE:
        return "idle";
    case H2_LOADER_UPGRADE_PHASE_TRIAL_PENDING:
        return "trial_pending";
    case H2_LOADER_UPGRADE_PHASE_TRIAL_RUNNING:
        return "trial_running";
    case H2_LOADER_UPGRADE_PHASE_CANONICAL_PENDING:
        return "canonical_pending";
    case H2_LOADER_UPGRADE_PHASE_FAILED:
        return "failed";
    case H2_LOADER_UPGRADE_PHASE_CORRUPT:
    default:
        return "corrupt";
    }
}

static int hex_nibble(char value, uint8_t *out) {
    if (value >= '0' && value <= '9') {
        *out = (uint8_t)(value - '0');
        return H2_PAL_OK;
    }
    if (value >= 'a' && value <= 'f') {
        *out = (uint8_t)(value - 'a' + 10);
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_FORMAT;
}

static int hex_decode_32(const char *text, uint8_t out[32]) {
    if (text == NULL || out == NULL || strlen(text) != 64u) {
        return H2_PAL_ERR_FORMAT;
    }
    for (size_t i = 0u; i < 32u; ++i) {
        uint8_t high;
        uint8_t low;
        if (hex_nibble(text[i * 2u], &high) != H2_PAL_OK ||
            hex_nibble(text[i * 2u + 1u], &low) != H2_PAL_OK) {
            return H2_PAL_ERR_FORMAT;
        }
        out[i] = (uint8_t)((high << 4u) | low);
    }
    return H2_PAL_OK;
}

static void hex_encode_32(const uint8_t data[32], char out[H2_LOADER_SHA256_HEX_SIZE]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < 32u; ++i) {
        out[i * 2u] = hex[data[i] >> 4u];
        out[i * 2u + 1u] = hex[data[i] & 0x0fu];
    }
    out[64] = '\0';
}

static void put_u32_le(uint8_t **cursor, uint32_t value) {
    for (size_t i = 0u; i < 4u; ++i) {
        (*cursor)[i] = (uint8_t)(value >> (i * 8u));
    }
    *cursor += 4u;
}

static void put_u64_le(uint8_t **cursor, uint64_t value) {
    for (size_t i = 0u; i < 8u; ++i) {
        (*cursor)[i] = (uint8_t)(value >> (i * 8u));
    }
    *cursor += 8u;
}

static uint32_t get_u32_le(const uint8_t **cursor) {
    uint32_t value = 0u;
    for (size_t i = 0u; i < 4u; ++i) {
        value |= (uint32_t)(*cursor)[i] << (i * 8u);
    }
    *cursor += 4u;
    return value;
}

static uint64_t get_u64_le(const uint8_t **cursor) {
    uint64_t value = 0u;
    for (size_t i = 0u; i < 8u; ++i) {
        value |= (uint64_t)(*cursor)[i] << (i * 8u);
    }
    *cursor += 8u;
    return value;
}

static int record_text_size(const char *text, size_t *out_len) {
    size_t len;
    if (text == NULL || out_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    len = strlen(text);
    if (len == 0u || len >= H2_LOADER_IDENTITY_TEXT_MAX || len > UINT8_MAX) {
        return H2_PAL_ERR_FORMAT;
    }
    *out_len = len;
    return H2_PAL_OK;
}

static int record_text_valid(const char *text, int token) {
    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    for (size_t i = 0u; i < H2_LOADER_IDENTITY_TEXT_MAX; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\0') {
            return 1;
        }
        if (token) {
            if (!((ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') || ch == '.' ||
                    ch == '_' || ch == '-')) {
                return 0;
            }
        } else if (ch < 0x20u || ch > 0x7eu || ch == ' ') {
            return 0;
        }
    }
    return 0;
}

int h2_loader_upgrade_record_encode(
    const h2_loader_upgrade_record_t *record,
    void *data,
    size_t capacity,
    size_t *out_len) {
    uint8_t package_digest[32];
    uint8_t image_digest[32];
    size_t board_len;
    size_t target_len;
    size_t version_len;
    size_t needed;
    uint8_t *cursor = (uint8_t *)data;

    if (record == NULL || data == NULL || out_len == NULL || record->format != 1u ||
        record->phase > H2_LOADER_UPGRADE_PHASE_FAILED ||
        record->candidate.role != H2_LOADER_IMAGE_ROLE_H2LOADER ||
        record->candidate.image_size == 0u || record->canonical_partition == 0u ||
        record->trial_partition == 0u || record->canonical_partition == record->trial_partition ||
        !record_text_valid(record->candidate.board, 1) ||
        !record_text_valid(record->candidate.target, 1) ||
        !record_text_valid(record->candidate.version, 0) ||
        record_text_size(record->candidate.board, &board_len) != H2_PAL_OK ||
        record_text_size(record->candidate.target, &target_len) != H2_PAL_OK ||
        record_text_size(record->candidate.version, &version_len) != H2_PAL_OK ||
        hex_decode_32(record->package_sha256, package_digest) != H2_PAL_OK ||
        hex_decode_32(record->candidate.image_sha256, image_digest) != H2_PAL_OK) {
        return H2_PAL_ERR_FORMAT;
    }
    needed = 4u + 1u + 32u + 1u + 1u + board_len + 1u + target_len +
        1u + version_len + 8u + 32u + 4u + 4u + 4u;
    if (capacity < needed) {
        return H2_PAL_ERR_NO_SPACE;
    }
    put_u32_le(&cursor, record->format);
    *cursor++ = (uint8_t)record->phase;
    memcpy(cursor, package_digest, sizeof(package_digest));
    cursor += sizeof(package_digest);
    *cursor++ = (uint8_t)record->candidate.role;
#define PUT_TEXT(value, value_len) \
    do { \
        *cursor++ = (uint8_t)(value_len); \
        memcpy(cursor, (value), (value_len)); \
        cursor += (value_len); \
    } while (0)
    PUT_TEXT(record->candidate.board, board_len);
    PUT_TEXT(record->candidate.target, target_len);
    PUT_TEXT(record->candidate.version, version_len);
#undef PUT_TEXT
    put_u64_le(&cursor, record->candidate.image_size);
    memcpy(cursor, image_digest, sizeof(image_digest));
    cursor += sizeof(image_digest);
    put_u32_le(&cursor, record->canonical_partition);
    put_u32_le(&cursor, record->trial_partition);
    put_u32_le(&cursor, (uint32_t)record->last_result);
    *out_len = (size_t)(cursor - (uint8_t *)data);
    return H2_PAL_OK;
}

static int decode_text(
    const uint8_t **cursor,
    const uint8_t *end,
    char out[H2_LOADER_IDENTITY_TEXT_MAX]) {
    size_t len;
    if (*cursor >= end) {
        return H2_PAL_ERR_FORMAT;
    }
    len = *(*cursor)++;
    if (len == 0u || len >= H2_LOADER_IDENTITY_TEXT_MAX || (size_t)(end - *cursor) < len) {
        return H2_PAL_ERR_FORMAT;
    }
    memcpy(out, *cursor, len);
    out[len] = '\0';
    *cursor += len;
    return H2_PAL_OK;
}

int h2_loader_upgrade_record_decode(
    const void *data,
    size_t len,
    h2_loader_upgrade_record_t *out_record) {
    const uint8_t *cursor = (const uint8_t *)data;
    const uint8_t *end;
    if (data == NULL || out_record == NULL || len < 92u || len > H2_LOADER_UPGRADE_RECORD_MAX) {
        return H2_PAL_ERR_FORMAT;
    }
    end = cursor + len;
    memset(out_record, 0, sizeof(*out_record));
    out_record->format = get_u32_le(&cursor);
    out_record->phase = (h2_loader_upgrade_phase_t)*cursor++;
    if (out_record->format != 1u || out_record->phase > H2_LOADER_UPGRADE_PHASE_FAILED ||
        (size_t)(end - cursor) < 34u) {
        return H2_PAL_ERR_FORMAT;
    }
    hex_encode_32(cursor, out_record->package_sha256);
    cursor += 32u;
    out_record->candidate.format = 1u;
    out_record->candidate.role = (h2_loader_image_role_t)*cursor++;
    if (out_record->candidate.role != H2_LOADER_IMAGE_ROLE_H2LOADER ||
        decode_text(&cursor, end, out_record->candidate.board) != H2_PAL_OK ||
        decode_text(&cursor, end, out_record->candidate.target) != H2_PAL_OK ||
        decode_text(&cursor, end, out_record->candidate.version) != H2_PAL_OK ||
        (size_t)(end - cursor) < 52u) {
        return H2_PAL_ERR_FORMAT;
    }
    out_record->candidate.image_size = get_u64_le(&cursor);
    hex_encode_32(cursor, out_record->candidate.image_sha256);
    cursor += 32u;
    out_record->canonical_partition = get_u32_le(&cursor);
    out_record->trial_partition = get_u32_le(&cursor);
    out_record->last_result = (int32_t)get_u32_le(&cursor);
    if (cursor != end || out_record->candidate.image_size == 0u ||
        out_record->canonical_partition == 0u || out_record->trial_partition == 0u ||
        out_record->canonical_partition == out_record->trial_partition ||
        !record_text_valid(out_record->candidate.board, 1) ||
        !record_text_valid(out_record->candidate.target, 1) ||
        !record_text_valid(out_record->candidate.version, 0)) {
        return H2_PAL_ERR_FORMAT;
    }
    return H2_PAL_OK;
}

static int pref_open(
    const h2_pal_pref_api_t *pref,
    h2_pal_pref_open_mode_t mode,
    h2_pal_pref_namespace_t **out_ns) {
    if (out_ns == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_ns = NULL;
    if (pref == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return h2_pal_pref_open(pref, H2_LOADER_PREF_NAMESPACE, mode, out_ns);
}

static int mfg_record_decode(
    const void *data,
    size_t len,
    h2_loader_mfg_summary_t *out_summary) {
    const uint8_t *cursor = (const uint8_t *)data;
    uint32_t format;
    uint32_t state;
    uint32_t passed;
    uint32_t total;
    uint32_t passed_mask = 0u;
    uint32_t skipped_mask = 0u;

    if (data == NULL || out_summary == NULL ||
        (len != H2_LOADER_MFG_RECORD_V1_SIZE &&
         len != H2_LOADER_MFG_RECORD_V2_SIZE &&
         len != H2_LOADER_MFG_RECORD_SIZE)) {
        return H2_PAL_ERR_FORMAT;
    }
    memset(out_summary, 0, sizeof(*out_summary));
    format = get_u32_le(&cursor);
    if (format == H2_LOADER_MFG_RECORD_FORMAT &&
        len == H2_LOADER_MFG_RECORD_SIZE) {
        out_summary->total = H2_LOADER_MFG_STEP_TOTAL;
        memcpy(out_summary->step_status, cursor,
               sizeof(out_summary->step_status));
    } else if ((format == H2_LOADER_MFG_RECORD_V1_FORMAT &&
                len == H2_LOADER_MFG_RECORD_V1_SIZE) ||
               (format == H2_LOADER_MFG_RECORD_V2_FORMAT &&
                len == H2_LOADER_MFG_RECORD_V2_SIZE)) {
        state = get_u32_le(&cursor);
        passed = get_u32_le(&cursor);
        total = get_u32_le(&cursor);
        if (total != H2_LOADER_MFG_STEP_TOTAL || passed > total ||
            state < 1u || state > 3u) {
            return H2_PAL_ERR_FORMAT;
        }
        if (format == H2_LOADER_MFG_RECORD_V2_FORMAT) {
            const uint32_t valid_mask =
                (UINT32_C(1) << H2_LOADER_MFG_STEP_TOTAL) - UINT32_C(1);
            passed_mask = get_u32_le(&cursor);
            skipped_mask = get_u32_le(&cursor);
            if ((passed_mask & skipped_mask) != 0u ||
                ((passed_mask | skipped_mask) & ~valid_mask) != 0u) {
                return H2_PAL_ERR_FORMAT;
            }
        } else if (passed > 0u) {
            passed_mask = (UINT32_C(1) << passed) - UINT32_C(1);
        }
        if (state == 2u &&
            (passed != H2_LOADER_MFG_STEP_TOTAL ||
             passed_mask !=
                 (UINT32_C(1) << H2_LOADER_MFG_STEP_TOTAL) - UINT32_C(1) ||
             skipped_mask != 0u)) {
            return H2_PAL_ERR_FORMAT;
        }
        out_summary->total = H2_LOADER_MFG_STEP_TOTAL;
        for (uint32_t i = 0u; i < H2_LOADER_MFG_STEP_TOTAL; ++i) {
            if ((passed_mask & (UINT32_C(1) << i)) != 0u) {
                out_summary->step_status[i] = H2_LOADER_MFG_STEP_PASSED;
            } else if ((skipped_mask & (UINT32_C(1) << i)) != 0u) {
                out_summary->step_status[i] = H2_LOADER_MFG_STEP_SKIPPED;
            }
        }
        if (state == 3u && passed < H2_LOADER_MFG_STEP_TOTAL &&
            out_summary->step_status[passed] == H2_LOADER_MFG_STEP_UNTESTED) {
            out_summary->step_status[passed] = H2_LOADER_MFG_STEP_FAILED;
        }
    } else {
        return H2_PAL_ERR_FORMAT;
    }
    if (h2_loader_mfg_summary_validate(out_summary) != H2_PAL_OK) {
        memset(out_summary, 0, sizeof(*out_summary));
        return H2_PAL_ERR_FORMAT;
    }
    return H2_PAL_OK;
}

int h2_loader_mfg_read(
    const h2_pal_pref_api_t *pref,
    const h2_pal_mem_api_t *allocator,
    h2_loader_mfg_summary_t *out_summary,
    int *out_present) {
    h2_pal_pref_namespace_t *ns = NULL;
    void *data = NULL;
    size_t len = 0u;
    int close_rc;
    int rc;

    if (allocator == NULL || out_summary == NULL || out_present == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_summary, 0, sizeof(*out_summary));
    *out_present = 0;
    rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_ONLY, &ns);
    if (rc == H2_PAL_ERR_NOT_FOUND) {
        return H2_PAL_OK;
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ns == NULL || ns->get_blob == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->get_blob(ns, allocator, H2_LOADER_MFG_KEY, &data, &len);
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    if (rc == H2_PAL_ERR_NOT_FOUND) {
        return close_rc;
    }
    if (rc != H2_PAL_OK || close_rc != H2_PAL_OK) {
        h2_pal_mem_free(allocator, data);
        return rc != H2_PAL_OK ? rc : close_rc;
    }
    *out_present = 1;
    rc = mfg_record_decode(data, len, out_summary);
    const int needs_migration =
        rc == H2_PAL_OK && len != H2_LOADER_MFG_RECORD_SIZE;
    h2_pal_mem_free(allocator, data);
    if (rc == H2_PAL_ERR_FORMAT) {
        memset(out_summary, 0, sizeof(*out_summary));
        out_summary->total = H2_LOADER_MFG_STEP_TOTAL;
        rc = h2_loader_mfg_write(pref, out_summary);
    } else if (needs_migration) {
        rc = h2_loader_mfg_write(pref, out_summary);
    }
    return rc;
}

int h2_loader_mfg_write(
    const h2_pal_pref_api_t *pref,
    const h2_loader_mfg_summary_t *summary) {
    uint8_t data[H2_LOADER_MFG_RECORD_SIZE];
    uint8_t *cursor = data;
    h2_pal_pref_namespace_t *ns = NULL;
    int close_rc;
    int rc;

    rc = h2_loader_mfg_summary_validate(summary);
    if (rc != H2_PAL_OK || summary->total != H2_LOADER_MFG_STEP_TOTAL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    put_u32_le(&cursor, H2_LOADER_MFG_RECORD_FORMAT);
    memcpy(cursor, summary->step_status, sizeof(summary->step_status));
    rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ns == NULL || ns->set_blob == NULL || ns->commit == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->set_blob(ns, H2_LOADER_MFG_KEY, data, sizeof(data));
        if (rc == H2_PAL_OK) {
            rc = ns->commit(ns);
        }
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

int h2_loader_mfg_reset(
    const h2_pal_pref_api_t *pref,
    uint32_t total) {
    const h2_loader_mfg_summary_t summary = {.total = total};
    return h2_loader_mfg_write(pref, &summary);
}

static int upgrade_record_write(
    const h2_pal_pref_api_t *pref,
    const h2_loader_upgrade_record_t *record) {
    uint8_t data[H2_LOADER_UPGRADE_RECORD_MAX];
    h2_pal_pref_namespace_t *ns = NULL;
    size_t len = 0u;
    int rc;
    int close_rc;

    rc = h2_loader_upgrade_record_encode(record, data, sizeof(data), &len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ns == NULL || ns->set_blob == NULL || ns->commit == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->set_blob(ns, H2_LOADER_UPGRADE_KEY, data, len);
        if (rc == H2_PAL_OK) {
            rc = ns->commit(ns);
        }
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

static int upgrade_record_read(
    const h2_pal_pref_api_t *pref,
    const h2_pal_mem_api_t *allocator,
    h2_loader_upgrade_record_t *out_record,
    int *out_present) {
    h2_pal_pref_namespace_t *ns = NULL;
    void *data = NULL;
    size_t len = 0u;
    int rc;
    int close_rc;

    if (allocator == NULL || out_record == NULL || out_present == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_record, 0, sizeof(*out_record));
    out_record->phase = H2_LOADER_UPGRADE_PHASE_IDLE;
    *out_present = 0;
    rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_ONLY, &ns);
    if (rc == H2_PAL_ERR_NOT_FOUND) {
        return H2_PAL_OK;
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ns == NULL || ns->get_blob == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->get_blob(ns, allocator, H2_LOADER_UPGRADE_KEY, &data, &len);
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    if (rc == H2_PAL_ERR_NOT_FOUND) {
        return H2_PAL_OK;
    }
    if (rc != H2_PAL_OK) {
        h2_pal_mem_free(allocator, data);
        return rc;
    }
    if (close_rc != H2_PAL_OK) {
        h2_pal_mem_free(allocator, data);
        return close_rc;
    }
    *out_present = 1;
    rc = h2_loader_upgrade_record_decode(data, len, out_record);
    h2_pal_mem_free(allocator, data);
    return rc;
}

static int upgrade_record_remove(
    const h2_pal_pref_api_t *pref,
    h2_loader_upgrade_record_t *record) {
    h2_pal_pref_namespace_t *ns = NULL;
    int rc;
    int close_rc;

    record->phase = H2_LOADER_UPGRADE_PHASE_IDLE;
    record->last_result = H2_PAL_OK;
    rc = upgrade_record_write(pref, record);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ns == NULL || ns->remove == NULL || ns->commit == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->remove(ns, H2_LOADER_UPGRADE_KEY);
        if (rc == H2_PAL_OK || rc == H2_PAL_ERR_NOT_FOUND) {
            rc = ns->remove(ns, H2_LOADER_UPGRADE_STEP_KEY);
            if (rc == H2_PAL_OK || rc == H2_PAL_ERR_NOT_FOUND) {
                rc = ns->commit(ns);
            }
        }
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

static int upgrade_record_complete(
    const h2_pal_pref_api_t *pref,
    h2_loader_upgrade_record_t *record) {
    uint8_t data[H2_LOADER_UPGRADE_RECORD_MAX];
    h2_pal_pref_namespace_t *ns = NULL;
    size_t len = 0u;
    int rc;
    int close_rc;

    record->phase = H2_LOADER_UPGRADE_PHASE_IDLE;
    record->last_result = H2_PAL_OK;
    rc = h2_loader_upgrade_record_encode(record, data, sizeof(data), &len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ns == NULL || ns->set_blob == NULL || ns->remove == NULL ||
        ns->commit == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->set_blob(ns, H2_LOADER_UPGRADE_KEY, data, len);
        if (rc == H2_PAL_OK) {
            rc = ns->remove(ns, H2_LOADER_UPGRADE_STEP_KEY);
            if (rc == H2_PAL_ERR_NOT_FOUND) {
                rc = H2_PAL_OK;
            }
        }
        if (rc == H2_PAL_OK) {
            rc = ns->commit(ns);
        }
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

static int pref_set_u32(const h2_pal_pref_api_t *pref, const char *key, uint32_t value) {
    h2_pal_pref_namespace_t *ns = NULL;
    int rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    int close_rc;

    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ns == NULL || ns->set_u32 == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->set_u32(ns, key, value);
        if (rc == H2_PAL_OK && ns->commit != NULL) {
            rc = ns->commit(ns);
        }
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

static int pref_get_u32(
    const h2_pal_pref_api_t *pref,
    const char *key,
    uint32_t *out_value) {
    h2_pal_pref_namespace_t *ns = NULL;
    int rc;
    int close_rc;

    if (key == NULL || out_value == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_ONLY, &ns);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ns == NULL || ns->get_u32 == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->get_u32(ns, key, out_value);
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

static int mfg_acceptance_revision_write(
    const h2_pal_pref_api_t *pref,
    uint32_t revision) {
    h2_pal_pref_namespace_t *ns = NULL;
    int rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    int close_rc;

    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ns == NULL || ns->set_u32 == NULL || ns->commit == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->set_u32(ns, H2_LOADER_MFG_ACCEPTANCE_REVISION_KEY, revision);
        if (rc == H2_PAL_OK) {
            rc = ns->commit(ns);
        }
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

int h2_loader_mfg_ensure_acceptance_revision(
    const h2_pal_pref_api_t *pref,
    uint32_t total,
    uint32_t required_revision) {
    uint32_t stored_revision = 0u;
    int rc;

    if (pref == NULL || total == 0u || required_revision == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = pref_get_u32(pref, H2_LOADER_MFG_ACCEPTANCE_REVISION_KEY,
                      &stored_revision);
    if (rc == H2_PAL_OK && stored_revision == required_revision) {
        return H2_PAL_OK;
    }
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND) {
        return rc;
    }
    rc = h2_loader_mfg_reset(pref, total);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return mfg_acceptance_revision_write(pref, required_revision);
}

static int pref_set_i32(const h2_pal_pref_api_t *pref, const char *key, int32_t value) {
    h2_pal_pref_namespace_t *ns = NULL;
    int rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    int close_rc;

    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ns == NULL || ns->set_i32 == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->set_i32(ns, key, value);
        if (rc == H2_PAL_OK && ns->commit != NULL) {
            rc = ns->commit(ns);
        }
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

static int pref_set_string(const h2_pal_pref_api_t *pref, const char *key, const char *value) {
    h2_pal_pref_namespace_t *ns = NULL;
    int rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    int close_rc;

    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ns == NULL || ns->set_string == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->set_string(ns, key, value != NULL ? value : "");
        if (rc == H2_PAL_OK && ns->commit != NULL) {
            rc = ns->commit(ns);
        }
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

static int pref_set_bool(const h2_pal_pref_api_t *pref, const char *key, int value) {
    h2_pal_pref_namespace_t *ns = NULL;
    int rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    int close_rc;

    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ns == NULL || ns->set_bool == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->set_bool(ns, key, value ? 1 : 0);
        if (rc == H2_PAL_OK && ns->commit != NULL) {
            rc = ns->commit(ns);
        }
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

static int pref_remove(const h2_pal_pref_api_t *pref, const char *key) {
    h2_pal_pref_namespace_t *ns = NULL;
    int rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    int close_rc;

    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ns == NULL || ns->remove == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->remove(ns, key);
        if ((rc == H2_PAL_OK || rc == H2_PAL_ERR_NOT_FOUND) && ns->commit != NULL) {
            rc = ns->commit(ns);
        }
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

int h2_loader_read_pref_status(
    const h2_pal_pref_api_t *pref,
    const h2_pal_mem_api_t *allocator,
    h2_loader_status_t *out_status) {
    h2_pal_pref_namespace_t *ns = NULL;
    uint32_t value = 0u;
    int32_t last = 0;
    int rc;

    if (pref == NULL || out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->boot_intent = H2_LOADER_BOOT_INTENT_LOADER;
    out_status->install_state = H2_LOADER_INSTALL_STATE_IDLE;

    rc = pref_open(pref, H2_PAL_PREF_OPEN_READ_ONLY, &ns);
    if (rc != H2_PAL_OK) {
        return rc == H2_PAL_ERR_NOT_FOUND ? H2_PAL_OK : rc;
    }
    if (ns == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (ns->get_u32 != NULL && ns->get_u32(ns, "boot_intent", &value) == H2_PAL_OK) {
        out_status->boot_intent = (h2_loader_boot_intent_t)value;
    }
    if (ns->get_u32 != NULL && ns->get_u32(ns, "install_state", &value) == H2_PAL_OK) {
        out_status->install_state = (h2_loader_install_state_t)value;
    }
    if (ns->get_bool != NULL) {
        (void)ns->get_bool(ns, "app_confirmed", &out_status->app_confirmed);
        (void)ns->get_bool(ns, "manual_hold", &out_status->manual_hold);
    }
    if (ns->get_i32 != NULL && ns->get_i32(ns, "last_result", &last) == H2_PAL_OK) {
        out_status->last_result = last;
    }
    if (ns->get_string != NULL && allocator != NULL) {
        char *text = NULL;
        if (ns->get_string(ns, allocator, "installed_version", &text) == H2_PAL_OK && text != NULL) {
            copy_text(out_status->installed.version, sizeof(out_status->installed.version), text);
            h2_pal_mem_free(allocator, text);
            out_status->installed.valid = 1;
        }
        text = NULL;
        if (ns->get_string(ns, allocator, "installed_checksum", &text) == H2_PAL_OK && text != NULL) {
            copy_text(out_status->installed.checksum, sizeof(out_status->installed.checksum), text);
            h2_pal_mem_free(allocator, text);
            out_status->installed.valid = 1;
        }
        text = NULL;
        if (ns->get_string(ns, allocator, H2_LOADER_UPGRADE_STEP_KEY, &text) == H2_PAL_OK && text != NULL) {
            copy_text(out_status->loader_upgrade_step,
                sizeof(out_status->loader_upgrade_step), text);
            h2_pal_mem_free(allocator, text);
        }
    }
    if (ns->get_blob != NULL && allocator != NULL) {
        void *data = NULL;
        size_t len = 0u;
        if (ns->get_blob(ns, allocator, H2_LOADER_MFG_KEY, &data, &len) == H2_PAL_OK) {
            if (mfg_record_decode(data, len, &out_status->mfg) != H2_PAL_OK) {
                memset(&out_status->mfg, 0, sizeof(out_status->mfg));
            }
            h2_pal_mem_free(allocator, data);
        }
    }
    if (ns->close != NULL) {
        (void)ns->close(ns);
    }
    if (allocator != NULL) {
        h2_loader_metadata_t *records[] = {
            &out_status->stage,
            &out_status->partition_1,
            &out_status->partition_2,
        };
        const h2_loader_metadata_slot_t slots[] = {
            H2_LOADER_METADATA_SLOT_STAGE,
            H2_LOADER_METADATA_SLOT_PARTITION_1,
            H2_LOADER_METADATA_SLOT_PARTITION_2,
        };
        size_t i;
        for (i = 0u; i < sizeof(slots) / sizeof(slots[0]); ++i) {
            int present = 0;
            rc = h2_loader_metadata_read(
                pref, allocator, slots[i], records[i], &present);
            if (rc == H2_PAL_ERR_NOT_FOUND || rc == H2_PAL_ERR_UNSUPPORTED) {
                rc = H2_PAL_OK;
            }
            if (rc != H2_PAL_OK) {
                memset(records[i], 0, sizeof(*records[i]));
                if (out_status->last_result == H2_PAL_OK) {
                    out_status->last_result = rc;
                }
            }
        }
    }
    return H2_PAL_OK;
}

static int pref_set_identity(
    const h2_pal_pref_api_t *pref,
    const char *prefix,
    const h2_loader_identity_t *identity) {
    char key[48];
    int rc;

    if (pref == NULL || prefix == NULL || identity == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (void)snprintf(key, sizeof(key), "%s_version", prefix);
    rc = pref_set_string(pref, key, identity->version);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    (void)snprintf(key, sizeof(key), "%s_checksum", prefix);
    rc = pref_set_string(pref, key, identity->checksum);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    (void)snprintf(key, sizeof(key), "%s_size", prefix);
    return pref_set_u32(pref, key, (uint32_t)identity->size);
}

static int pref_set_state(
    const h2_pal_pref_api_t *pref,
    h2_loader_install_state_t state,
    h2_loader_boot_intent_t intent) {
    int rc = pref_set_u32(pref, "install_state", (uint32_t)state);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return pref_set_u32(pref, "boot_intent", (uint32_t)intent);
}

static int remove_staged_package(h2_loader_t *loader) {
    int rc;

    if (loader == NULL || loader->config.package.fs == NULL ||
        loader->config.package.package_path == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    rc = h2_pal_fs_remove(loader->config.package.fs, loader->config.package.package_path);
    return rc == H2_PAL_FS_ERR_NOT_FOUND ? H2_PAL_OK : rc;
}

static int remove_stage_pref(const h2_pal_pref_api_t *pref, const char *key) {
    int rc = pref_remove(pref, key);
    return rc == H2_PAL_ERR_NOT_FOUND ? H2_PAL_OK : rc;
}

static int read_optional_pref_string(
    h2_pal_pref_namespace_t *ns,
    const h2_pal_mem_api_t *allocator,
    const char *key,
    char *out,
    size_t out_len,
    int *out_found) {
    char *value = NULL;
    int rc;

    if (ns == NULL || allocator == NULL || key == NULL || out == NULL || out_len == 0u || out_found == NULL ||
        ns->get_string == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    *out_found = 0;
    out[0] = '\0';
    rc = ns->get_string(ns, allocator, key, &value);
    if (rc == H2_PAL_ERR_NOT_FOUND) {
        return H2_PAL_OK;
    }
    if (rc != H2_PAL_OK) {
        if (value != NULL) {
            h2_pal_mem_free(allocator, value);
        }
        return rc;
    }
    if (value == NULL) {
        return H2_PAL_ERR_IO;
    }
    copy_text(out, out_len, value);
    h2_pal_mem_free(allocator, value);
    *out_found = 1;
    return H2_PAL_OK;
}

static int read_staged_pref_snapshot(
    h2_loader_t *loader,
    h2_loader_stage_pref_snapshot_t *snapshot) {
    h2_pal_pref_namespace_t *ns = NULL;
    int rc;
    int close_rc;

    if (loader == NULL || snapshot == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    rc = pref_open(loader->config.pref, H2_PAL_PREF_OPEN_READ_ONLY, &ns);
    if (rc == H2_PAL_ERR_NOT_FOUND) {
        return H2_PAL_OK;
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ns == NULL || ns->get_u32 == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
        goto out;
    }
    rc = read_optional_pref_string(ns,
        loader->config.package.allocator,
        "staged_version",
        snapshot->version,
        sizeof(snapshot->version),
        &snapshot->has_version);
    if (rc != H2_PAL_OK) {
        goto out;
    }
    rc = read_optional_pref_string(ns,
        loader->config.package.allocator,
        "staged_checksum",
        snapshot->checksum,
        sizeof(snapshot->checksum),
        &snapshot->has_checksum);
    if (rc != H2_PAL_OK) {
        goto out;
    }
    rc = ns->get_u32(ns, "staged_size", &snapshot->size);
    if (rc == H2_PAL_ERR_NOT_FOUND) {
        rc = H2_PAL_OK;
    } else if (rc == H2_PAL_OK) {
        snapshot->has_size = 1;
    }

out:
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

static int clear_staged_state(h2_loader_t *loader) {
    int rc;

    if (loader == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = remove_stage_pref(loader->config.pref, "staged_version");
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = remove_stage_pref(loader->config.pref, "staged_checksum");
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return remove_stage_pref(loader->config.pref, "staged_size");
}

static int normalize_legacy_staged_lifecycle(
    h2_loader_t *loader,
    const h2_loader_status_t *status) {
    h2_loader_install_state_t state;
    h2_loader_boot_intent_t intent;

    if (loader == NULL || status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (status->install_state != H2_LOADER_INSTALL_STATE_STAGED) {
        return H2_PAL_OK;
    }
    if (!status->installed.valid) {
        state = H2_LOADER_INSTALL_STATE_IDLE;
        intent = H2_LOADER_BOOT_INTENT_LOADER;
    } else if (status->app_confirmed) {
        state = H2_LOADER_INSTALL_STATE_CONFIRMED;
        intent = H2_LOADER_BOOT_INTENT_AUTO;
    } else {
        state = H2_LOADER_INSTALL_STATE_MAIN_FAILED;
        intent = H2_LOADER_BOOT_INTENT_LOADER;
    }
    return pref_set_state(loader->config.pref, state, intent);
}

static int clear_installed_state(h2_loader_t *loader) {
    int rc;

    if (loader == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = remove_stage_pref(loader->config.pref, "installed_version");
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = remove_stage_pref(loader->config.pref, "installed_checksum");
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = remove_stage_pref(loader->config.pref, "installed_size");
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return pref_set_bool(loader->config.pref, "app_confirmed", 0);
}

static void cleanup_failed_stage_publish(h2_loader_t *loader) {
    if (loader == NULL) {
        return;
    }
    (void)h2_loader_stage_abort(
        loader->config.package.fs,
        loader->config.pref,
        loader->config.package.package_path);
}

static void emit_event(h2_loader_t *loader, h2_loader_startup_event_t event, int code) {
    if (loader != NULL && loader->config.on_event != NULL) {
        loader->config.on_event(loader->config.event_user, event, code);
    }
}

static int clear_failed_app_to_loader(h2_loader_t *loader) {
    int rc;

    if (loader == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (void)remove_staged_package(loader);
    rc = clear_staged_state(loader);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = clear_installed_state(loader);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return pref_set_state(loader->config.pref,
        H2_LOADER_INSTALL_STATE_IDLE,
        H2_LOADER_BOOT_INTENT_LOADER);
}

static int mark_main_failed(h2_loader_t *loader, int result) {
    int rc;

    if (loader == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (void)pref_set_i32(loader->config.pref, "last_result", result);
    rc = pref_set_state(loader->config.pref,
        H2_LOADER_INSTALL_STATE_MAIN_FAILED,
        H2_LOADER_BOOT_INTENT_LOADER);
    emit_event(loader, H2_LOADER_STARTUP_EVENT_MAIN_FAILED, result);
    return rc == H2_PAL_OK ? result : rc;
}

static int mark_install_failed(h2_loader_t *loader, int result) {
    if (loader == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (void)pref_set_i32(loader->config.pref, "last_result", result);
    (void)pref_set_state(loader->config.pref,
        H2_LOADER_INSTALL_STATE_INSTALL_FAILED,
        H2_LOADER_BOOT_INTENT_LOADER);
    emit_event(loader, H2_LOADER_STARTUP_EVENT_INSTALL_FAILED, result);
    return result;
}

static int prepare_app_transition(h2_loader_t *loader) {
    if (loader == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (loader->config.before_disruptive == NULL) {
        return H2_PAL_OK;
    }
    return loader->config.before_disruptive(
        loader->config.disruptive_user,
        H2_LOADER_DISRUPTIVE_BOOT_APP);
}

static int prepare_and_mark_main_failed(h2_loader_t *loader, int result) {
    int rc = prepare_app_transition(loader);
    return rc == H2_PAL_OK ? mark_main_failed(loader, result) : rc;
}

static int boot_app_no_hook(h2_loader_t *loader);

static int recover_unconfirmed_app_to_loader(h2_loader_t *loader, int result) {
    int rc;

    if (loader == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (void)pref_set_i32(loader->config.pref, "last_result", result);
    rc = clear_failed_app_to_loader(loader);
    if (rc != H2_PAL_OK) {
        (void)pref_set_state(loader->config.pref,
            H2_LOADER_INSTALL_STATE_MAIN_FAILED,
            H2_LOADER_BOOT_INTENT_LOADER);
    }
    emit_event(loader, H2_LOADER_STARTUP_EVENT_MAIN_FAILED, result);
    return rc == H2_PAL_OK ? result : rc;
}

static int staged_snapshot_identity(
    const h2_loader_stage_pref_snapshot_t *snapshot,
    h2_loader_identity_t *out_identity) {
    if (snapshot == NULL || out_identity == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_identity, 0, sizeof(*out_identity));
    if (!snapshot->has_checksum || !snapshot->has_size) {
        return H2_PAL_OK;
    }
    copy_text(out_identity->checksum, sizeof(out_identity->checksum), snapshot->checksum);
    copy_text(out_identity->version,
        sizeof(out_identity->version),
        snapshot->has_version ? snapshot->version : snapshot->checksum);
    out_identity->size = snapshot->size;
    out_identity->valid = is_sha256_hex(out_identity->checksum) ? 1 : 0;
    if (!out_identity->valid) {
        memset(out_identity, 0, sizeof(*out_identity));
    }
    return H2_PAL_OK;
}

static int mount_loader_file_points(h2_loader_t *loader) {
    int rc;

    if (loader == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (loader->config.mount_file_point == NULL) {
        return H2_PAL_OK;
    }
    rc = loader->config.mount_file_point(loader->config.mount_user, "/dl");
    if (rc != H2_PAL_OK) {
        (void)pref_set_i32(loader->config.pref, "last_result", rc);
        return rc;
    }
    rc = loader->config.mount_file_point(loader->config.mount_user, "/data");
    if (rc != H2_PAL_OK) {
        (void)pref_set_i32(loader->config.pref, "last_result", rc);
        return rc;
    }
    return H2_PAL_OK;
}

static int select_and_reboot_with_transition(
    h2_loader_t *loader,
    uint32_t partition_id,
    h2_loader_boot_intent_t intent,
    h2_loader_reboot_transition_fn transition,
    void *transition_user) {
    h2_pal_power_boot_partition_t running;
    int rc;

    if (loader == NULL || loader->config.power == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (partition_id != 0u) {
        rc = h2_pal_power_get_running_boot_partition(loader->config.power, &running);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (running.id == 0u || running.id == partition_id) {
            return H2_PAL_ERR_INVALID_STATE;
        }
        rc = h2_pal_power_set_next_boot_partition(loader->config.power, partition_id);
        if (rc != H2_PAL_OK) {
            (void)pref_set_i32(loader->config.pref, "last_result", rc);
            return rc;
        }
    }
    rc = pref_set_u32(loader->config.pref, "boot_intent", (uint32_t)intent);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (transition != NULL) {
        rc = transition(transition_user);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return h2_pal_power_reboot(loader->config.power, H2_LOADER_REBOOT_REASON_DEFAULT);
}

static int select_and_reboot(h2_loader_t *loader, uint32_t partition_id, h2_loader_boot_intent_t intent) {
    return select_and_reboot_with_transition(
        loader, partition_id, intent, NULL, NULL);
}

static int image_identity_equal(
    const h2_loader_image_identity_t *left,
    const h2_loader_image_identity_t *right) {
    return left != NULL && right != NULL &&
        left->role == right->role &&
        strcmp(left->board, right->board) == 0 &&
        strcmp(left->target, right->target) == 0 &&
        strcmp(left->version, right->version) == 0 &&
        (left->image_size == 0u || left->image_size == right->image_size) &&
        (left->image_sha256[0] == '\0' ||
            strcmp(left->image_sha256, right->image_sha256) == 0);
}

static int upgrade_partition_capacity(
    h2_loader_t *loader,
    uint32_t partition_id,
    uint64_t *out_capacity) {
    h2_pal_disk_partition_t partition;
    const h2_loader_image_reader_api_t *reader = loader->config.package.image_reader;
    const h2_loader_image_writer_api_t *writer = loader->config.package.image_writer;
    if (reader != NULL && reader->vtable != NULL && reader->vtable->get_capacity != NULL) {
        return reader->vtable->get_capacity(reader->user, partition_id, out_capacity);
    }
    if (writer != NULL && writer->vtable != NULL && writer->vtable->get_capacity != NULL) {
        return writer->vtable->get_capacity(writer->user, partition_id, out_capacity);
    }
    if (loader->config.package.disk == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    {
        int rc = h2_pal_disk_get_partition(loader->config.package.disk, partition_id, &partition);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    *out_capacity = partition.size;
    return H2_PAL_OK;
}

static int upgrade_select_and_reboot_with_transition(
    h2_loader_t *loader,
    uint32_t partition_id,
    h2_loader_upgrade_transition_fn transition,
    void *transition_user) {
    h2_pal_power_boot_partition_t running;
    int rc = h2_pal_power_get_running_boot_partition(
        loader->config.power, &running);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (partition_id == 0u || running.id == 0u ||
        partition_id == running.id) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    rc = h2_pal_power_set_next_boot_partition(
        loader->config.power, partition_id);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (transition != NULL) {
        rc = transition(transition_user);
        if (rc != H2_PAL_OK) {
            int restore_rc = h2_pal_power_set_next_boot_partition(
                loader->config.power, running.id);
            if (restore_rc != H2_PAL_OK) {
                return restore_rc;
            }
            return rc;
        }
    }
    rc = h2_pal_power_reboot(
        loader->config.power, H2_LOADER_REBOOT_REASON_DEFAULT);
    if (rc != H2_PAL_OK) {
        int restore_rc = h2_pal_power_set_next_boot_partition(
            loader->config.power, running.id);
        if (restore_rc != H2_PAL_OK) {
            return restore_rc;
        }
    }
    return rc;
}

static int upgrade_select_and_reboot(
    h2_loader_t *loader,
    uint32_t partition_id) {
    return upgrade_select_and_reboot_with_transition(
        loader, partition_id, NULL, NULL);
}

static int upgrade_mark_failed(
    h2_loader_t *loader,
    h2_loader_upgrade_record_t *record,
    int result,
    const char *step) {
    record->phase = H2_LOADER_UPGRADE_PHASE_FAILED;
    record->last_result = result;
    if (step != NULL && step[0] != '\0') {
        (void)pref_set_string(
            loader->config.pref, H2_LOADER_UPGRADE_STEP_KEY, step);
    }
    (void)upgrade_record_write(loader->config.pref, record);
    return result;
}

static int upgrade_mark_recovery_failed(
    h2_loader_t *loader,
    h2_loader_upgrade_record_t *record,
    int result,
    const char *step) {
    printf(
        "H2_LOADER_UPGRADE_RECOVERY result=fail code=%d phase=%s step=%s\n",
        result,
        h2_loader_upgrade_phase_name(record->phase),
        step);
    fflush(stdout);
    return upgrade_mark_failed(loader, record, result, step);
}

static int h2_loader_upgrade_recover(
    h2_loader_t *loader,
    h2_loader_startup_action_t *out_action,
    int *out_handled) {
    h2_loader_upgrade_record_t record;
    h2_pal_power_boot_partition_t running;
    int present = 0;
    int rc;

    *out_handled = 0;
    rc = upgrade_record_read(
        loader->config.pref,
        loader->config.package.allocator,
        &record,
        &present);
    if (rc == H2_PAL_ERR_FORMAT) {
        loader->status.loader_upgrade.phase = H2_LOADER_UPGRADE_PHASE_CORRUPT;
        *out_handled = 1;
        return H2_PAL_OK;
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_pal_power_get_running_boot_partition(loader->config.power, &running);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    loader->status.running_partition_id = running.id;
    if (!present || record.phase == H2_LOADER_UPGRADE_PHASE_IDLE) {
        if (running.id == loader->config.app_partition_id) {
            *out_handled = 1;
        }
        return H2_PAL_OK;
    }
    loader->status.loader_upgrade = record;
    *out_handled = 1;
    if (record.canonical_partition != loader->config.h2loader_partition_id ||
        record.trial_partition != loader->config.app_partition_id ||
        !config_accepts_board(&loader->config, record.candidate.board) ||
        strcmp(record.candidate.target, loader->config.target) != 0) {
        return upgrade_mark_recovery_failed(
            loader,
            &record,
            H2_PAL_ERR_INVALID_STATE,
            "record_identity");
    }
    if (record.phase == H2_LOADER_UPGRADE_PHASE_FAILED) {
        *out_handled = 0;
        return H2_PAL_OK;
    }
    if (record.phase == H2_LOADER_UPGRADE_PHASE_TRIAL_PENDING &&
        running.id == record.canonical_partition) {
        rc = h2_loader_image_verify(&loader->package, &record.candidate, record.trial_partition);
        if (rc != H2_PAL_OK) {
            return upgrade_mark_recovery_failed(
                loader,
                &record,
                rc,
                "trial_verify");
        }
        rc = upgrade_select_and_reboot(loader, record.trial_partition);
        if (rc == H2_PAL_OK && out_action != NULL) {
            *out_action = H2_LOADER_STARTUP_ACTION_REBOOTING_H2LOADER;
        }
        return rc == H2_PAL_OK ? rc : upgrade_mark_recovery_failed(
            loader,
            &record,
            rc,
            "trial_select");
    }
    if ((record.phase == H2_LOADER_UPGRADE_PHASE_TRIAL_PENDING ||
            record.phase == H2_LOADER_UPGRADE_PHASE_TRIAL_RUNNING) &&
        running.id == record.trial_partition) {
        if (!image_identity_equal(&loader->config.active_identity, &record.candidate)) {
            return upgrade_mark_recovery_failed(
                loader,
                &record,
                H2_PAL_ERR_FORMAT,
                "trial_identity");
        }
        rc = h2_loader_image_verify(
            &loader->package,
            &record.candidate,
            record.trial_partition);
        if (rc != H2_PAL_OK) {
            return upgrade_mark_recovery_failed(
                loader,
                &record,
                rc,
                "trial_verify");
        }
        if (loader->config.confirm_active_image != NULL) {
            rc = loader->config.confirm_active_image(
                loader->config.confirm_user);
            if (rc != H2_PAL_OK) {
                return upgrade_mark_recovery_failed(
                    loader,
                    &record,
                    rc,
                    "trial_confirm");
            }
        }
        record.phase = H2_LOADER_UPGRADE_PHASE_TRIAL_RUNNING;
        record.last_result = H2_PAL_OK;
        rc = upgrade_record_write(loader->config.pref, &record);
        if (rc != H2_PAL_OK) {
            return upgrade_mark_recovery_failed(
                loader,
                &record,
                rc,
                "trial_record");
        }
        rc = h2_loader_image_copy_to(
            &loader->package,
            &record.candidate,
            record.trial_partition,
            record.canonical_partition);
        if (rc != H2_PAL_OK) {
            return upgrade_mark_recovery_failed(
                loader,
                &record,
                rc,
                "canonical_copy");
        }
        record.phase = H2_LOADER_UPGRADE_PHASE_CANONICAL_PENDING;
        rc = upgrade_record_write(loader->config.pref, &record);
        if (rc != H2_PAL_OK) {
            return upgrade_mark_recovery_failed(
                loader,
                &record,
                rc,
                "canonical_record");
        }
        rc = upgrade_select_and_reboot(loader, record.canonical_partition);
        if (rc == H2_PAL_OK && out_action != NULL) {
            *out_action = H2_LOADER_STARTUP_ACTION_REBOOTING_H2LOADER;
        }
        return rc == H2_PAL_OK ? rc : upgrade_mark_recovery_failed(
            loader,
            &record,
            rc,
            "canonical_select");
    }
    if (record.phase == H2_LOADER_UPGRADE_PHASE_CANONICAL_PENDING &&
        running.id == record.trial_partition) {
        rc = h2_loader_image_verify(&loader->package, &record.candidate, record.canonical_partition);
        if (rc != H2_PAL_OK) {
            return upgrade_mark_recovery_failed(
                loader,
                &record,
                rc,
                "canonical_verify");
        }
        rc = upgrade_select_and_reboot(loader, record.canonical_partition);
        if (rc == H2_PAL_OK && out_action != NULL) {
            *out_action = H2_LOADER_STARTUP_ACTION_REBOOTING_H2LOADER;
        }
        return rc == H2_PAL_OK ? rc : upgrade_mark_recovery_failed(
            loader,
            &record,
            rc,
            "canonical_select");
    }
    if (record.phase == H2_LOADER_UPGRADE_PHASE_CANONICAL_PENDING &&
        running.id == record.canonical_partition) {
        if (!image_identity_equal(&loader->config.active_identity, &record.candidate)) {
            return upgrade_mark_recovery_failed(
                loader,
                &record,
                H2_PAL_ERR_FORMAT,
                "canonical_identity");
        }
        rc = h2_loader_image_verify(
            &loader->package,
            &record.candidate,
            record.canonical_partition);
        if (rc != H2_PAL_OK) {
            return upgrade_mark_recovery_failed(
                loader,
                &record,
                rc,
                "canonical_verify");
        }
        if (loader->config.confirm_active_image != NULL) {
            rc = loader->config.confirm_active_image(
                loader->config.confirm_user);
            if (rc != H2_PAL_OK) {
                return upgrade_mark_recovery_failed(
                    loader,
                    &record,
                    rc,
                    "canonical_confirm");
            }
        }
        rc = remove_staged_package(loader);
        if (rc != H2_PAL_OK) {
            return upgrade_mark_recovery_failed(
                loader,
                &record,
                rc,
                "staged_package_remove");
        }
        rc = clear_staged_state(loader);
        if (rc != H2_PAL_OK) {
            return upgrade_mark_recovery_failed(
                loader,
                &record,
                rc,
                "staged_state_clear");
        }
        rc = pref_set_state(
            loader->config.pref,
            H2_LOADER_INSTALL_STATE_IDLE,
            H2_LOADER_BOOT_INTENT_LOADER);
        if (rc != H2_PAL_OK) {
            return upgrade_mark_recovery_failed(
                loader,
                &record,
                rc,
                "loader_state_clear");
        }
        return upgrade_record_complete(loader->config.pref, &record);
    }
    return upgrade_mark_recovery_failed(
        loader,
        &record,
        H2_PAL_ERR_INVALID_STATE,
        "partition_state");
}

static int select_app_and_reboot_pending_confirm(h2_loader_t *loader) {
    int rc;

    if (loader == NULL || loader->config.power == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (loader->config.app_partition_id != 0u) {
        rc = h2_pal_power_set_next_boot_partition(loader->config.power, loader->config.app_partition_id);
        if (rc != H2_PAL_OK) {
            (void)pref_set_i32(loader->config.pref, "last_result", rc);
            return rc;
        }
    }
    rc = pref_set_state(loader->config.pref,
        H2_LOADER_INSTALL_STATE_INSTALLED_PENDING_CONFIRM,
        H2_LOADER_BOOT_INTENT_AUTO);
    if (rc != H2_PAL_OK) {
        if (loader->config.h2loader_partition_id != 0u) {
            int rollback_rc = h2_pal_power_set_next_boot_partition(
                loader->config.power,
                loader->config.h2loader_partition_id);
            if (rollback_rc != H2_PAL_OK) {
                (void)pref_set_i32(loader->config.pref, "last_result", rollback_rc);
                return rollback_rc;
            }
        }
        return rc;
    }
    return h2_pal_power_reboot(loader->config.power, H2_LOADER_REBOOT_REASON_DEFAULT);
}

int h2_loader_init(h2_loader_t *loader, const h2_loader_config_t *config) {
    int rc;

    if (loader == NULL || config == NULL || config->package.fs == NULL ||
        config->pref == NULL || config->power == NULL ||
        config->hardware_capabilities == 0u ||
        (config->hardware_capabilities & ~H2_LOADER_CAPABILITIES_ALL) != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(loader, 0, sizeof(*loader));
    mfg_gate_bypass_store(&loader->mfg_gate_bypass, 0);
    atomic_availability_store(
        &loader->implemented_commands,
        H2_LOADER_COMMAND_AVAILABILITY_INITIALIZED);
    atomic_availability_store(
        &loader->command_availability,
        H2_LOADER_COMMAND_AVAILABILITY_INITIALIZED |
            H2_LOADER_COMMAND_AVAILABILITY_ALL);
    loader->config = *config;
    loader->config.package.package_path =
        default_if_empty(config->package.package_path, H2_LOADER_DEFAULT_PACKAGE_PATH);
    loader->config.package.data_root =
        default_if_empty(config->package.data_root, H2_LOADER_DEFAULT_DATA_ROOT);
    loader->config.package.installed_checksum_path =
        default_if_empty(config->package.installed_checksum_path, H2_LOADER_DEFAULT_CHECKSUM_PATH);
    loader->config.package.app_entry_path =
        default_if_empty(config->package.app_entry_path, H2_LOADER_DEFAULT_APP_ENTRY_PATH);
    if (loader->config.package.app_partition_id == 0u) {
        loader->config.package.app_partition_id = config->app_partition_id;
    }
    rc = h2_loader_package_init(&loader->package, &loader->config.package);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return h2_loader_read_status(loader, &loader->status);
}

int h2_loader_startup(h2_loader_t *loader, h2_loader_startup_action_t *out_action) {
    h2_loader_stage_pref_snapshot_t staged_snapshot;
    h2_loader_identity_t staged_pref_identity;
    int rc;
    int staged_state;
    int upgrade_handled = 0;

    if (loader == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (out_action != NULL) {
        *out_action = H2_LOADER_STARTUP_ACTION_COMMAND_MODE;
    }

    rc = h2_loader_upgrade_recover(loader, out_action, &upgrade_handled);
    if (upgrade_handled) {
        if (rc != H2_PAL_OK) {
            loader->status.last_result = rc;
        }
        return H2_PAL_OK;
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }

    if (loader->config.confirm_active_image != NULL) {
        rc = loader->config.confirm_active_image(loader->config.confirm_user);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }

    if (loader->force_command_mode) {
        return H2_PAL_OK;
    }

    rc = h2_loader_read_status(loader, &loader->status);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (loader->status.install_state == H2_LOADER_INSTALL_STATE_CONFIRMED &&
        !loader->status.app_confirmed) {
        rc = pref_set_bool(loader->config.pref, "app_confirmed", 1);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        loader->status.app_confirmed = 1;
    }
    if (loader->status.install_state == H2_LOADER_INSTALL_STATE_STAGED) {
        rc = normalize_legacy_staged_lifecycle(loader, &loader->status);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        rc = h2_loader_read_status(loader, &loader->status);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    /* A durable install request was already accepted in a previous boot. It
       must be consumed before a fresh MFG gate decision, otherwise the device
       can acknowledge restart, reboot, and then strand the accepted App in
       Loader command mode. This bypass is volatile; the durable request is
       still the source of truth. */
    if (loader->status.install_state ==
            H2_LOADER_INSTALL_STATE_INSTALL_REQUESTED ||
        loader->status.install_state == H2_LOADER_INSTALL_STATE_INSTALLING) {
        mfg_gate_bypass_store(&loader->mfg_gate_bypass, 1);
    }
    if (!mfg_gate_satisfied(loader, &loader->status.mfg)) {
        return H2_PAL_OK;
    }
    if (loader->status.manual_hold ||
        loader->status.install_state == H2_LOADER_INSTALL_STATE_RETURN_REQUESTED ||
        loader->status.install_state == H2_LOADER_INSTALL_STATE_INSTALL_FAILED) {
        return H2_PAL_OK;
    }
    if (loader->status.install_state == H2_LOADER_INSTALL_STATE_MAIN_FAILED) {
        return H2_PAL_OK;
    }
    if (loader->status.install_state == H2_LOADER_INSTALL_STATE_INSTALLED_PENDING_CONFIRM) {
        rc = prepare_app_transition(loader);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        return recover_unconfirmed_app_to_loader(loader, H2_PAL_ERR_INVALID_STATE);
    }
    staged_state =
        loader->status.install_state == H2_LOADER_INSTALL_STATE_STAGED ||
        loader->status.install_state == H2_LOADER_INSTALL_STATE_INSTALLING ||
        loader->status.install_state == H2_LOADER_INSTALL_STATE_INSTALL_REQUESTED;
    if (staged_state) {
        rc = read_staged_pref_snapshot(loader, &staged_snapshot);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        rc = staged_snapshot_identity(&staged_snapshot, &staged_pref_identity);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (loader->status.install_state == H2_LOADER_INSTALL_STATE_INSTALL_REQUESTED &&
            loader->status.installed.valid &&
            (!staged_pref_identity.valid ||
                h2_loader_package_identity_equal(&staged_pref_identity, &loader->status.installed))) {
            rc = prepare_app_transition(loader);
            if (rc != H2_PAL_OK) {
                return rc;
            }
            emit_event(loader, H2_LOADER_STARTUP_EVENT_BOOT_APP, H2_PAL_OK);
            rc = boot_app_no_hook(loader);
            if (out_action != NULL && rc == H2_PAL_OK) {
                *out_action = H2_LOADER_STARTUP_ACTION_REBOOTING_APP;
            }
            if (rc != H2_PAL_OK) {
                return mark_main_failed(loader, rc);
            }
            return rc;
        }
    }
    if (staged_state) {
        rc = mount_loader_file_points(loader);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        rc = h2_loader_package_read_staged_identity(&loader->package, loader->config.pref, &loader->status.staged);
        if (rc != H2_PAL_OK) {
            (void)pref_set_i32(loader->config.pref, "last_result", rc);
            return rc;
        }
    }
    if (loader->status.install_state == H2_LOADER_INSTALL_STATE_INSTALL_REQUESTED &&
        !loader->status.staged.valid) {
        if (loader->status.installed.valid) {
            rc = prepare_app_transition(loader);
            if (rc != H2_PAL_OK) {
                return rc;
            }
            emit_event(loader, H2_LOADER_STARTUP_EVENT_BOOT_APP, H2_PAL_OK);
            rc = boot_app_no_hook(loader);
            if (out_action != NULL && rc == H2_PAL_OK) {
                *out_action = H2_LOADER_STARTUP_ACTION_REBOOTING_APP;
            }
            if (rc != H2_PAL_OK) {
                return mark_main_failed(loader, rc);
            }
            return rc;
        }
        return prepare_and_mark_main_failed(loader, H2_PAL_ERR_NOT_FOUND);
    }
    if (!loader->status.staged.valid &&
        (loader->status.install_state == H2_LOADER_INSTALL_STATE_STAGED ||
            loader->status.install_state == H2_LOADER_INSTALL_STATE_INSTALLING)) {
        return prepare_and_mark_main_failed(loader, H2_PAL_ERR_INVALID_STATE);
    }

    if (loader->status.staged.valid && staged_state) {
        h2_loader_package_inspection_t inspection;

        rc = h2_loader_package_inspect(
            &loader->package, loader->config.pref, &inspection);
        if (rc == H2_PAL_OK && !inspection.legacy &&
            inspection.manifest.role == H2_LOADER_IMAGE_ROLE_H2LOADER) {
            if (loader->status.install_state != H2_LOADER_INSTALL_STATE_STAGED ||
                loader->status.boot_intent != H2_LOADER_BOOT_INTENT_LOADER) {
                rc = pref_set_state(
                    loader->config.pref,
                    H2_LOADER_INSTALL_STATE_STAGED,
                    H2_LOADER_BOOT_INTENT_LOADER);
                if (rc != H2_PAL_OK) {
                    return rc;
                }
            }
            return H2_PAL_OK;
        }
    }

    if (loader->status.install_state == H2_LOADER_INSTALL_STATE_STAGED ||
        loader->status.install_state == H2_LOADER_INSTALL_STATE_INSTALLING ||
        loader->status.install_state == H2_LOADER_INSTALL_STATE_INSTALL_REQUESTED) {
        int pending_confirm_required =
            loader->status.install_state == H2_LOADER_INSTALL_STATE_INSTALLING ||
            !loader->status.app_confirmed;

        rc = prepare_app_transition(loader);
        if (rc != H2_PAL_OK) {
            return rc;
        }

        if (!h2_loader_package_identity_equal(&loader->status.staged, &loader->status.installed)) {
            h2_loader_package_inspection_t inspection;
            h2_loader_package_install_plan_t plan;
            h2_loader_package_install_result_t result;
            int had_confirmed_app = loader->status.app_confirmed;

            memset(&plan, 0, sizeof(plan));
            memset(&result, 0, sizeof(result));
            emit_event(loader, H2_LOADER_STARTUP_EVENT_INSTALL_BEGIN, H2_PAL_OK);
            rc = pref_set_state(loader->config.pref,
                H2_LOADER_INSTALL_STATE_INSTALLING,
                H2_LOADER_BOOT_INTENT_LOADER);
            if (rc != H2_PAL_OK) {
                return mark_install_failed(loader, rc);
            }
            rc = h2_loader_package_inspect(&loader->package, loader->config.pref, &inspection);
            if (rc == H2_PAL_OK &&
                !inspection.legacy &&
                (inspection.manifest.role != H2_LOADER_IMAGE_ROLE_APP ||
                    !config_accepts_board(
                        &loader->config,
                        inspection.manifest.board) ||
                    strcmp(inspection.manifest.target, loader->config.target) != 0)) {
                rc = H2_PAL_ERR_INVALID_STATE;
            }
            if (rc == H2_PAL_OK) {
                if (inspection.legacy) {
                    rc = pref_set_bool(
                        loader->config.pref,
                        "app_confirmed",
                        0);
                } else {
                    rc = h2_loader_package_plan_install(
                        &loader->package,
                        &inspection,
                        loader->config.app_partition_id,
                        &plan);
                    if (rc == H2_PAL_OK && plan.update_app) {
                        rc = pref_set_bool(
                            loader->config.pref,
                            "app_confirmed",
                            0);
                    }
                }
                if (rc == H2_PAL_OK && inspection.legacy) {
                    rc = h2_loader_package_install_staged(
                        &loader->package,
                        &loader->status.staged);
                }
                if (rc == H2_PAL_OK && !inspection.legacy) {
                    rc = h2_loader_package_install_to(
                        &loader->package,
                        &inspection,
                        loader->config.app_partition_id,
                        &plan,
                        &result);
                }
            }
            if (rc != H2_PAL_OK) {
                return mark_install_failed(loader, rc);
            }
            rc = pref_set_identity(loader->config.pref, "installed", &loader->status.staged);
            if (rc != H2_PAL_OK) {
                return mark_install_failed(loader, rc);
            }
            pending_confirm_required = inspection.legacy ||
                h2_loader_install_requires_app_confirmation(
                    result.app_written,
                    had_confirmed_app);
            if (!pending_confirm_required) {
                rc = pref_set_state(
                    loader->config.pref,
                    H2_LOADER_INSTALL_STATE_CONFIRMED,
                    H2_LOADER_BOOT_INTENT_AUTO);
                if (rc != H2_PAL_OK) {
                    return mark_install_failed(loader, rc);
                }
            }
        } else {
            emit_event(loader, H2_LOADER_STARTUP_EVENT_INSTALL_SKIP_SAME_IDENTITY, H2_PAL_OK);
        }
        emit_event(loader, H2_LOADER_STARTUP_EVENT_BOOT_APP, H2_PAL_OK);
        rc = pending_confirm_required ?
            select_app_and_reboot_pending_confirm(loader) :
            boot_app_no_hook(loader);
        if (out_action != NULL && rc == H2_PAL_OK) {
            *out_action = H2_LOADER_STARTUP_ACTION_REBOOTING_APP;
        }
        if (rc != H2_PAL_OK) {
            return mark_main_failed(loader, rc);
        }
        return rc;
    }
    if (loader->status.installed.valid) {
        rc = prepare_app_transition(loader);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        emit_event(loader, H2_LOADER_STARTUP_EVENT_BOOT_APP, H2_PAL_OK);
        rc = boot_app_no_hook(loader);
        if (out_action != NULL && rc == H2_PAL_OK) {
            *out_action = H2_LOADER_STARTUP_ACTION_REBOOTING_APP;
        }
        if (rc != H2_PAL_OK) {
            return mark_main_failed(loader, rc);
        }
        return rc;
    }

    return H2_PAL_OK;
}

int h2_loader_upgrade_start_with_transition(
    h2_loader_t *loader,
    h2_loader_upgrade_transition_fn transition,
    void *transition_user) {
    h2_loader_package_inspection_t inspection;
    h2_loader_package_install_plan_t install_plan;
    h2_loader_package_install_result_t install_result;
    h2_loader_upgrade_record_t existing;
    h2_loader_upgrade_record_t record;
    h2_pal_power_boot_partition_t running;
    uint64_t canonical_capacity;
    uint64_t trial_capacity = 0u;
    int present = 0;
    int rc;

    memset(&install_plan, 0, sizeof(install_plan));
    install_plan.update_app = 1;
    memset(&install_result, 0, sizeof(install_result));

    if (loader == NULL || loader->config.active_identity.role != H2_LOADER_IMAGE_ROLE_H2LOADER ||
        loader->config.h2loader_partition_id == 0u || loader->config.app_partition_id == 0u ||
        loader->config.h2loader_partition_id == loader->config.app_partition_id) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    rc = h2_loader_read_status(loader, &loader->status);
    if (rc != H2_PAL_OK) return rc;
    rc = require_command_available(
        loader, &loader->status,
        H2_LOADER_COMMAND_AVAILABLE_REBOOT_UPGRADE);
    if (rc != H2_PAL_OK) return rc;
    rc = upgrade_record_read(
        loader->config.pref,
        loader->config.package.allocator,
        &existing,
        &present);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (present && existing.phase != H2_LOADER_UPGRADE_PHASE_IDLE &&
        existing.phase != H2_LOADER_UPGRADE_PHASE_FAILED) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    rc = h2_pal_power_get_running_boot_partition(loader->config.power, &running);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (running.id != loader->config.h2loader_partition_id) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    rc = h2_loader_package_inspect(&loader->package, loader->config.pref, &inspection);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (inspection.legacy || inspection.manifest.role != H2_LOADER_IMAGE_ROLE_H2LOADER ||
        !config_accepts_board(&loader->config, inspection.manifest.board) ||
        strcmp(inspection.manifest.target, loader->config.target) != 0) {
        return H2_PAL_ERR_FORMAT;
    }
    if (present && existing.phase == H2_LOADER_UPGRADE_PHASE_FAILED) {
        rc = upgrade_record_remove(loader->config.pref, &existing);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    rc = h2_loader_image_verify(
        &loader->package,
        &inspection.manifest,
        loader->config.h2loader_partition_id);
    if (rc == H2_PAL_OK) {
        memset(&record, 0, sizeof(record));
        record.format = 1u;
        copy_text(record.package_sha256, sizeof(record.package_sha256),
            inspection.staged.checksum);
        record.candidate = inspection.manifest;
        record.canonical_partition = loader->config.h2loader_partition_id;
        record.trial_partition = loader->config.app_partition_id;
        rc = upgrade_record_complete(loader->config.pref, &record);
        if (rc == H2_PAL_OK) {
            rc = remove_staged_package(loader);
        }
        if (rc == H2_PAL_OK) {
            rc = clear_staged_state(loader);
        }
        if (rc == H2_PAL_OK) {
            rc = pref_set_state(
                loader->config.pref,
                H2_LOADER_INSTALL_STATE_IDLE,
                H2_LOADER_BOOT_INTENT_LOADER);
        }
        if (rc == H2_PAL_OK) {
            loader->status.loader_upgrade = record;
        }
        return rc;
    }
    if (rc != H2_PAL_ERR_FORMAT) {
        return rc;
    }
    rc = upgrade_partition_capacity(
        loader,
        loader->config.h2loader_partition_id,
        &canonical_capacity);
    if (rc == H2_PAL_OK) {
        rc = upgrade_partition_capacity(loader, loader->config.app_partition_id, &trial_capacity);
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if ((canonical_capacity > 0u && inspection.manifest.image_size > canonical_capacity) ||
        (trial_capacity > 0u && inspection.manifest.image_size > trial_capacity)) {
        return H2_PAL_ERR_NO_SPACE;
    }
    if (loader->config.before_disruptive != NULL) {
        rc = loader->config.before_disruptive(
            loader->config.disruptive_user,
            H2_LOADER_DISRUPTIVE_UPGRADE_H2LOADER);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    rc = clear_installed_state(loader);
    if (rc == H2_PAL_OK) {
        rc = pref_set_state(
            loader->config.pref,
            H2_LOADER_INSTALL_STATE_IDLE,
            H2_LOADER_BOOT_INTENT_LOADER);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_loader_package_install_to(
            &loader->package,
            &inspection,
            loader->config.app_partition_id,
            &install_plan,
            &install_result);
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    memset(&record, 0, sizeof(record));
    record.format = 1u;
    record.phase = H2_LOADER_UPGRADE_PHASE_TRIAL_PENDING;
    copy_text(record.package_sha256, sizeof(record.package_sha256), inspection.staged.checksum);
    record.candidate = inspection.manifest;
    record.canonical_partition = loader->config.h2loader_partition_id;
    record.trial_partition = loader->config.app_partition_id;
    record.last_result = H2_PAL_OK;
    rc = upgrade_record_write(loader->config.pref, &record);
    if (rc == H2_PAL_OK) {
        loader->status.loader_upgrade = record;
    }
    if (rc == H2_PAL_OK) {
        rc = upgrade_select_and_reboot_with_transition(
            loader, record.trial_partition, transition, transition_user);
    }
    if (rc != H2_PAL_OK) {
        return upgrade_mark_failed(loader, &record, rc, "start");
    }
    return H2_PAL_OK;
}

int h2_loader_upgrade_start(h2_loader_t *loader) {
    return h2_loader_upgrade_start_with_transition(loader, NULL, NULL);
}

int h2_loader_mark_return_requested(const h2_pal_pref_api_t *pref) {
    int rc = pref_set_state(pref,
        H2_LOADER_INSTALL_STATE_RETURN_REQUESTED,
        H2_LOADER_BOOT_INTENT_LOADER);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return pref_set_i32(pref, "last_result", H2_PAL_OK);
}

int h2_loader_mark_app_confirmed(const h2_pal_pref_api_t *pref) {
    int rc = pref_set_bool(pref, "app_confirmed", 1);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return pref_set_state(pref,
        H2_LOADER_INSTALL_STATE_CONFIRMED,
        H2_LOADER_BOOT_INTENT_AUTO);
}

static int remove_optional_file(
    const h2_pal_fs_api_t *fs,
    const char *path) {
    int rc;

    if (fs == NULL || path == NULL || path[0] == '\0') {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_pal_fs_remove(fs, path);
    return rc == H2_PAL_FS_ERR_NOT_FOUND ? H2_PAL_OK : rc;
}

int h2_loader_begin_stage_replacement(
    h2_loader_t *loader,
    const char *temporary_path,
    const char *previous_path) {
    const h2_pal_fs_api_t *fs;
    int first_error = H2_PAL_OK;
    int rc;

    if (loader == NULL || temporary_path == NULL || previous_path == NULL ||
        loader->config.package.fs == NULL ||
        loader->config.package.package_path == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_loader_stage_begin(loader->config.pref);
    if (rc != H2_PAL_OK) {
        first_error = rc;
    }
    fs = loader->config.package.fs;
    rc = remove_optional_file(fs, temporary_path);
    if (rc != H2_PAL_OK && first_error == H2_PAL_OK) {
        first_error = rc;
    }
    rc = remove_optional_file(fs, previous_path);
    if (rc != H2_PAL_OK && first_error == H2_PAL_OK) {
        first_error = rc;
    }
    rc = remove_optional_file(fs, loader->config.package.package_path);
    if (rc != H2_PAL_OK && first_error == H2_PAL_OK) {
        first_error = rc;
    }
    return first_error;
}

int h2_loader_prepare_stage_publish(h2_loader_t *loader) {
    if (loader == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_loader_stage_begin(loader->config.pref);
}

int h2_loader_set_last_result(h2_loader_t *loader, int result) {
    int rc;

    if (loader == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = pref_set_i32(loader->config.pref, "last_result", result);
    if (rc == H2_PAL_OK) {
        loader->status.last_result = result;
    }
    return rc;
}

int h2_loader_set_hold(h2_loader_t *loader, int enabled) {
    h2_pal_pref_namespace_t *ns = NULL;
    int rc;
    int close_rc;

    if (loader == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (loader->config.power != NULL) {
        rc = h2_pal_power_set_hold(loader->config.power, enabled);
        if (rc != H2_PAL_OK && rc != H2_PAL_ERR_UNSUPPORTED) {
            return rc;
        }
    }
    rc = pref_open(loader->config.pref, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ns == NULL || ns->set_bool == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->set_bool(ns, "manual_hold", enabled ? 1 : 0);
        if (rc == H2_PAL_OK && ns->commit != NULL) {
            rc = ns->commit(ns);
        }
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

static int validate_app_transition(h2_loader_t *loader) {
    h2_loader_status_t status;
    int rc;

    if (loader == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (loader->config.mfg_required_total > 0u &&
        mfg_gate_bypass_load(&loader->mfg_gate_bypass) == 0) {
        rc = h2_loader_read_status(loader, &status);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (!h2_loader_mfg_summary_is_passed(
                &status.mfg,
                loader->config.mfg_required_total)) {
            return H2_PAL_ERR_INVALID_STATE;
        }
    }
    return H2_PAL_OK;
}

static int boot_app_no_hook(h2_loader_t *loader) {
    int rc = validate_app_transition(loader);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return select_and_reboot(loader, loader->config.app_partition_id, H2_LOADER_BOOT_INTENT_AUTO);
}

int h2_loader_boot_app(h2_loader_t *loader) {
    int rc = validate_app_transition(loader);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = prepare_app_transition(loader);
    return rc == H2_PAL_OK ?
        select_and_reboot(loader, loader->config.app_partition_id,
                          H2_LOADER_BOOT_INTENT_AUTO) :
        rc;
}

int h2_loader_boot_h2loader(h2_loader_t *loader) {
    int rc;
    if (loader == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (loader->config.before_disruptive != NULL) {
        rc = loader->config.before_disruptive(
            loader->config.disruptive_user,
            H2_LOADER_DISRUPTIVE_BOOT_H2LOADER);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return select_and_reboot(loader, loader->config.h2loader_partition_id, H2_LOADER_BOOT_INTENT_LOADER);
}

static int reboot_partition_with_transition(
    h2_loader_t *loader,
    uint32_t partition_id,
    h2_loader_boot_intent_t intent,
    uint32_t command_bit,
    h2_loader_disruptive_action_t disruptive_action,
    h2_loader_reboot_transition_fn transition,
    void *transition_user) {
    int rc;

    if (loader == NULL || loader->config.power == NULL || partition_id == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_loader_read_status(loader, &loader->status);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = require_command_available(
        loader,
        &loader->status,
        command_bit);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_pal_power_set_next_boot_partition(loader->config.power, partition_id);
    if (rc != H2_PAL_OK) {
        (void)pref_set_i32(loader->config.pref, "last_result", rc);
        return rc;
    }
    rc = pref_set_u32(
        loader->config.pref,
        "boot_intent",
        (uint32_t)intent);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    loader->status.boot_intent = intent;
    if (transition != NULL) {
        rc = transition(transition_user);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    if (loader->config.before_disruptive != NULL) {
        rc = loader->config.before_disruptive(
            loader->config.disruptive_user,
            disruptive_action);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return h2_pal_power_reboot(loader->config.power, 0u);
}

int h2_loader_reboot_h2loader_with_transition(
    h2_loader_t *loader,
    h2_loader_reboot_transition_fn transition,
    void *transition_user) {
    return reboot_partition_with_transition(
        loader,
        loader != NULL ? loader->config.h2loader_partition_id : 0u,
        H2_LOADER_BOOT_INTENT_LOADER,
        H2_LOADER_COMMAND_AVAILABLE_REBOOT_LOADER,
        H2_LOADER_DISRUPTIVE_BOOT_H2LOADER,
        transition,
        transition_user);
}

int h2_loader_reboot_app_with_transition(
    h2_loader_t *loader,
    h2_loader_reboot_transition_fn transition,
    void *transition_user) {
    return reboot_partition_with_transition(
        loader,
        loader != NULL ? loader->config.app_partition_id : 0u,
        H2_LOADER_BOOT_INTENT_AUTO,
        H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP,
        H2_LOADER_DISRUPTIVE_BOOT_APP,
        transition,
        transition_user);
}

int h2_loader_reboot_upgrade_with_transition(
    h2_loader_t *loader,
    h2_loader_reboot_transition_fn transition,
    void *transition_user) {
    return reboot_partition_with_transition(
        loader,
        loader != NULL ? loader->config.h2loader_partition_id : 0u,
        H2_LOADER_BOOT_INTENT_AUTO,
        H2_LOADER_COMMAND_AVAILABLE_REBOOT_UPGRADE,
        H2_LOADER_DISRUPTIVE_BOOT_H2LOADER,
        transition,
        transition_user);
}

int h2_loader_reboot_h2loader(h2_loader_t *loader) {
    return h2_loader_reboot_h2loader_with_transition(
        loader, NULL, NULL);
}

int h2_loader_read_status(h2_loader_t *loader, h2_loader_status_t *out_status) {
    h2_loader_status_t status;
    h2_pal_power_boot_partition_t partition;
    int upgrade_present = 0;
    int rc;

    if (loader == NULL || out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_loader_read_pref_status(loader->config.pref, loader->config.package.allocator, &status);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_loader_status_set_device(&status, loader->config.board, loader->config.target, loader->config.chip);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_loader_status_set_active(
        &status,
        H2_LOADER_ACTIVE_ROLE_H2LOADER,
        "h2loader",
        loader->config.active_identity.version,
        loader->config.active_identity.image_sha256);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    status.capabilities = loader->config.hardware_capabilities;
    if (status.stage.valid) {
        status.staged.valid = 1;
        status.staged.size = status.stage.package_size;
        copy_text(status.staged.checksum, sizeof(status.staged.checksum),
            status.stage.package_checksum);
        copy_text(status.staged.version, sizeof(status.staged.version),
            status.stage.version);
    } else {
        rc = h2_loader_package_read_staged_identity(
            &loader->package, loader->config.pref, &status.staged);
        if (rc != H2_PAL_OK) status.staged.valid = 0;
    }
    rc = h2_pal_power_get_running_boot_partition(loader->config.power, &partition);
    if (rc == H2_PAL_OK) {
        status.running_partition_id = partition.id;
    }
    rc = h2_pal_power_get_next_boot_partition(loader->config.power, &partition);
    if (rc == H2_PAL_OK) {
        status.next_partition_id = partition.id;
    }
    rc = upgrade_record_read(
        loader->config.pref,
        loader->config.package.allocator,
        &status.loader_upgrade,
        &upgrade_present);
    if (rc == H2_PAL_ERR_FORMAT) {
        memset(&status.loader_upgrade, 0, sizeof(status.loader_upgrade));
        status.loader_upgrade.phase = H2_LOADER_UPGRADE_PHASE_CORRUPT;
    } else if (rc != H2_PAL_OK || !upgrade_present) {
        memset(&status.loader_upgrade, 0, sizeof(status.loader_upgrade));
        status.loader_upgrade.phase = H2_LOADER_UPGRADE_PHASE_IDLE;
    }
    status.upgrade_phase_known = 1u;
    status.command_availability =
        h2_loader_get_command_availability(loader, &status);
    rc = h2_loader_states_pack(&status, &status.states);
    if (rc != H2_PAL_OK) return rc;
    loader->status = status;
    *out_status = status;
    return H2_PAL_OK;
}

int h2_loader_publish_stage(h2_loader_t *loader, uint32_t bytes, const char *sha256) {
    int rc;

    if (loader == NULL || sha256 == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_loader_stage_publish(
        &loader->package,
        loader->config.pref,
        bytes,
        sha256,
        &loader->status.stage);
    if (rc != H2_PAL_OK) {
        cleanup_failed_stage_publish(loader);
    }
    return rc;
}

int h2_loader_abort_stage(h2_loader_t *loader) {
    int rc;

    if (loader == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_loader_stage_abort(
        loader->config.package.fs,
        loader->config.pref,
        loader->config.package.package_path);
    if (rc == H2_PAL_OK) memset(&loader->status.stage, 0, sizeof(loader->status.stage));
    return rc;
}

static int request_install_staged(h2_loader_t *loader) {
    h2_pal_pref_namespace_t *ns = NULL;
    h2_loader_status_t status;
    h2_loader_install_state_t next_state;
    int retry_pending_confirm;
    int rc;
    int close_rc;

    if (loader == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_loader_read_status(loader, &status);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (!status.stage.valid && !status.staged.valid) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    rc = require_command_available(
        loader,
        &status,
        H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (!mfg_gate_satisfied(loader, &status.mfg)) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    retry_pending_confirm =
        status.install_state == H2_LOADER_INSTALL_STATE_MAIN_FAILED &&
        h2_loader_package_identity_equal(&status.staged, &status.installed);
    next_state = retry_pending_confirm ?
        H2_LOADER_INSTALL_STATE_INSTALLING :
        H2_LOADER_INSTALL_STATE_INSTALL_REQUESTED;
    if (loader->config.power != NULL) {
        rc = h2_pal_power_set_hold(loader->config.power, 0);
        if (rc != H2_PAL_OK && rc != H2_PAL_ERR_UNSUPPORTED) {
            return rc;
        }
    }
    rc = pref_open(loader->config.pref, H2_PAL_PREF_OPEN_READ_WRITE, &ns);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (ns == NULL || ns->set_bool == NULL || ns->set_u32 == NULL ||
        ns->commit == NULL) {
        rc = H2_PAL_ERR_UNSUPPORTED;
    } else {
        rc = ns->set_bool(ns, "manual_hold", 0);
        if (rc == H2_PAL_OK) {
            rc = ns->set_u32(ns, "install_state", (uint32_t)next_state);
        }
        if (rc == H2_PAL_OK) {
            rc = ns->set_u32(
                ns, "boot_intent", (uint32_t)H2_LOADER_BOOT_INTENT_AUTO);
        }
        if (rc == H2_PAL_OK) {
            rc = ns->commit(ns);
            if (rc == H2_PAL_OK) {
                loader->status.manual_hold = 0;
                loader->status.install_state = next_state;
                loader->status.boot_intent = H2_LOADER_BOOT_INTENT_AUTO;
            }
        }
    }
    close_rc = ns != NULL && ns->close != NULL ? ns->close(ns) : H2_PAL_OK;
    return rc == H2_PAL_OK ? close_rc : rc;
}

int h2_loader_request_install_staged_with_transition(
    h2_loader_t *loader,
    h2_loader_install_transition_fn transition,
    void *transition_user) {
    int rc = request_install_staged(loader);
    if (rc == H2_PAL_OK && transition != NULL) {
        rc = transition(transition_user);
    }
    if (rc == H2_PAL_OK) {
        rc = prepare_app_transition(loader);
    }
    return rc;
}

int h2_loader_request_install_staged(h2_loader_t *loader) {
    return h2_loader_request_install_staged_with_transition(
        loader, NULL, NULL);
}

int h2_loader_install_staged_with_transition(
    h2_loader_t *loader,
    h2_loader_install_transition_fn transition,
    void *transition_user) {
    h2_loader_startup_action_t action;
    int rc = request_install_staged(loader);
    if (rc == H2_PAL_OK && transition != NULL) {
        rc = transition(transition_user);
    }
    return rc == H2_PAL_OK ? h2_loader_startup(loader, &action) : rc;
}

int h2_loader_install_staged(h2_loader_t *loader) {
    return h2_loader_install_staged_with_transition(loader, NULL, NULL);
}
