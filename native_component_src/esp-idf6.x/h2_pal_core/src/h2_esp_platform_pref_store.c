#include "h2_esp_platform_pref_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define H2_ESP_PREF_NAMESPACE_MAX 64u
#define H2_ESP_PREF_KEY_MAX 96u
#define H2_ESP_PREF_VALUE_MAX (16u * 1024u)
#define H2_ESP_PREF_PATH_MAX 512u
#define H2_ESP_PREF_HEADER_SIZE 20u
#define H2_ESP_PREF_FORMAT_VERSION 1u

static const uint8_t s_pref_magic[4] = {'H', '2', 'P', 'F'};

static uint16_t read_le16(const uint8_t *data) {
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8u));
}

static uint32_t read_le32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
           ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

static void write_le16(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8u);
}

static void write_le32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8u);
    data[2] = (uint8_t)(value >> 16u);
    data[3] = (uint8_t)(value >> 24u);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size) {
    size_t index;
    for (index = 0u; index < size; ++index) {
        unsigned bit;
        crc ^= data[index];
        for (bit = 0u; bit < 8u; ++bit) {
            crc = (crc >> 1u) ^ ((crc & 1u) != 0u ? 0xedb88320u : 0u);
        }
    }
    return crc;
}

static uint32_t record_crc(const uint8_t *record, size_t size) {
    uint32_t crc = 0xffffffffu;
    crc = crc32_update(crc, record, 16u);
    if (size > H2_ESP_PREF_HEADER_SIZE) {
        crc = crc32_update(crc, record + H2_ESP_PREF_HEADER_SIZE,
                           size - H2_ESP_PREF_HEADER_SIZE);
    }
    return crc ^ 0xffffffffu;
}

static size_t bounded_strlen(const char *text, size_t limit) {
    size_t length = 0u;
    if (text == NULL) {
        return 0u;
    }
    while (length <= limit && text[length] != '\0') {
        ++length;
    }
    return length;
}

static int validate_name(const char *text, size_t limit) {
    size_t length = bounded_strlen(text, limit);
    return length > 0u && length <= limit ? H2_PAL_OK
                                          : H2_PAL_ERR_INVALID_ARG;
}

static int map_errno_value(int error_number) {
    switch (error_number) {
    case ENOENT:
        return H2_PAL_ERR_NOT_FOUND;
    case ENOMEM:
        return H2_PAL_ERR_NO_MEMORY;
    case ENOSPC:
        return H2_PAL_ERR_NO_SPACE;
    case EINVAL:
    case ENAMETOOLONG:
        return H2_PAL_ERR_INVALID_ARG;
    default:
        return H2_PAL_ERR_IO;
    }
}

