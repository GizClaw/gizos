#include "h2_h2loader_host.h"

#include "h2_h2loader_host_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int h2_h2loader_host_copy_text(
    char *out,
    size_t out_size,
    const char *value,
    size_t value_len) {
    if (out == NULL || out_size == 0u || value == NULL ||
        value_len == 0u || value_len >= out_size) {
        return 0;
    }
    memcpy(out, value, value_len);
    out[value_len] = '\0';
    return 1;
}

int h2_h2loader_host_is_safe_identity(const char *value) {
    size_t len;

    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    len = strlen(value);
    if (len >= H2_H2LOADER_HOST_IDENTITY_MAX_LEN) {
        return 0;
    }
    for (size_t i = 0u; i < len; ++i) {
        unsigned char byte = (unsigned char)value[i];
        if (!((byte >= 'a' && byte <= 'z') ||
              (byte >= 'A' && byte <= 'Z') ||
              (byte >= '0' && byte <= '9') ||
              byte == '.' || byte == '_' || byte == '-')) {
            return 0;
        }
    }
    return 1;
}

int h2_h2loader_host_is_sha256(const char *value) {
    if (value == NULL ||
        strlen(value) != H2_H2LOADER_HOST_SHA256_HEX_LEN) {
        return 0;
    }
    for (size_t i = 0u; i < H2_H2LOADER_HOST_SHA256_HEX_LEN; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

int h2_h2loader_host_is_safe_resource_name(const char *value) {
    const char *segment;
    size_t len;

    if (value == NULL || value[0] == '\0' || value[0] == '/' ||
        strchr(value, '\\') != NULL) {
        return 0;
    }
    len = strlen(value);
    if (len >= H2_H2LOADER_HOST_RESOURCE_NAME_MAX_LEN ||
        value[len - 1u] == '/') {
        return 0;
    }
    segment = value;
    for (const char *cursor = value;; ++cursor) {
        if (*cursor != '/' && *cursor != '\0') {
            unsigned char byte = (unsigned char)*cursor;
            if (byte < 0x21u || byte > 0x7eu) {
                return 0;
            }
            continue;
        }
        size_t segment_len = (size_t)(cursor - segment);
        if (segment_len == 0u ||
            (segment_len == 1u && segment[0] == '.') ||
            (segment_len == 2u && segment[0] == '.' && segment[1] == '.')) {
            return 0;
        }
        if (*cursor == '\0') {
            break;
        }
        segment = cursor + 1;
    }
    return 1;
}

static int parse_u32(const char *value, size_t len, uint32_t *out_value) {
    char text[32];
    char *end = NULL;
    unsigned long parsed;

    if (len == 0u || len >= sizeof(text)) {
        return 0;
    }
    memcpy(text, value, len);
    text[len] = '\0';
    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed > UINT32_MAX) {
        return 0;
    }
    *out_value = (uint32_t)parsed;
    return 1;
}

static int parse_u64(const char *value, size_t len, uint64_t *out_value) {
    char text[32];
    char *end = NULL;
    unsigned long long parsed;

    if (len == 0u || len >= sizeof(text)) {
        return 0;
    }
    memcpy(text, value, len);
    text[len] = '\0';
    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return 0;
    }
    *out_value = (uint64_t)parsed;
    return 1;
}

