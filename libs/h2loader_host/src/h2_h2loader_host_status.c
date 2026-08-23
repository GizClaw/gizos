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

static int field_matches(
    const char *field,
    size_t field_len,
    const char *name,
    const char **out_value,
    size_t *out_value_len) {
    size_t name_len = strlen(name);

    if (field_len <= name_len || field[name_len] != '=' ||
        memcmp(field, name, name_len) != 0) {
        return 0;
    }
    *out_value = field + name_len + 1u;
    *out_value_len = field_len - name_len - 1u;
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

static int parse_boolean(const char *value, size_t len, uint8_t *out_value) {
    if (len != 1u || (value[0] != '0' && value[0] != '1')) {
        return 0;
    }
    *out_value = (uint8_t)(value[0] == '1');
    return 1;
}

static h2_pal_result_t parse_field(
    h2_h2loader_host_status_t *status,
    const char *field,
    size_t field_len) {
    const char *value = NULL;
    size_t value_len = 0u;

#define H2_COPY_STATUS_FIELD(name_literal, member) \
    if (field_matches(field, field_len, name_literal, &value, &value_len)) { \
        if (value_len == 0u) { \
            status->member[0] = '\0'; \
            return H2_PAL_OK; \
        } \
        return h2_h2loader_host_copy_text( \
                   status->member, sizeof(status->member), value, value_len) \
            ? H2_PAL_OK \
            : H2_PAL_ERR_FORMAT; \
    }
    H2_COPY_STATUS_FIELD("board", board)
    H2_COPY_STATUS_FIELD("target", target)
    H2_COPY_STATUS_FIELD("chip", chip)
    H2_COPY_STATUS_FIELD("active_name", active_name)
    H2_COPY_STATUS_FIELD("active_version", active_version)
    H2_COPY_STATUS_FIELD("active_checksum", active_checksum)
    H2_COPY_STATUS_FIELD("installed_checksum", installed_checksum)
    H2_COPY_STATUS_FIELD("staged_checksum", staged_checksum)
    H2_COPY_STATUS_FIELD("upgrade_package_sha256", upgrade_package_sha256)
    H2_COPY_STATUS_FIELD("state", state)
    H2_COPY_STATUS_FIELD("upgrade_phase", upgrade_phase)
#undef H2_COPY_STATUS_FIELD

    if (field_matches(
            field, field_len, "active_role", &value, &value_len)) {
        if (value_len == 8u && memcmp(value, "h2loader", 8u) == 0) {
            status->active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER;
        } else if (value_len == 3u && memcmp(value, "app", 3u) == 0) {
            status->active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_APP;
        } else {
            return H2_PAL_ERR_FORMAT;
        }
        return H2_PAL_OK;
    }
    if (field_matches(
            field, field_len, "capabilities", &value, &value_len)) {
        if (!parse_u32(value, value_len, &status->capabilities)) {
            return H2_PAL_ERR_FORMAT;
        }
        status->has_capabilities = 1u;
        return H2_PAL_OK;
    }
    if (field_matches(
            field, field_len, "running_partition", &value, &value_len)) {
        if (!parse_u32(value, value_len, &status->running_partition)) {
            return H2_PAL_ERR_FORMAT;
        }
        status->has_running_partition = 1u;
        return H2_PAL_OK;
    }
    if (field_matches(field, field_len, "staged_bytes", &value, &value_len)) {
        return parse_u64(value, value_len, &status->staged_bytes)
            ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
    }
    if (field_matches(field, field_len, "upgrade_last", &value, &value_len)) {
        char number[32];
        char *end = NULL;
        long parsed;
        if (value_len == 0u || value_len >= sizeof(number)) {
            return H2_PAL_ERR_FORMAT;
        }
        memcpy(number, value, value_len);
        number[value_len] = '\0';
        errno = 0;
        parsed = strtol(number, &end, 10);
        if (errno != 0 || end == number || *end != '\0' ||
            parsed < INT32_MIN || parsed > INT32_MAX) {
            return H2_PAL_ERR_FORMAT;
        }
        status->upgrade_last = (int32_t)parsed;
        status->has_upgrade_last = 1u;
        return H2_PAL_OK;
    }
#define H2_PARSE_BOOLEAN_FIELD(name_literal, member) \
    if (field_matches(field, field_len, name_literal, &value, &value_len)) { \
        return parse_boolean(value, value_len, &status->member) \
            ? H2_PAL_OK \
            : H2_PAL_ERR_FORMAT; \
    }
    H2_PARSE_BOOLEAN_FIELD("installed_valid", installed_valid)
    H2_PARSE_BOOLEAN_FIELD("staged_valid", staged_valid)
    H2_PARSE_BOOLEAN_FIELD("app_confirmed", app_confirmed)
#undef H2_PARSE_BOOLEAN_FIELD
    return H2_PAL_OK;
}

h2_pal_result_t h2_h2loader_host_status_parse(
    const char *line,
    h2_h2loader_host_status_t *out_status) {
    static const char loader_marker[] = "H2_LOADER_STATUS ";
    static const char app_marker[] = "H2_APP_STATUS ";
    const char *fields;
    int app_line = 0;

    if (out_status != NULL) {
        memset(out_status, 0, sizeof(*out_status));
    }
    if (line == NULL || out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    fields = strstr(line, loader_marker);
    if (fields != NULL) {
        fields += sizeof(loader_marker) - 1u;
    } else {
        fields = strstr(line, app_marker);
        if (fields == NULL) {
            return H2_PAL_ERR_FORMAT;
        }
        fields += sizeof(app_marker) - 1u;
        app_line = 1;
    }
    while (*fields != '\0' && *fields != '\r' && *fields != '\n') {
        const char *end;
        h2_pal_result_t rc;

        while (*fields == ' ') {
            ++fields;
        }
        if (*fields == '\0' || *fields == '\r' || *fields == '\n') {
            break;
        }
        end = fields;
        while (*end != '\0' && *end != '\r' && *end != '\n' &&
               *end != ' ') {
            ++end;
        }
        rc = parse_field(out_status, fields, (size_t)(end - fields));
        if (rc != H2_PAL_OK) {
            memset(out_status, 0, sizeof(*out_status));
            return rc;
        }
        if (app_line) {
            const char *value = NULL;
            size_t value_len = 0u;
            if (field_matches(
                    fields,
                    (size_t)(end - fields),
                    "app",
                    &value,
                    &value_len) &&
                !h2_h2loader_host_copy_text(
                    out_status->active_name,
                    sizeof(out_status->active_name),
                    value,
                    value_len)) {
                memset(out_status, 0, sizeof(*out_status));
                return H2_PAL_ERR_FORMAT;
            }
        }
        fields = end;
    }
    if (app_line) {
        out_status->active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_APP;
    }
    if (!h2_h2loader_host_is_safe_identity(out_status->board) ||
        !h2_h2loader_host_is_safe_identity(out_status->target) ||
        out_status->active_role == H2_H2LOADER_HOST_ACTIVE_ROLE_UNKNOWN) {
        memset(out_status, 0, sizeof(*out_status));
        return H2_PAL_ERR_FORMAT;
    }
    if ((out_status->active_checksum[0] != '\0' &&
         !h2_h2loader_host_is_sha256(out_status->active_checksum)) ||
        (out_status->installed_checksum[0] != '\0' &&
         !h2_h2loader_host_is_sha256(out_status->installed_checksum)) ||
        (out_status->staged_checksum[0] != '\0' &&
         !h2_h2loader_host_is_sha256(out_status->staged_checksum)) ||
        (out_status->upgrade_package_sha256[0] != '\0' &&
         !h2_h2loader_host_is_sha256(
             out_status->upgrade_package_sha256))) {
        memset(out_status, 0, sizeof(*out_status));
        return H2_PAL_ERR_FORMAT;
    }
    return H2_PAL_OK;
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
        if (status->active_role != H2_H2LOADER_HOST_ACTIVE_ROLE_APP ||
            status->staged_valid != 0u ||
            (!package_manifest &&
             strcmp(status->active_name, asset->image) != 0) ||
            (package_manifest &&
             strcmp(status->active_version, asset->version) != 0) ||
            strcmp(status->state, "confirmed") != 0 ||
            status->installed_valid == 0u ||
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
        if (status->active_role != H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER ||
            status->staged_valid != 0u ||
            strcmp(status->active_version, asset->version) != 0 ||
            (package_manifest &&
             (!h2_h2loader_host_is_sha256(status->active_checksum) ||
              strcmp(status->active_checksum, asset->image_sha256) != 0)) ||
            strcmp(status->upgrade_phase, "idle") != 0) {
            return H2_PAL_ERR_INVALID_STATE;
        }
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_INVALID_ARG;
}
