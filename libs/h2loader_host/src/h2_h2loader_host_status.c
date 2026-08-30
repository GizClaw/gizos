#include "h2_h2loader_host.h"

#include "h2_h2loader_host_internal.h"

#include <errno.h>
#include <stdio.h>
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

static int parse_u64(const char *value, size_t len, int base, uint64_t *out_value) {
    char text[32];
    char *end = NULL;
    unsigned long long parsed;

    if (len == 0u || len >= sizeof(text)) {
        return 0;
    }
    memcpy(text, value, len);
    text[len] = '\0';
    errno = 0;
    parsed = strtoull(text, &end, base);
    if (errno != 0 || end == text || *end != '\0') {
        return 0;
    }
    *out_value = (uint64_t)parsed;
    return 1;
}

static int parse_decimal_u64(
    const char *value,
    size_t len,
    uint64_t *out_value) {
    if (value == NULL || len == 0u) return 0;
    for (size_t i = 0u; i < len; ++i) {
        if (value[i] < '0' || value[i] > '9') return 0;
    }
    return parse_u64(value, len, 10, out_value);
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

static int copy_optional_field(
    const char **cursor,
    const char *name,
    char *out,
    size_t out_size) {
    const char *value;
    size_t len;
    if (!take_field(cursor, name, &value, &len)) return 0;
    if (len == 1u && value[0] == '-') {
        out[0] = '\0';
        return 1;
    }
    return h2_h2loader_host_copy_text(out, out_size, value, len);
}

static int parse_role_value(
    const char *value,
    size_t len,
    h2_h2loader_host_active_role_t *out_role) {
    if (len == 3u && strncmp(value, "app", len) == 0) {
        *out_role = H2_H2LOADER_HOST_ACTIVE_ROLE_APP;
        return 1;
    }
    if (len == 6u && strncmp(value, "loader", len) == 0) {
        *out_role = H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER;
        return 1;
    }
    if (len == 7u && strncmp(value, "unknown", len) == 0) {
        *out_role = H2_H2LOADER_HOST_ACTIVE_ROLE_UNKNOWN;
        return 1;
    }
    return 0;
}

static int parse_metadata_field(
    const char **cursor,
    const char *prefix,
    h2_h2loader_host_metadata_t *out) {
    char name[64];
    const char *value;
    size_t len;
    uint32_t valid;
#define META_NAME(suffix) \
    do { \
        if (snprintf(name, sizeof(name), "%s_%s", prefix, suffix) < 0 || \
            strlen(name) >= sizeof(name)) return 0; \
    } while (0)
    META_NAME("valid");
    if (!take_field(cursor, name, &value, &len) ||
        !parse_u32(value, len, &valid) || valid > 1u) return 0;
    out->valid = (uint8_t)valid;
    META_NAME("package_checksum");
    if (!copy_optional_field(cursor, name, out->package_checksum,
            sizeof(out->package_checksum))) return 0;
    META_NAME("package_size");
    if (!take_field(cursor, name, &value, &len) ||
        !parse_decimal_u64(value, len, &out->package_size)) return 0;
    META_NAME("image_checksum");
    if (!copy_optional_field(cursor, name, out->image_checksum,
            sizeof(out->image_checksum))) return 0;
    META_NAME("image_size");
    if (!take_field(cursor, name, &value, &len) ||
        !parse_decimal_u64(value, len, &out->image_size)) return 0;
    META_NAME("role");
    if (!take_field(cursor, name, &value, &len) ||
        !parse_role_value(value, len, &out->role)) return 0;
    META_NAME("version");
    if (!copy_optional_field(cursor, name, out->version,
            sizeof(out->version))) return 0;
    META_NAME("board");
    if (!copy_optional_field(cursor, name, out->board,
            sizeof(out->board))) return 0;
    META_NAME("target");
    if (!copy_optional_field(cursor, name, out->target,
            sizeof(out->target))) return 0;
#undef META_NAME
    return 1;
}

h2_h2loader_host_active_role_t h2_h2loader_host_status_active_role(
    const h2_h2loader_host_status_t *status) {
    return status == NULL
        ? H2_H2LOADER_HOST_ACTIVE_ROLE_UNKNOWN
        : status->active_role;
}
uint32_t h2_h2loader_host_status_boot_intent(const h2_h2loader_host_status_t *status) {
    return status == NULL ? 0u : (uint32_t)status->boot_intent;
}
uint32_t h2_h2loader_host_status_mfg_mode(const h2_h2loader_host_status_t *status) {
    return status == NULL ? 0u : status->mfg_mode;
}
uint32_t h2_h2loader_host_status_mfg_step(
    const h2_h2loader_host_status_t *status, uint32_t index) {
    return status == NULL || index >= H2_H2LOADER_HOST_MFG_STEP_TOTAL
        ? UINT32_MAX : status->mfg_steps[index];
}

static int metadata_valid(
    const h2_h2loader_host_metadata_t *metadata,
    int stage) {
    if ((metadata->package_checksum[0] != '\0' &&
         !h2_h2loader_host_is_sha256(metadata->package_checksum)) ||
        (metadata->image_checksum[0] != '\0' &&
         !h2_h2loader_host_is_sha256(metadata->image_checksum)) ||
        (metadata->version[0] != '\0' &&
         !h2_h2loader_host_is_safe_identity(metadata->version)) ||
        (metadata->board[0] != '\0' &&
         !h2_h2loader_host_is_safe_identity(metadata->board)) ||
        (metadata->target[0] != '\0' &&
         !h2_h2loader_host_is_safe_identity(metadata->target))) {
        return 0;
    }
    if (!metadata->valid) return 1;
    return metadata->image_checksum[0] != '\0' && metadata->image_size != 0u &&
        metadata->role != H2_H2LOADER_HOST_ACTIVE_ROLE_UNKNOWN &&
        metadata->version[0] != '\0' && metadata->board[0] != '\0' &&
        metadata->target[0] != '\0' &&
        (!stage || (metadata->package_checksum[0] != '\0' &&
                    metadata->package_size != 0u));
}

static h2_pal_result_t parse_status_v2(
    const char *cursor,
    h2_h2loader_host_status_t *out_status) {
    const char *value;
    size_t len;

    if (!take_field(&cursor, "active_role", &value, &len) ||
        !parse_role_value(value, len, &out_status->active_role) ||
        !copy_optional_field(&cursor, "active_version",
            out_status->active_version, sizeof(out_status->active_version)) ||
        !copy_optional_field(&cursor, "active_checksum",
            out_status->active_checksum, sizeof(out_status->active_checksum)) ||
        !take_field(&cursor, "active_image_size", &value, &len) ||
        !parse_decimal_u64(value, len, &out_status->active_image_size) ||
        !take_field(&cursor, "running_partition", &value, &len) ||
        !parse_u32(value, len, &out_status->running_partition) ||
        !take_field(&cursor, "next_partition", &value, &len) ||
        !parse_u32(value, len, &out_status->next_partition) ||
        !take_field(&cursor, "boot_intent", &value, &len)) {
        return H2_PAL_ERR_FORMAT;
    }
    if (len == 6u && strncmp(value, "loader", len) == 0) {
        out_status->boot_intent = H2_H2LOADER_HOST_BOOT_INTENT_LOADER;
    } else if (len == 4u && strncmp(value, "auto", len) == 0) {
        out_status->boot_intent = H2_H2LOADER_HOST_BOOT_INTENT_AUTO;
    } else {
        return H2_PAL_ERR_FORMAT;
    }
    if (!parse_metadata_field(&cursor, "stage", &out_status->stage) ||
        !parse_metadata_field(&cursor, "partition_1", &out_status->partition_1) ||
        !parse_metadata_field(&cursor, "partition_2", &out_status->partition_2) ||
        !take_field(&cursor, "last_result", &value, &len) ||
        !parse_i32(value, len, &out_status->last) ||
        !take_field(&cursor, "mfg_mode", &value, &len) ||
        !parse_u32(value, len, &out_status->mfg_mode) ||
        (out_status->mfg_mode != 1u && out_status->mfg_mode != 2u) ||
        !take_field(&cursor, "mfg_steps", &value, &len) ||
        len != H2_H2LOADER_HOST_MFG_STEP_TOTAL) {
        return H2_PAL_ERR_FORMAT;
    }
    for (size_t index = 0u; index < len; ++index) {
        if (value[index] < '0' || value[index] > '3') return H2_PAL_ERR_FORMAT;
        out_status->mfg_steps[index] = (uint8_t)(value[index] - '0');
        if (out_status->mfg_mode == 1u && out_status->mfg_steps[index] != 0u) {
            return H2_PAL_ERR_FORMAT;
        }
    }
    if (*cursor == '\r') ++cursor;
    if (*cursor == '\n') ++cursor;
    if (*cursor != '\0' ||
        out_status->active_role == H2_H2LOADER_HOST_ACTIVE_ROLE_UNKNOWN ||
        (out_status->running_partition != 1u &&
         out_status->running_partition != 2u) ||
        (out_status->next_partition != 1u &&
         out_status->next_partition != 2u) ||
        !h2_h2loader_host_is_safe_identity(out_status->board) ||
        !h2_h2loader_host_is_safe_identity(out_status->target) ||
        !h2_h2loader_host_is_safe_identity(out_status->chip) ||
        !h2_h2loader_host_is_safe_identity(out_status->active_version) ||
        !h2_h2loader_host_is_sha256(out_status->active_checksum) ||
        out_status->active_image_size == 0u ||
        !metadata_valid(&out_status->stage, 1) ||
        !metadata_valid(&out_status->partition_1, 0) ||
        !metadata_valid(&out_status->partition_2, 0)) {
        return H2_PAL_ERR_FORMAT;
    }

    return H2_PAL_OK;
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
    COPY("board", board); COPY("target", target); COPY("chip", chip);
    if (!take_field(&cursor, "capabilities", &value, &len) || len != 10u ||
        !is_fixed_lower_hex(value, len) || !parse_u32(value, len, &out_status->capabilities)) goto format_error;
    if (!take_field(&cursor, "command_availability", &value, &len) || len != 10u ||
        !is_fixed_lower_hex(value, len) || !parse_u32(value, len, &out_status->command_availability)) goto format_error;
#undef COPY
    if ((out_status->capabilities & ~H2_H2LOADER_HOST_CAPABILITIES_ALL) != 0u ||
        (out_status->command_availability &
         ~H2_H2LOADER_HOST_COMMAND_AVAILABILITY_ALL) != 0u ||
        parse_status_v2(cursor, out_status) != H2_PAL_OK) {
        goto format_error;
    }
    return H2_PAL_OK;
format_error:
    memset(out_status, 0, sizeof(*out_status));
    return H2_PAL_ERR_FORMAT;
}

h2_pal_result_t h2_h2loader_host_status_verify_asset(
    const h2_h2loader_host_status_t *status,
    const h2_h2loader_host_catalog_entry_t *asset) {
    const h2_h2loader_host_metadata_t *active;
    h2_h2loader_host_active_role_t expected_role;

    if (status == NULL || asset == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (strcmp(status->board, asset->board) != 0 ||
        strcmp(status->target, asset->target) != 0) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (asset->role == H2_H2LOADER_HOST_ASSET_ROLE_APP) {
        expected_role = H2_H2LOADER_HOST_ACTIVE_ROLE_APP;
    } else if (asset->role == H2_H2LOADER_HOST_ASSET_ROLE_LOADER) {
        expected_role = H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER;
    } else {
        return H2_PAL_ERR_INVALID_ARG;
    }
    active = status->running_partition == 1u ? &status->partition_1 :
        status->running_partition == 2u ? &status->partition_2 : NULL;
    if (active == NULL || !active->valid || status->stage.valid ||
        status->active_role != expected_role || active->role != expected_role ||
        strcmp(status->active_version, asset->version) != 0 ||
        strcmp(status->active_checksum, asset->image_sha256) != 0 ||
        strcmp(active->version, asset->version) != 0 ||
        strcmp(active->board, asset->board) != 0 ||
        strcmp(active->target, asset->target) != 0 ||
        strcmp(active->image_checksum, asset->image_sha256) != 0 ||
        strcmp(active->package_checksum, asset->sha256) != 0) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return H2_PAL_OK;
}