static int is_fixed_lower_hex(const char *value, size_t len) {
    if (value == NULL || len < 3u || value[0] != '0' || value[1] != 'x') {
        return 0;
    }
    for (size_t i = 2u; i < len; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int parse_i32(const char *value, size_t len, int32_t *out_value) {
    char text[32];
    char *end = NULL;
    long parsed;

    if (len == 0u || len >= sizeof(text)) {
        return 0;
    }
    memcpy(text, value, len);
    text[len] = '\0';
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed < INT32_MIN || parsed > INT32_MAX) {
        return 0;
    }
    *out_value = (int32_t)parsed;
    return 1;
}

static int take_field(
    const char **cursor,
    const char *name,
    const char **out_value,
    size_t *out_len) {
    const size_t name_len = strlen(name);
    const char *value;
    const char *end;
    if (strncmp(*cursor, name, name_len) != 0 || (*cursor)[name_len] != '=') {
        return 0;
    }
    value = *cursor + name_len + 1u;
    end = value;
    while (*end != '\0' && *end != '\r' && *end != '\n' && *end != ' ') {
        ++end;
    }
    *out_value = value;
    *out_len = (size_t)(end - value);
    if (*end == ' ') {
        *cursor = end + 1u;
        return **cursor != ' ' && **cursor != '\0' &&
            **cursor != '\r' && **cursor != '\n';
    }
    *cursor = end;
    return 1;
}

static int copy_field(
    const char **cursor,
    const char *name,
    char *out,
    size_t out_size) {
    const char *value;
    size_t len;
    if (!take_field(cursor, name, &value, &len)) return 0;
    if (len == 0u) {
        out[0] = '\0';
        return 1;
    }
    return h2_h2loader_host_copy_text(out, out_size, value, len);
}

#define H2_STATES_ROLE_MASK UINT64_C(0x3)
#define H2_STATES_INTENT_MASK (UINT64_C(0x3) << 2u)
#define H2_STATES_INSTALL_MASK (UINT64_C(0xf) << 4u)
#define H2_STATES_UPGRADE_MASK (UINT64_C(0x7) << 8u)
#define H2_STATES_FLAGS_KNOWN (UINT64_C(1) << 11u)
#define H2_STATES_APP_CONFIRMED (UINT64_C(1) << 12u)
#define H2_STATES_MANUAL_HOLD (UINT64_C(1) << 13u)
#define H2_STATES_INSTALLED_VALID (UINT64_C(1) << 14u)
#define H2_STATES_STAGED_VALID (UINT64_C(1) << 15u)
#define H2_STATES_MFG_MASK (UINT64_C(0x3) << 16u)
#define H2_STATES_RESERVED_MASK (UINT64_C(0x3) << 62u)

static uint32_t state_field(uint64_t states, uint64_t mask, uint32_t shift) {
    return (uint32_t)((states & mask) >> shift);
}

h2_h2loader_host_active_role_t h2_h2loader_host_status_active_role(
    const h2_h2loader_host_status_t *status) {
    return status == NULL ? H2_H2LOADER_HOST_ACTIVE_ROLE_UNKNOWN :
        (h2_h2loader_host_active_role_t)state_field(
            status->states, H2_STATES_ROLE_MASK, 0u);
}
uint32_t h2_h2loader_host_status_boot_intent(const h2_h2loader_host_status_t *status) {
    return status == NULL ? 0u : state_field(status->states, H2_STATES_INTENT_MASK, 2u);
}
uint32_t h2_h2loader_host_status_install_state(const h2_h2loader_host_status_t *status) {
    return status == NULL ? 0u : state_field(status->states, H2_STATES_INSTALL_MASK, 4u);
}
uint32_t h2_h2loader_host_status_upgrade_phase(const h2_h2loader_host_status_t *status) {
    return status == NULL ? 0u : state_field(status->states, H2_STATES_UPGRADE_MASK, 8u);
}
uint32_t h2_h2loader_host_status_mfg_mode(const h2_h2loader_host_status_t *status) {
    return status == NULL ? 0u : state_field(status->states, H2_STATES_MFG_MASK, 16u);
}
uint32_t h2_h2loader_host_status_mfg_step(
    const h2_h2loader_host_status_t *status, uint32_t index) {
    return status == NULL || index >= 22u ? UINT32_MAX :
        state_field(status->states, UINT64_C(0x3) << (18u + index * 2u),
                    18u + index * 2u);
}
int h2_h2loader_host_status_flags_known(const h2_h2loader_host_status_t *status) {
    return status != NULL && (status->states & H2_STATES_FLAGS_KNOWN) != 0u;
}
int h2_h2loader_host_status_app_confirmed(const h2_h2loader_host_status_t *status) {
    return status != NULL && (status->states & H2_STATES_APP_CONFIRMED) != 0u;
}
int h2_h2loader_host_status_manual_hold(const h2_h2loader_host_status_t *status) {
    return status != NULL && (status->states & H2_STATES_MANUAL_HOLD) != 0u;
}
int h2_h2loader_host_status_installed_valid(const h2_h2loader_host_status_t *status) {
    return status != NULL && (status->states & H2_STATES_INSTALLED_VALID) != 0u;
}
int h2_h2loader_host_status_staged_valid(const h2_h2loader_host_status_t *status) {
    return status != NULL && (status->states & H2_STATES_STAGED_VALID) != 0u;
}

static int states_valid(uint64_t states) {
    const uint64_t lifecycle = H2_STATES_APP_CONFIRMED |
        H2_STATES_MANUAL_HOLD | H2_STATES_INSTALLED_VALID |
        H2_STATES_STAGED_VALID;
    const uint32_t role = state_field(states, H2_STATES_ROLE_MASK, 0u);
    const uint32_t intent = state_field(states, H2_STATES_INTENT_MASK, 2u);
    const uint32_t install = state_field(states, H2_STATES_INSTALL_MASK, 4u);
    const uint32_t upgrade = state_field(states, H2_STATES_UPGRADE_MASK, 8u);
    const uint32_t mfg = state_field(states, H2_STATES_MFG_MASK, 16u);
    if ((states & H2_STATES_RESERVED_MASK) != 0u || role == 0u || role == 3u ||
        intent == 3u || install > 9u || upgrade > 6u || mfg == 0u || mfg == 3u ||
        ((states & H2_STATES_FLAGS_KNOWN) == 0u && (states & lifecycle) != 0u)) {
        return 0;
    }
    if (mfg != 2u) {
        for (uint32_t i = 0u; i < 22u; ++i) {
            if (state_field(states, UINT64_C(0x3) << (18u + i * 2u),
                            18u + i * 2u) != 0u) return 0;
        }
    }
    return 1;
}

h2_pal_result_t h2_h2loader_host_status_parse(
    const char *line,
    h2_h2loader_host_status_t *out_status) {
    static const char marker[] = "H2_LOADER_STATUS ";
    const char *cursor;
    const char *value;
    size_t len;

    if (out_status != NULL) {
        memset(out_status, 0, sizeof(*out_status));
    }
    if (line == NULL || out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (strncmp(line, marker, sizeof(marker) - 1u) != 0) goto format_error;
    cursor = line + sizeof(marker) - 1u;
#define COPY(name, member) \
    if (!copy_field(&cursor, name, out_status->member, sizeof(out_status->member))) goto format_error
#define U32(name, member) \
    if (!take_field(&cursor, name, &value, &len) || !parse_u32(value, len, &out_status->member)) goto format_error
#define U64(name, member) \
    if (!take_field(&cursor, name, &value, &len) || !parse_u64(value, len, &out_status->member)) goto format_error
#define I32(name, member) \
    if (!take_field(&cursor, name, &value, &len) || !parse_i32(value, len, &out_status->member)) goto format_error
    COPY("board", board); COPY("target", target); COPY("chip", chip);
    if (!take_field(&cursor, "capabilities", &value, &len) || len != 10u ||
        !is_fixed_lower_hex(value, len) || !parse_u32(value, len, &out_status->capabilities)) goto format_error;
    if (!take_field(&cursor, "command_availability", &value, &len) || len != 10u ||
        !is_fixed_lower_hex(value, len) || !parse_u32(value, len, &out_status->command_availability)) goto format_error;
    if (!take_field(&cursor, "states", &value, &len) || len != 18u ||
        !is_fixed_lower_hex(value, len) || !parse_u64(value, len, &out_status->states)) goto format_error;
    COPY("active_name", active_name); COPY("active_version", active_version);
    COPY("active_checksum", active_checksum); I32("last", last);
    COPY("installed_version", installed_version); COPY("installed_checksum", installed_checksum);
    COPY("staged_version", staged_version); COPY("staged_checksum", staged_checksum);
    U64("staged_bytes", staged_bytes); U32("running_partition", running_partition);
    U32("next_partition", next_partition); U32("canonical_partition", canonical_partition);
    U32("trial_partition", trial_partition); I32("upgrade_last", upgrade_last);
    COPY("upgrade_step", upgrade_step); COPY("upgrade_package_sha256", upgrade_package_sha256);
    COPY("candidate_board", candidate_board); COPY("candidate_target", candidate_target);
    COPY("candidate_version", candidate_version); U64("candidate_bytes", candidate_bytes);
    COPY("candidate_sha256", candidate_sha256);
#undef COPY
#undef U32
#undef U64
#undef I32
    if (*cursor == '\r') ++cursor;
    if (*cursor == '\n') ++cursor;
    if (*cursor != '\0') goto format_error;
    if (!h2_h2loader_host_is_safe_identity(out_status->board) ||
        !h2_h2loader_host_is_safe_identity(out_status->target) ||
        !h2_h2loader_host_is_safe_identity(out_status->chip) ||
        !h2_h2loader_host_is_safe_identity(out_status->active_name) ||
        (out_status->capabilities & ~H2_H2LOADER_HOST_CAPABILITIES_ALL) != 0u ||
        (out_status->command_availability & ~H2_H2LOADER_HOST_COMMAND_AVAILABILITY_ALL) != 0u ||
        !states_valid(out_status->states)) goto format_error;
    if ((out_status->active_checksum[0] != '\0' &&
         !h2_h2loader_host_is_sha256(out_status->active_checksum)) ||
        (out_status->installed_checksum[0] != '\0' &&
         !h2_h2loader_host_is_sha256(out_status->installed_checksum)) ||
        (out_status->staged_checksum[0] != '\0' &&
         !h2_h2loader_host_is_sha256(out_status->staged_checksum)) ||
        (out_status->upgrade_package_sha256[0] != '\0' &&
         !h2_h2loader_host_is_sha256(
             out_status->upgrade_package_sha256)) ||
        (out_status->candidate_sha256[0] != '\0' &&
         !h2_h2loader_host_is_sha256(out_status->candidate_sha256))) goto format_error;
    if (h2_h2loader_host_status_installed_valid(out_status) !=
        (out_status->installed_version[0] != '\0' &&
         out_status->installed_checksum[0] != '\0')) goto format_error;
    if (h2_h2loader_host_status_staged_valid(out_status) !=
        (out_status->staged_version[0] != '\0' &&
         out_status->staged_checksum[0] != '\0')) goto format_error;
    if ((out_status->installed_version[0] != '\0' &&
         !h2_h2loader_host_is_safe_identity(out_status->installed_version)) ||
        (out_status->staged_version[0] != '\0' &&
         !h2_h2loader_host_is_safe_identity(out_status->staged_version)) ||
        (!h2_h2loader_host_status_staged_valid(out_status) &&
         out_status->staged_bytes != 0u) ||
        (h2_h2loader_host_status_staged_valid(out_status) &&
         out_status->staged_bytes == 0u)) goto format_error;
    if ((out_status->candidate_board[0] == '\0') !=
        (out_status->candidate_target[0] == '\0') ||
        (out_status->candidate_board[0] == '\0') !=
        (out_status->candidate_version[0] == '\0') ||
        (out_status->candidate_board[0] == '\0') !=
        (out_status->candidate_sha256[0] == '\0') ||
        (out_status->candidate_board[0] == '\0' &&
         out_status->candidate_bytes != 0u) ||
        (out_status->candidate_board[0] != '\0' &&
         (out_status->candidate_bytes == 0u ||
          !h2_h2loader_host_is_safe_identity(out_status->candidate_board) ||
          !h2_h2loader_host_is_safe_identity(out_status->candidate_target) ||
          !h2_h2loader_host_is_safe_identity(out_status->candidate_version)))) goto format_error;
    return H2_PAL_OK;
format_error:
    memset(out_status, 0, sizeof(*out_status));
    return H2_PAL_ERR_FORMAT;
}

h2_pal_result_t h2_h2loader_host_status_verify_asset(
    const h2_h2loader_host_status_t *status,
    const h2_h2loader_host_catalog_entry_t *asset) {
    if (status == NULL || asset == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (strcmp(status->board, asset->board) != 0 ||
        strcmp(status->target, asset->target) != 0) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (asset->role == H2_H2LOADER_HOST_ASSET_ROLE_APP) {
        int package_manifest = asset->identity_source ==
            H2_H2LOADER_HOST_ASSET_IDENTITY_PACKAGE_MANIFEST;
        if (h2_h2loader_host_status_active_role(status) !=
                H2_H2LOADER_HOST_ACTIVE_ROLE_APP ||
            h2_h2loader_host_status_staged_valid(status) ||
            (!package_manifest &&
             strcmp(status->active_name, asset->image) != 0) ||
            (package_manifest &&
             strcmp(status->active_version, asset->version) != 0) ||
            h2_h2loader_host_status_install_state(status) != 6u ||
            !h2_h2loader_host_status_app_confirmed(status) ||
            !h2_h2loader_host_status_installed_valid(status) ||
            strcmp(status->installed_checksum, asset->sha256) != 0 ||
            !h2_h2loader_host_is_sha256(asset->image_sha256) ||
            (status->active_checksum[0] != '\0' &&
             strcmp(
                 status->active_checksum,
                 asset->image_sha256) != 0)) {
            return H2_PAL_ERR_INVALID_STATE;
        }
        return H2_PAL_OK;
    }
    if (asset->role == H2_H2LOADER_HOST_ASSET_ROLE_LOADER) {
        int package_manifest = asset->identity_source ==
            H2_H2LOADER_HOST_ASSET_IDENTITY_PACKAGE_MANIFEST;
        if (h2_h2loader_host_status_active_role(status) !=
                H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER ||
            h2_h2loader_host_status_staged_valid(status) ||
            strcmp(status->active_version, asset->version) != 0 ||
            (package_manifest &&
             (!h2_h2loader_host_is_sha256(status->active_checksum) ||
              strcmp(status->active_checksum, asset->image_sha256) != 0)) ||
            h2_h2loader_host_status_upgrade_phase(status) != 1u) {
            return H2_PAL_ERR_INVALID_STATE;
        }
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_INVALID_ARG;
}