static int encode_hex(const char *text, char *out, size_t out_size) {
    static const char digits[] = "0123456789abcdef";
    size_t length = strlen(text);
    size_t index;
    if (out_size <= length * 2u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (index = 0u; index < length; ++index) {
        uint8_t value = (uint8_t)text[index];
        out[index * 2u] = digits[value >> 4u];
        out[(index * 2u) + 1u] = digits[value & 0x0fu];
    }
    out[length * 2u] = '\0';
    return H2_PAL_OK;
}

static int decode_hex(const char *text, char *out, size_t out_size) {
    size_t length = strlen(text);
    size_t index;
    if ((length & 1u) != 0u || out_size <= length / 2u) {
        return H2_PAL_ERR_IO;
    }
    for (index = 0u; index < length; index += 2u) {
        unsigned high;
        unsigned low;
        char a = text[index];
        char b = text[index + 1u];
        if (a >= '0' && a <= '9') high = (unsigned)(a - '0');
        else if (a >= 'a' && a <= 'f') high = (unsigned)(a - 'a' + 10);
        else return H2_PAL_ERR_IO;
        if (b >= '0' && b <= '9') low = (unsigned)(b - '0');
        else if (b >= 'a' && b <= 'f') low = (unsigned)(b - 'a' + 10);
        else return H2_PAL_ERR_IO;
        out[index / 2u] = (char)((high << 4u) | low);
    }
    out[length / 2u] = '\0';
    return H2_PAL_OK;
}

static int is_encoded_name(const char *text, size_t decoded_limit) {
    size_t length = strlen(text);
    size_t index;
    if (length == 0u || (length & 1u) != 0u ||
        length > decoded_limit * 2u) {
        return 0;
    }
    for (index = 0u; index < length; ++index) {
        if (!((text[index] >= '0' && text[index] <= '9') ||
              (text[index] >= 'a' && text[index] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int is_encoded_temporary_name(const char *text, size_t decoded_limit) {
    size_t length = strlen(text);
    size_t stem_size;
    size_t index;
    if (length <= 4u || strcmp(text + length - 4u, ".new") != 0) {
        return 0;
    }
    stem_size = length - 4u;
    if ((stem_size & 1u) != 0u || stem_size > decoded_limit * 2u) {
        return 0;
    }
    for (index = 0u; index < stem_size; ++index) {
        if (!((text[index] >= '0' && text[index] <= '9') ||
              (text[index] >= 'a' && text[index] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int namespace_path(const h2_esp_pref_store_t *store,
                          const char *name_space, char *out, size_t out_size) {
    char encoded[(H2_ESP_PREF_NAMESPACE_MAX * 2u) + 1u];
    int length;
    int rc = validate_name(name_space, H2_ESP_PREF_NAMESPACE_MAX);
    if (store == NULL || store->base_path == NULL || out == NULL ||
        rc != H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = encode_hex(name_space, encoded, sizeof(encoded));
    if (rc != H2_PAL_OK) return rc;
    length = snprintf(out, out_size, "%s/%s", store->base_path, encoded);
    return length >= 0 && (size_t)length < out_size ? H2_PAL_OK
                                                    : H2_PAL_ERR_INVALID_ARG;
}

static int record_path(const h2_esp_pref_store_t *store,
                       const char *name_space, const char *key,
                       char *out, size_t out_size) {
    char directory[H2_ESP_PREF_PATH_MAX];
    char encoded[(H2_ESP_PREF_KEY_MAX * 2u) + 1u];
    int length;
    int rc = validate_name(key, H2_ESP_PREF_KEY_MAX);
    if (rc != H2_PAL_OK) return rc;
    rc = namespace_path(store, name_space, directory, sizeof(directory));
    if (rc != H2_PAL_OK) return rc;
    rc = encode_hex(key, encoded, sizeof(encoded));
    if (rc != H2_PAL_OK) return rc;
    length = snprintf(out, out_size, "%s/%s", directory, encoded);
    return length >= 0 && (size_t)length < out_size ? H2_PAL_OK
                                                    : H2_PAL_ERR_INVALID_ARG;
}

static int read_file(const char *path, uint8_t **out_data, size_t *out_size) {
    struct stat info;
    FILE *file;
    uint8_t *data;
    if (stat(path, &info) != 0) return map_errno_value(errno);
    if (info.st_size < 0 || (uint64_t)info.st_size > SIZE_MAX) return H2_PAL_ERR_IO;
    data = (uint8_t *)malloc(info.st_size == 0 ? 1u : (size_t)info.st_size);
    if (data == NULL) return H2_PAL_ERR_NO_MEMORY;
    file = fopen(path, "rb");
    if (file == NULL) { free(data); return map_errno_value(errno); }
    if ((size_t)info.st_size > 0u &&
        fread(data, 1u, (size_t)info.st_size, file) != (size_t)info.st_size) {
        int saved = errno;
        fclose(file);
        free(data);
        return map_errno_value(saved == 0 ? EIO : saved);
    }
    if (fclose(file) != 0) { free(data); return map_errno_value(errno); }
    *out_data = data;
    *out_size = (size_t)info.st_size;
    return H2_PAL_OK;
}

static int decode_record(const uint8_t *record, size_t record_size,
                         const char *name_space, const char *key,
                         h2_pal_pref_entry_type_t expected_type,
                         h2_pal_pref_entry_type_t *out_type,
                         const uint8_t **out_value, size_t *out_value_size) {
    size_t namespace_size;
    size_t key_size;
    size_t value_size;
    h2_pal_pref_entry_type_t type;
    if (record_size < H2_ESP_PREF_HEADER_SIZE ||
        memcmp(record, s_pref_magic, sizeof(s_pref_magic)) != 0 ||
        read_le16(record + 4u) != H2_ESP_PREF_FORMAT_VERSION ||
        record[7] != 0u) return H2_PAL_ERR_IO;
    type = (h2_pal_pref_entry_type_t)record[6];
    if (type < H2_PAL_PREF_ENTRY_BLOB || type > H2_PAL_PREF_ENTRY_BOOL)
        return H2_PAL_ERR_IO;
    namespace_size = read_le16(record + 8u);
    key_size = read_le16(record + 10u);
    value_size = read_le32(record + 12u);
    if (namespace_size != strlen(name_space) || key_size != strlen(key) ||
        value_size > H2_ESP_PREF_VALUE_MAX ||
        H2_ESP_PREF_HEADER_SIZE + namespace_size + key_size + value_size != record_size ||
        memcmp(record + H2_ESP_PREF_HEADER_SIZE, name_space, namespace_size) != 0 ||
        memcmp(record + H2_ESP_PREF_HEADER_SIZE + namespace_size, key, key_size) != 0 ||
        read_le32(record + 16u) != record_crc(record, record_size))
        return H2_PAL_ERR_IO;
    if (expected_type != H2_PAL_PREF_ENTRY_UNKNOWN && type != expected_type)
        return H2_PAL_ERR_INVALID_STATE;
    *out_type = type;
    *out_value = record + H2_ESP_PREF_HEADER_SIZE + namespace_size + key_size;
    *out_value_size = value_size;
    return H2_PAL_OK;
}

static int committed_size(const h2_esp_pref_store_t *store, size_t *out_size) {
    DIR *root = opendir(store->base_path);
    struct dirent *namespace_entry;
    size_t total = 0u;
    if (root == NULL) return map_errno_value(errno);
    while ((namespace_entry = readdir(root)) != NULL) {
        char directory[H2_ESP_PREF_PATH_MAX];
        char name_space[H2_ESP_PREF_NAMESPACE_MAX + 1u];
        DIR *namespace_dir;
        struct dirent *entry;
        if (namespace_entry->d_name[0] == '.') continue;
        if (!is_encoded_name(namespace_entry->d_name,
                             H2_ESP_PREF_NAMESPACE_MAX) ||
            decode_hex(namespace_entry->d_name, name_space,
                       sizeof(name_space)) != H2_PAL_OK) {
            closedir(root);
            return H2_PAL_ERR_IO;
        }
        if (snprintf(directory, sizeof(directory), "%s/%s", store->base_path,
                     namespace_entry->d_name) >= (int)sizeof(directory)) {
            closedir(root); return H2_PAL_ERR_IO;
        }
        namespace_dir = opendir(directory);
        if (namespace_dir == NULL) {
            closedir(root);
            return H2_PAL_ERR_IO;
        }
        while ((entry = readdir(namespace_dir)) != NULL) {
            char path[H2_ESP_PREF_PATH_MAX];
            char key[H2_ESP_PREF_KEY_MAX + 1u];
            uint8_t *record = NULL;
            size_t record_size = 0u;
            const uint8_t *value;
            size_t value_size;
            h2_pal_pref_entry_type_t type;
            int rc;
            size_t length = strlen(entry->d_name);
            if (entry->d_name[0] == '.' ||
                (length > 4u && strcmp(entry->d_name + length - 4u, ".new") == 0))
                continue;
            if (!is_encoded_name(entry->d_name, H2_ESP_PREF_KEY_MAX) ||
                decode_hex(entry->d_name, key, sizeof(key)) != H2_PAL_OK) {
                closedir(namespace_dir);
                closedir(root);
                return H2_PAL_ERR_IO;
            }
            if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >=
                (int)sizeof(path)) { closedir(namespace_dir); closedir(root); return H2_PAL_ERR_IO; }
            rc = read_file(path, &record, &record_size);
            if (rc == H2_PAL_OK) {
                rc = decode_record(record, record_size, name_space, key,
                                   H2_PAL_PREF_ENTRY_UNKNOWN, &type, &value,
                                   &value_size);
            }
            free(record);
            if (rc != H2_PAL_OK) {
                closedir(namespace_dir);
                closedir(root);
                return rc;
            }
            if (record_size > SIZE_MAX - total) {
                closedir(namespace_dir);
                closedir(root);
                return H2_PAL_ERR_NO_SPACE;
            }
            total += record_size;
        }
        closedir(namespace_dir);
    }
    closedir(root);
    *out_size = total;
    return H2_PAL_OK;
}

static int write_atomic_file(h2_esp_pref_store_t *store, const char *path,
                             const void *data, size_t size) {
    char temporary[H2_ESP_PREF_PATH_MAX];
    unsigned attempt;
    if (snprintf(temporary, sizeof(temporary), "%s.new", path) >=
        (int)sizeof(temporary)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (attempt = 0u; attempt < 2u; ++attempt) {
        FILE *file = fopen(temporary, "wb");
        int rc = H2_PAL_OK;
        if (file == NULL) {
            rc = map_errno_value(errno);
        } else if (store->test_fault_once == H2_ESP_PREF_STORE_FAULT_WRITE ||
                   store->test_fault_once == H2_ESP_PREF_STORE_FAULT_NO_SPACE) {
            rc = store->test_fault_once == H2_ESP_PREF_STORE_FAULT_NO_SPACE
                     ? H2_PAL_ERR_NO_SPACE
                     : H2_PAL_ERR_IO;
            store->test_fault_once = H2_ESP_PREF_STORE_FAULT_NONE;
            store->test_fault_hits++;
            (void)fclose(file);
        } else if (fwrite(data, 1u, size, file) != size ||
                   store->test_fault_once == H2_ESP_PREF_STORE_FAULT_SYNC ||
                   fflush(file) != 0 || fsync(fileno(file)) != 0) {
            int saved = errno;
            if (store->test_fault_once == H2_ESP_PREF_STORE_FAULT_SYNC) {
                store->test_fault_once = H2_ESP_PREF_STORE_FAULT_NONE;
                store->test_fault_hits++;
                saved = EIO;
            }
            (void)fclose(file);
            rc = map_errno_value(saved == 0 ? EIO : saved);
        } else if (store->test_fault_once == H2_ESP_PREF_STORE_FAULT_CLOSE) {
            store->test_fault_once = H2_ESP_PREF_STORE_FAULT_NONE;
            store->test_fault_hits++;
            (void)fclose(file);
            rc = H2_PAL_ERR_IO;
        } else if (fclose(file) != 0) {
            rc = map_errno_value(errno == 0 ? EIO : errno);
        } else if (store->test_fault_once == H2_ESP_PREF_STORE_FAULT_RENAME) {
            store->test_fault_once = H2_ESP_PREF_STORE_FAULT_NONE;
            store->test_fault_hits++;
            rc = H2_PAL_ERR_IO;
        } else if (rename(temporary, path) != 0) {
            rc = map_errno_value(errno);
        } else {
            return H2_PAL_OK;
        }
        (void)unlink(temporary);
        if (rc != H2_PAL_ERR_NO_SPACE || attempt != 0u) return rc;
        rc = h2_esp_pref_store_prepare(store);
        if (rc != H2_PAL_OK) return rc;
    }
    return H2_PAL_ERR_NO_SPACE;
}

int h2_esp_pref_store_prepare(h2_esp_pref_store_t *store) {
    char migration_temporary[H2_ESP_PREF_PATH_MAX];
    DIR *root;
    struct dirent *namespace_entry;
    if (store == NULL || store->base_path == NULL || store->committed_budget == 0u)
        return H2_PAL_ERR_INVALID_ARG;
    if (mkdir(store->base_path, 0700) != 0 && errno != EEXIST)
        return map_errno_value(errno);
    if (snprintf(migration_temporary, sizeof(migration_temporary),
                 "%s/.migration.new", store->base_path) <
        (int)sizeof(migration_temporary)) {
        (void)unlink(migration_temporary);
    }
    root = opendir(store->base_path);
    if (root == NULL) return map_errno_value(errno);
    while ((namespace_entry = readdir(root)) != NULL) {
        char directory[H2_ESP_PREF_PATH_MAX];
        DIR *namespace_dir;
        struct dirent *entry;
        if (namespace_entry->d_name[0] == '.') continue;
        if (!is_encoded_name(namespace_entry->d_name,
                             H2_ESP_PREF_NAMESPACE_MAX)) {
            continue;
        }
        if (snprintf(directory, sizeof(directory), "%s/%s", store->base_path,
                     namespace_entry->d_name) >= (int)sizeof(directory)) continue;
        namespace_dir = opendir(directory);
        if (namespace_dir == NULL) continue;
        while ((entry = readdir(namespace_dir)) != NULL) {
            char path[H2_ESP_PREF_PATH_MAX];
            if (!is_encoded_temporary_name(entry->d_name,
                                           H2_ESP_PREF_KEY_MAX)) {
                continue;
            }
            if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) < (int)sizeof(path))
                (void)unlink(path);
        }
        closedir(namespace_dir);
    }
    closedir(root);
    return H2_PAL_OK;
}

int h2_esp_pref_store_get(h2_esp_pref_store_t *store, const char *name_space,
                          const char *key, h2_pal_pref_entry_type_t expected_type,
                          uint8_t **out_value, size_t *out_value_size) {
    char path[H2_ESP_PREF_PATH_MAX];
    uint8_t *record = NULL;
    size_t record_size = 0u;
    const uint8_t *value;
    size_t value_size;
    h2_pal_pref_entry_type_t type;
    uint8_t *copy;
    int rc;
    if (out_value == NULL || out_value_size == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_value = NULL; *out_value_size = 0u;
    rc = record_path(store, name_space, key, path, sizeof(path));
    if (rc != H2_PAL_OK) return rc;
    rc = read_file(path, &record, &record_size);
    if (rc != H2_PAL_OK) return rc;
    rc = decode_record(record, record_size, name_space, key, expected_type,
                       &type, &value, &value_size);
    if (rc != H2_PAL_OK) { free(record); return rc; }
    copy = (uint8_t *)malloc(value_size == 0u ? 1u : value_size);
    if (copy == NULL) { free(record); return H2_PAL_ERR_NO_MEMORY; }
    if (value_size > 0u) memcpy(copy, value, value_size);
    free(record);
    *out_value = copy; *out_value_size = value_size;
    return H2_PAL_OK;
}

int h2_esp_pref_store_set(h2_esp_pref_store_t *store, const char *name_space,
                          const char *key, h2_pal_pref_entry_type_t type,
                          const void *value, size_t value_size) {
    char directory[H2_ESP_PREF_PATH_MAX];
    char path[H2_ESP_PREF_PATH_MAX];
    uint8_t *old_record = NULL;
    size_t old_size = 0u;
    uint8_t *record;
    size_t namespace_size;
    size_t key_size;
    size_t record_size;
    size_t total;
    int had_old = 0;
    int rc;
    if (type < H2_PAL_PREF_ENTRY_BLOB || type > H2_PAL_PREF_ENTRY_BOOL ||
        (value == NULL && value_size != 0u) || value_size > H2_ESP_PREF_VALUE_MAX)
        return H2_PAL_ERR_INVALID_ARG;
    rc = record_path(store, name_space, key, path, sizeof(path));
    if (rc != H2_PAL_OK) return rc;
    rc = namespace_path(store, name_space, directory, sizeof(directory));
    if (rc != H2_PAL_OK) return rc;
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) return map_errno_value(errno);
    namespace_size = strlen(name_space); key_size = strlen(key);
    record_size = H2_ESP_PREF_HEADER_SIZE + namespace_size + key_size + value_size;
    record = (uint8_t *)calloc(1u, record_size);
    if (record == NULL) return H2_PAL_ERR_NO_MEMORY;
    memcpy(record, s_pref_magic, sizeof(s_pref_magic));
    write_le16(record + 4u, H2_ESP_PREF_FORMAT_VERSION);
    record[6] = (uint8_t)type;
    write_le16(record + 8u, (uint16_t)namespace_size);
    write_le16(record + 10u, (uint16_t)key_size);
    write_le32(record + 12u, (uint32_t)value_size);
    memcpy(record + H2_ESP_PREF_HEADER_SIZE, name_space, namespace_size);
    memcpy(record + H2_ESP_PREF_HEADER_SIZE + namespace_size, key, key_size);
    if (value_size > 0u) memcpy(record + H2_ESP_PREF_HEADER_SIZE + namespace_size + key_size, value, value_size);
    write_le32(record + 16u, record_crc(record, record_size));
    rc = read_file(path, &old_record, &old_size);
    if (rc == H2_PAL_OK) {
        const uint8_t *old_value;
        size_t old_value_size;
        h2_pal_pref_entry_type_t old_type;
        had_old = 1;
        rc = decode_record(old_record, old_size, name_space, key,
                           H2_PAL_PREF_ENTRY_UNKNOWN, &old_type, &old_value,
                           &old_value_size);
        if (rc != H2_PAL_OK) {
            free(old_record);
            free(record);
            return rc;
        }
    }
    if (had_old && old_size == record_size && memcmp(old_record, record, record_size) == 0) {
        free(old_record); free(record); return H2_PAL_OK;
    }
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND) { free(record); return rc; }
    free(old_record);
    rc = committed_size(store, &total);
    if (rc != H2_PAL_OK) { free(record); return rc; }
    if ((had_old && old_size > total) ||
        total - (had_old ? old_size : 0u) > store->committed_budget ||
        record_size > store->committed_budget - (total - (had_old ? old_size : 0u))) {
        free(record); return H2_PAL_ERR_NO_SPACE;
    }
    rc = write_atomic_file(store, path, record, record_size);
    free(record);
    return rc;
}

int h2_esp_pref_store_remove(h2_esp_pref_store_t *store, const char *name_space,
                             const char *key) {
    char path[H2_ESP_PREF_PATH_MAX];
    uint8_t *record = NULL;
    size_t record_size = 0u;
    const uint8_t *value;
    size_t value_size;
    h2_pal_pref_entry_type_t type;
    int rc = record_path(store, name_space, key, path, sizeof(path));
    if (rc != H2_PAL_OK) return rc;
    rc = read_file(path, &record, &record_size);
    if (rc == H2_PAL_OK) {
        rc = decode_record(record, record_size, name_space, key,
                           H2_PAL_PREF_ENTRY_UNKNOWN, &type, &value,
                           &value_size);
    }
    free(record);
    if (rc != H2_PAL_OK) return rc;
    return unlink(path) == 0 ? H2_PAL_OK : map_errno_value(errno);
}

int h2_esp_pref_store_clear(h2_esp_pref_store_t *store, const char *name_space) {
    char directory[H2_ESP_PREF_PATH_MAX];
    h2_esp_pref_store_entry_t *entries = NULL;
    size_t count = 0u;
    size_t index;
    int rc = namespace_path(store, name_space, directory, sizeof(directory));
    if (rc != H2_PAL_OK) return rc;
    rc = h2_esp_pref_store_list(store, name_space, &entries, &count);
    if (rc != H2_PAL_OK) return rc;
    for (index = 0u; index < count; ++index) {
        char path[H2_ESP_PREF_PATH_MAX];
        rc = record_path(store, name_space, entries[index].key, path,
                         sizeof(path));
        if (rc != H2_PAL_OK || unlink(path) != 0) {
            if (rc == H2_PAL_OK) rc = map_errno_value(errno);
            free(entries);
            return rc;
        }
    }
    free(entries);
    (void)rmdir(directory);
    return H2_PAL_OK;
}

int h2_esp_pref_store_list(h2_esp_pref_store_t *store, const char *name_space,
                           h2_esp_pref_store_entry_t **out_entries,
                           size_t *out_count) {
    char directory[H2_ESP_PREF_PATH_MAX];
    DIR *dir;
    struct dirent *raw;
    h2_esp_pref_store_entry_t *entries = NULL;
    size_t count = 0u;
    int rc = H2_PAL_OK;
    if (out_entries == NULL || out_count == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_entries = NULL; *out_count = 0u;
    rc = namespace_path(store, name_space, directory, sizeof(directory));
    if (rc != H2_PAL_OK) return rc;
    dir = opendir(directory);
    if (dir == NULL) return errno == ENOENT ? H2_PAL_OK : map_errno_value(errno);
    while ((raw = readdir(dir)) != NULL) {
        h2_esp_pref_store_entry_t entry;
        uint8_t *record = NULL;
        size_t record_size = 0u;
        const uint8_t *value;
        h2_pal_pref_entry_type_t type;
        char path[H2_ESP_PREF_PATH_MAX];
        size_t value_size;
        h2_esp_pref_store_entry_t *grown;
        size_t length = strlen(raw->d_name);
        if (raw->d_name[0] == '.' || (length > 4u && strcmp(raw->d_name + length - 4u, ".new") == 0)) continue;
        rc = decode_hex(raw->d_name, entry.key, sizeof(entry.key));
        if (rc != H2_PAL_OK) break;
        if (snprintf(path, sizeof(path), "%s/%s", directory, raw->d_name) >= (int)sizeof(path)) { rc = H2_PAL_ERR_IO; break; }
        rc = read_file(path, &record, &record_size);
        if (rc == H2_PAL_OK) rc = decode_record(record, record_size, name_space, entry.key,
                                                H2_PAL_PREF_ENTRY_UNKNOWN, &type, &value, &value_size);
        free(record);
        if (rc != H2_PAL_OK) break;
        entry.type = type; entry.value_size = value_size;
        grown = (h2_esp_pref_store_entry_t *)realloc(entries, (count + 1u) * sizeof(*entries));
        if (grown == NULL) { rc = H2_PAL_ERR_NO_MEMORY; break; }
        entries = grown; entries[count++] = entry;
    }
    closedir(dir);
    if (rc != H2_PAL_OK) { free(entries); return rc; }
    *out_entries = entries; *out_count = count;
    return H2_PAL_OK;
}

static int marker_path(const h2_esp_pref_store_t *store, const char *marker,
                       char *out, size_t out_size) {
    int length;
    if (store == NULL || validate_name(marker, 32u) != H2_PAL_OK) return H2_PAL_ERR_INVALID_ARG;
    length = snprintf(out, out_size, "%s/.%s", store->base_path, marker);
    return length >= 0 && (size_t)length < out_size ? H2_PAL_OK : H2_PAL_ERR_INVALID_ARG;
}

int h2_esp_pref_store_write_marker(h2_esp_pref_store_t *store, const char *marker,
                                   const char *value) {
    char path[H2_ESP_PREF_PATH_MAX];
    size_t length;
    int rc = marker_path(store, marker, path, sizeof(path));
    if (rc != H2_PAL_OK || value == NULL) return H2_PAL_ERR_INVALID_ARG;
    length = strlen(value);
    return write_atomic_file(store, path, value, length);
}

int h2_esp_pref_store_read_marker(h2_esp_pref_store_t *store, const char *marker,
                                  char *out_value, size_t out_value_size) {
    char path[H2_ESP_PREF_PATH_MAX];
    uint8_t *data = NULL;
    size_t size = 0u;
    int rc;
    if (out_value == NULL || out_value_size == 0u) return H2_PAL_ERR_INVALID_ARG;
    out_value[0] = '\0';
    rc = marker_path(store, marker, path, sizeof(path));
    if (rc != H2_PAL_OK) return rc;
    rc = read_file(path, &data, &size);
    if (rc != H2_PAL_OK) return rc;
    if (size >= out_value_size) { free(data); return H2_PAL_ERR_IO; }
    memcpy(out_value, data, size); out_value[size] = '\0'; free(data);
    return H2_PAL_OK;
}
