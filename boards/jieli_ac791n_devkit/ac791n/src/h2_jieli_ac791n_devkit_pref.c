#include "asm/sfc_norflash_api.h"
#include "device/ioctl_cmds.h"
#include "lfs.h"
#include "os/os_api.h"

#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_ac791n_devkit_partitions.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern int snprintf(char *buffer, size_t size, const char *format, ...);

enum {
  H2_PREF_NAMESPACE_MAX = 64,
  H2_PREF_KEY_MAX = 96,
  H2_PREF_VALUE_MAX = 16 * 1024,
  /* Leading slash, hex namespace, slash, hex key, ".tmp" and terminator. */
  H2_PREF_PATH_MAX =
      2u * (H2_PREF_NAMESPACE_MAX + H2_PREF_KEY_MAX) + 2u + sizeof(".tmp"),
  H2_PREF_MAGIC = 0x31504648,
};

typedef struct jieli_pref_namespace {
  h2_pal_pref_namespace_t pal;
  char path[1u + H2_PREF_NAMESPACE_MAX * 2u + 1u];
  h2_pal_pref_open_mode_t mode;
} jieli_pref_namespace_t;

typedef struct jieli_pref_header {
  uint32_t magic;
  uint32_t value_size;
  uint16_t key_size;
  uint8_t type;
  uint8_t reserved;
} jieli_pref_header_t;

struct h2_pal_pref_cursor {
  lfs_dir_t directory;
  int open;
  char key[H2_PREF_KEY_MAX + 1u];
};

static lfs_t pref_lfs;
static OS_MUTEX pref_mutex;
/* Atomic publication states: 0 uninitialized, 1 creating, 2 ready. */
static int pref_mutex_ready;
static int pref_ready;
static void (*pref_diagnostic)(const char *line);

static void pref_trace(const char *line) {
  if (pref_diagnostic != NULL) pref_diagnostic(line);
}

static void pref_trace_key(const char *event, const char *key, int result) {
  if (pref_diagnostic == NULL) return;
  char line[192];
  (void)snprintf(
      line, sizeof(line), "H2_JIELI_PREF_%s key=%s code=%d\r\n",
      event, key == NULL ? "(null)" : key, result);
  pref_diagnostic(line);
}

static int pref_flash_read(
    const struct lfs_config *config, lfs_block_t block, lfs_off_t offset,
    void *buffer, lfs_size_t size) {
  uint32_t address = (uint32_t)block * config->block_size + offset;
  if (address > H2_JIELI_PREF_SIZE || size > H2_JIELI_PREF_SIZE - address) {
    return LFS_ERR_IO;
  }
  return norflash_read(
             NULL, buffer, size, H2_JIELI_PREF_ADDRESS + address) ==
          (int)size
      ? LFS_ERR_OK
      : LFS_ERR_IO;
}

static int pref_flash_program(
    const struct lfs_config *config, lfs_block_t block, lfs_off_t offset,
    const void *buffer, lfs_size_t size) {
  uint32_t address = (uint32_t)block * config->block_size + offset;
  if (address > H2_JIELI_PREF_SIZE || size > H2_JIELI_PREF_SIZE - address) {
    return LFS_ERR_IO;
  }
  (void)norflash_ioctl(NULL, IOCTL_SET_WRITE_PROTECT, 0u);
  return norflash_write(
             NULL, (void *)buffer, size, H2_JIELI_PREF_ADDRESS + address) ==
          (int)size
      ? LFS_ERR_OK
      : LFS_ERR_IO;
}

static int pref_flash_erase(
    const struct lfs_config *config, lfs_block_t block) {
  uint32_t address = (uint32_t)block * config->block_size;
  if (address > H2_JIELI_PREF_SIZE ||
      config->block_size > H2_JIELI_PREF_SIZE - address) {
    return LFS_ERR_IO;
  }
  (void)norflash_ioctl(NULL, IOCTL_SET_WRITE_PROTECT, 0u);
  return norflash_ioctl(
             NULL, IOCTL_ERASE_SECTOR, H2_JIELI_PREF_ADDRESS + address) == 0
      ? LFS_ERR_OK
      : LFS_ERR_IO;
}

static int pref_flash_sync(const struct lfs_config *config) {
  (void)config;
  return LFS_ERR_OK;
}

static const struct lfs_config pref_config = {
    .read = pref_flash_read,
    .prog = pref_flash_program,
    .erase = pref_flash_erase,
    .sync = pref_flash_sync,
    .read_size = 256u,
    .prog_size = 256u,
    .block_size = H2_JIELI_FLASH_SECTOR_SIZE,
    .block_count = H2_JIELI_PREF_SIZE / H2_JIELI_FLASH_SECTOR_SIZE,
    .block_cycles = 500,
    .cache_size = 256u,
    .lookahead_size = 16u,
};

static int map_lfs_error(int result) {
  if (result >= 0) return H2_PAL_OK;
  if (result == LFS_ERR_NOENT) return H2_PAL_ERR_NOT_FOUND;
  if (result == LFS_ERR_NOMEM) return H2_PAL_ERR_NO_MEMORY;
  if (result == LFS_ERR_NOSPC) return H2_PAL_ERR_NO_SPACE;
  if (result == LFS_ERR_INVAL || result == LFS_ERR_NAMETOOLONG) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return H2_PAL_ERR_IO;
}

static int pref_lock(void) {
  while (__atomic_load_n(&pref_mutex_ready, __ATOMIC_ACQUIRE) != 2) {
    int expected = 0;
    if (__atomic_compare_exchange_n(&pref_mutex_ready, &expected, 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
      pref_trace("H2_JIELI_PREF_ENTER step=mutex_create\r\n");
      int result = os_mutex_create(&pref_mutex);
      __atomic_store_n(&pref_mutex_ready, result == OS_NO_ERR ? 2 : 0,
                       __ATOMIC_RELEASE);
      if (result != OS_NO_ERR) return H2_PAL_ERR_IO;
      pref_trace("H2_JIELI_PREF_OK step=mutex_create\r\n");
    } else {
      os_time_dly(1u);
    }
  }
  pref_trace("H2_JIELI_PREF_ENTER step=mutex_pend\r\n");
  return os_mutex_pend(&pref_mutex, 0u) == OS_NO_ERR ? H2_PAL_OK
                                                      : H2_PAL_ERR_IO;
}

static void pref_unlock(void) {
  (void)os_mutex_post(&pref_mutex);
}

static int pref_region_erased(void) {
  uint8_t buffer[256];
  for (uint32_t offset = 0u; offset < H2_JIELI_PREF_SIZE;
       offset += sizeof(buffer)) {
    if (norflash_read(
            NULL, buffer, sizeof(buffer), H2_JIELI_PREF_ADDRESS + offset) !=
        (int)sizeof(buffer)) {
      return 0;
    }
    for (size_t index = 0u; index < sizeof(buffer); ++index) {
      if (buffer[index] != 0xffu) return 0;
    }
  }
  return 1;
}

static int pref_prepare_locked(void) {
  if (pref_ready) return H2_PAL_OK;
  int result;
  pref_trace("H2_JIELI_PREF_ENTER step=lfs_mount\r\n");
  result = lfs_mount(&pref_lfs, &pref_config);
  pref_trace("H2_JIELI_PREF_OK step=lfs_mount_returned\r\n");
  if (result != LFS_ERR_OK) {
    pref_trace("H2_JIELI_PREF_ENTER step=erased_scan\r\n");
    if (!pref_region_erased()) return H2_PAL_ERR_IO;
    pref_trace("H2_JIELI_PREF_ENTER step=lfs_format\r\n");
    result = lfs_format(&pref_lfs, &pref_config);
    pref_trace("H2_JIELI_PREF_OK step=lfs_format_returned\r\n");
    if (result == LFS_ERR_OK) result = lfs_mount(&pref_lfs, &pref_config);
  }
  if (result != LFS_ERR_OK) return map_lfs_error(result);
  pref_ready = 1;
  pref_trace("H2_JIELI_PREF_OK step=prepare\r\n");
  return H2_PAL_OK;
}

static size_t bounded_length(const char *text, size_t maximum) {
  size_t length = 0u;
  if (text == NULL) return 0u;
  while (length <= maximum && text[length] != '\0') ++length;
  return length;
}

static int valid_name(const char *text, size_t maximum) {
  size_t length = bounded_length(text, maximum);
  return length != 0u && length <= maximum;
}

static void hex_encode(const char *input, size_t length, char *output) {
  static const char digits[] = "0123456789abcdef";
  for (size_t index = 0u; index < length; ++index) {
    uint8_t byte = (uint8_t)input[index];
    output[index * 2u] = digits[byte >> 4u];
    output[index * 2u + 1u] = digits[byte & 0x0fu];
  }
  output[length * 2u] = '\0';
}

static int record_path(
    const jieli_pref_namespace_t *name_space, const char *key,
    char path[H2_PREF_PATH_MAX]) {
  size_t key_length = bounded_length(key, H2_PREF_KEY_MAX);
  if (name_space == NULL || key_length == 0u || key_length > H2_PREF_KEY_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  size_t namespace_length = strlen(name_space->path);
  if (namespace_length + 1u + key_length * 2u + 1u > H2_PREF_PATH_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memcpy(path, name_space->path, namespace_length);
  path[namespace_length] = '/';
  hex_encode(key, key_length, path + namespace_length + 1u);
  return H2_PAL_OK;
}

static int require_writable(const jieli_pref_namespace_t *name_space) {
  if (name_space == NULL) return H2_PAL_ERR_INVALID_ARG;
  return name_space->mode == H2_PAL_PREF_OPEN_READ_WRITE
             ? H2_PAL_OK
             : H2_PAL_ERR_INVALID_STATE;
}

static jieli_pref_namespace_t *to_namespace(h2_pal_pref_namespace_t *raw) {
  return raw == NULL ? NULL : (jieli_pref_namespace_t *)raw->user;
}

static int read_record(
    jieli_pref_namespace_t *name_space, const char *key,
    h2_pal_pref_entry_type_t expected_type, uint8_t **out_value,
    size_t *out_size) {
  if (out_value == NULL || out_size == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_value = NULL;
  *out_size = 0u;
  pref_trace_key("READ_ENTER", key, H2_PAL_OK);
  char path[H2_PREF_PATH_MAX];
  int result = record_path(name_space, key, path);
  if (result != H2_PAL_OK) return result;
  result = pref_lock();
  if (result != H2_PAL_OK) return result;
  result = pref_prepare_locked();
  lfs_file_t file;
  int file_open = 0;
  if (result == H2_PAL_OK) {
    result = map_lfs_error(lfs_file_open(&pref_lfs, &file, path, LFS_O_RDONLY));
    if (result == H2_PAL_OK) file_open = 1;
  }
  jieli_pref_header_t header;
  if (result == H2_PAL_OK &&
      lfs_file_read(&pref_lfs, &file, &header, sizeof(header)) !=
          (lfs_ssize_t)sizeof(header)) {
    result = H2_PAL_ERR_IO;
  }
  if (result == H2_PAL_OK &&
      (header.magic != H2_PREF_MAGIC || header.type != expected_type ||
       header.key_size != strlen(key) || header.value_size > H2_PREF_VALUE_MAX)) {
    result = H2_PAL_ERR_IO;
  }
  char stored_key[H2_PREF_KEY_MAX + 1u];
  if (result == H2_PAL_OK &&
      lfs_file_read(&pref_lfs, &file, stored_key, header.key_size) !=
          header.key_size) {
    result = H2_PAL_ERR_IO;
  }
  if (result == H2_PAL_OK) {
    stored_key[header.key_size] = '\0';
    if (strcmp(stored_key, key) != 0) result = H2_PAL_ERR_IO;
  }
  uint8_t *value = NULL;
  if (result == H2_PAL_OK) {
    value = malloc(header.value_size == 0u ? 1u : header.value_size);
    if (value == NULL) result = H2_PAL_ERR_NO_MEMORY;
  }
  if (result == H2_PAL_OK && header.value_size != 0u &&
      lfs_file_read(&pref_lfs, &file, value, header.value_size) !=
          (lfs_ssize_t)header.value_size) {
    result = H2_PAL_ERR_IO;
  }
  if (file_open) (void)lfs_file_close(&pref_lfs, &file);
  pref_unlock();
  pref_trace_key("READ_EXIT", key, result);
  if (result != H2_PAL_OK) {
    free(value);
    return result;
  }
  *out_value = value;
  *out_size = header.value_size;
  return H2_PAL_OK;
}

static int write_record(
    jieli_pref_namespace_t *name_space, const char *key,
    h2_pal_pref_entry_type_t type, const void *value, size_t value_size) {
  int result = require_writable(name_space);
  if (result != H2_PAL_OK) return result;
  if (!valid_name(key, H2_PREF_KEY_MAX) ||
      (value == NULL && value_size != 0u) || value_size > H2_PREF_VALUE_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  char path[H2_PREF_PATH_MAX];
  char temporary[H2_PREF_PATH_MAX];
  result = record_path(name_space, key, path);
  if (result != H2_PAL_OK || strlen(path) + 5u > sizeof(temporary)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  strcpy(temporary, path);
  strcat(temporary, ".tmp");
  result = pref_lock();
  if (result != H2_PAL_OK) return result;
  result = pref_prepare_locked();
  lfs_file_t file;
  int file_open = 0;
  if (result == H2_PAL_OK) {
    int open_result = lfs_file_open(
        &pref_lfs, &file, temporary,
        LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    result = map_lfs_error(open_result);
    if (result == H2_PAL_OK) file_open = 1;
  }
  jieli_pref_header_t header = {
      .magic = H2_PREF_MAGIC,
      .value_size = (uint32_t)value_size,
      .key_size = (uint16_t)strlen(key),
      .type = (uint8_t)type,
      .reserved = 0u,
  };
  if (result == H2_PAL_OK &&
      lfs_file_write(&pref_lfs, &file, &header, sizeof(header)) !=
          (lfs_ssize_t)sizeof(header)) result = H2_PAL_ERR_IO;
  if (result == H2_PAL_OK &&
      lfs_file_write(&pref_lfs, &file, key, header.key_size) !=
          header.key_size) result = H2_PAL_ERR_IO;
  if (result == H2_PAL_OK && value_size != 0u &&
      lfs_file_write(&pref_lfs, &file, value, value_size) !=
          (lfs_ssize_t)value_size) result = H2_PAL_ERR_IO;
  if (result == H2_PAL_OK) result = map_lfs_error(lfs_file_sync(&pref_lfs, &file));
  if (file_open) {
    int close_result = map_lfs_error(lfs_file_close(&pref_lfs, &file));
    if (result == H2_PAL_OK) result = close_result;
  }
  if (result == H2_PAL_OK) {
    /* littlefs replaces an existing destination in the rename transaction.
     * Removing it first loses the old record if rename fails or power is lost. */
    result = map_lfs_error(lfs_rename(&pref_lfs, temporary, path));
  } else {
    (void)lfs_remove(&pref_lfs, temporary);
  }
  pref_unlock();
  return result;
}

static int pref_close(h2_pal_pref_namespace_t *raw) {
  jieli_pref_namespace_t *name_space = to_namespace(raw);
  if (name_space == NULL) return H2_PAL_ERR_INVALID_ARG;
  free(name_space);
  return H2_PAL_OK;
}

static int pref_get_blob(
    h2_pal_pref_namespace_t *raw, const h2_pal_mem_api_t *allocator,
    const char *key, void **out_data, size_t *out_length) {
  uint8_t *value = NULL;
  size_t size = 0u;
  if (allocator == NULL || out_data == NULL || out_length == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_data = NULL;
  *out_length = 0u;
  int result = read_record(
      to_namespace(raw), key, H2_PAL_PREF_ENTRY_BLOB, &value, &size);
  if (result != H2_PAL_OK) return result;
  void *copy = h2_pal_mem_alloc(allocator, size == 0u ? 1u : size);
  if (copy == NULL) {
    free(value);
    return H2_PAL_ERR_NO_MEMORY;
  }
  if (size != 0u) memcpy(copy, value, size);
  free(value);
  *out_data = copy;
  *out_length = size;
  return H2_PAL_OK;
}

static int pref_set_blob(
    h2_pal_pref_namespace_t *raw, const char *key, const void *data,
    size_t length) {
  return write_record(
      to_namespace(raw), key, H2_PAL_PREF_ENTRY_BLOB, data, length);
}

static int pref_get_string(
    h2_pal_pref_namespace_t *raw, const h2_pal_mem_api_t *allocator,
    const char *key, char **out_value) {
  uint8_t *value = NULL;
  size_t size = 0u;
  if (allocator == NULL || out_value == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_value = NULL;
  int result = read_record(
      to_namespace(raw), key, H2_PAL_PREF_ENTRY_STRING, &value, &size);
  if (result != H2_PAL_OK) return result;
  char *copy = h2_pal_mem_alloc(allocator, size + 1u);
  if (copy == NULL) {
    free(value);
    return H2_PAL_ERR_NO_MEMORY;
  }
  if (size != 0u) memcpy(copy, value, size);
  copy[size] = '\0';
  free(value);
  *out_value = copy;
  return H2_PAL_OK;
}

static int pref_set_string(
    h2_pal_pref_namespace_t *raw, const char *key, const char *value) {
  size_t length = bounded_length(value, H2_PREF_VALUE_MAX);
  if (value == NULL || length > H2_PREF_VALUE_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return write_record(
      to_namespace(raw), key, H2_PAL_PREF_ENTRY_STRING, value, length);
}

static int pref_get_integer(
    h2_pal_pref_namespace_t *raw, const char *key,
    h2_pal_pref_entry_type_t type, uint32_t *out_value) {
  uint8_t *value = NULL;
  size_t size = 0u;
  if (out_value == NULL) return H2_PAL_ERR_INVALID_ARG;
  int result = read_record(to_namespace(raw), key, type, &value, &size);
  if (result != H2_PAL_OK) return result;
  if (size != 4u) {
    free(value);
    return H2_PAL_ERR_IO;
  }
  *out_value = (uint32_t)value[0] | ((uint32_t)value[1] << 8u) |
               ((uint32_t)value[2] << 16u) | ((uint32_t)value[3] << 24u);
  free(value);
  return H2_PAL_OK;
}

static int pref_set_integer(
    h2_pal_pref_namespace_t *raw, const char *key,
    h2_pal_pref_entry_type_t type, uint32_t value) {
  uint8_t bytes[4] = {
      (uint8_t)value, (uint8_t)(value >> 8u),
      (uint8_t)(value >> 16u), (uint8_t)(value >> 24u)};
  return write_record(to_namespace(raw), key, type, bytes, sizeof(bytes));
}

static int pref_get_u32(
    h2_pal_pref_namespace_t *raw, const char *key, uint32_t *out_value) {
  return pref_get_integer(raw, key, H2_PAL_PREF_ENTRY_U32, out_value);
}

static int pref_set_u32(
    h2_pal_pref_namespace_t *raw, const char *key, uint32_t value) {
  return pref_set_integer(raw, key, H2_PAL_PREF_ENTRY_U32, value);
}

static int pref_get_i32(
    h2_pal_pref_namespace_t *raw, const char *key, int32_t *out_value) {
  if (out_value == NULL) return H2_PAL_ERR_INVALID_ARG;
  uint32_t value = 0u;
  int result = pref_get_integer(raw, key, H2_PAL_PREF_ENTRY_I32, &value);
  if (result == H2_PAL_OK) *out_value = (int32_t)value;
  return result;
}

static int pref_set_i32(
    h2_pal_pref_namespace_t *raw, const char *key, int32_t value) {
  return pref_set_integer(raw, key, H2_PAL_PREF_ENTRY_I32, (uint32_t)value);
}

static int pref_get_bool(
    h2_pal_pref_namespace_t *raw, const char *key, int *out_value) {
  uint8_t *value = NULL;
  size_t size = 0u;
  if (out_value == NULL) return H2_PAL_ERR_INVALID_ARG;
  int result = read_record(
      to_namespace(raw), key, H2_PAL_PREF_ENTRY_BOOL, &value, &size);
  if (result != H2_PAL_OK) return result;
  if (size != 1u || value[0] > 1u) {
    free(value);
    return H2_PAL_ERR_IO;
  }
  *out_value = value[0] != 0u;
  free(value);
  return H2_PAL_OK;
}

static int pref_set_bool(
    h2_pal_pref_namespace_t *raw, const char *key, int value) {
  uint8_t stored = value != 0 ? 1u : 0u;
  return write_record(
      to_namespace(raw), key, H2_PAL_PREF_ENTRY_BOOL, &stored,
      sizeof(stored));
}

static int pref_remove(h2_pal_pref_namespace_t *raw, const char *key) {
  jieli_pref_namespace_t *name_space = to_namespace(raw);
  int result = require_writable(name_space);
  char path[H2_PREF_PATH_MAX];
  if (result == H2_PAL_OK) result = record_path(name_space, key, path);
  if (result != H2_PAL_OK) return result;
  result = pref_lock();
  if (result != H2_PAL_OK) return result;
  result = pref_prepare_locked();
  if (result == H2_PAL_OK) result = map_lfs_error(lfs_remove(&pref_lfs, path));
  pref_unlock();
  return result;
}

static int pref_clear(h2_pal_pref_namespace_t *raw) {
  jieli_pref_namespace_t *name_space = to_namespace(raw);
  int result = require_writable(name_space);
  if (result != H2_PAL_OK) return result;
  result = pref_lock();
  if (result != H2_PAL_OK) return result;
  result = pref_prepare_locked();
  lfs_dir_t directory;
  int directory_open = 0;
  if (result == H2_PAL_OK) {
    result = map_lfs_error(lfs_dir_open(&pref_lfs, &directory, name_space->path));
    if (result == H2_PAL_OK) directory_open = 1;
  }
  struct lfs_info info;
  while (result == H2_PAL_OK) {
    int read_result = lfs_dir_read(&pref_lfs, &directory, &info);
    if (read_result <= 0) {
      result = map_lfs_error(read_result);
      break;
    }
    if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) continue;
    char path[H2_PREF_PATH_MAX];
    if (strlen(name_space->path) + strlen(info.name) + 2u > sizeof(path)) {
      result = H2_PAL_ERR_IO;
      break;
    }
    strcpy(path, name_space->path);
    strcat(path, "/");
    strcat(path, info.name);
    if (lfs_remove(&pref_lfs, path) != LFS_ERR_OK) result = H2_PAL_ERR_IO;
  }
  if (directory_open) {
    int close_result = map_lfs_error(lfs_dir_close(&pref_lfs, &directory));
    if (result == H2_PAL_OK) result = close_result;
  }
  pref_unlock();
  return result;
}

static int pref_commit(h2_pal_pref_namespace_t *raw) {
  return to_namespace(raw) == NULL ? H2_PAL_ERR_INVALID_ARG : H2_PAL_OK;
}

static int pref_iterate(
    h2_pal_pref_namespace_t *raw, h2_pal_pref_cursor_t **cursor,
    h2_pal_pref_entry_t *out_entry) {
  jieli_pref_namespace_t *name_space = to_namespace(raw);
  if (name_space == NULL || cursor == NULL || out_entry == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  int result = pref_lock();
  if (result != H2_PAL_OK) return result;
  result = pref_prepare_locked();
  if (*cursor == NULL && result == H2_PAL_OK) {
    *cursor = calloc(1u, sizeof(**cursor));
    if (*cursor == NULL) result = H2_PAL_ERR_NO_MEMORY;
    if (result == H2_PAL_OK) {
      result = map_lfs_error(
          lfs_dir_open(&pref_lfs, &(*cursor)->directory, name_space->path));
      if (result == H2_PAL_OK) (*cursor)->open = 1;
      if (result != H2_PAL_OK) {
        free(*cursor);
        *cursor = NULL;
      }
    }
  }
  struct lfs_info info;
  int read_result = 0;
  do {
    if (result == H2_PAL_OK) {
      read_result = lfs_dir_read(&pref_lfs, &(*cursor)->directory, &info);
      if (read_result < 0) result = map_lfs_error(read_result);
    }
  } while (result == H2_PAL_OK && read_result > 0 &&
           (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0 ||
            strstr(info.name, ".tmp") != NULL));
  if (result == H2_PAL_OK && read_result == 0) result = H2_PAL_ERR_NOT_FOUND;
  if (result == H2_PAL_OK) {
    char path[H2_PREF_PATH_MAX];
    if (strlen(name_space->path) + strlen(info.name) + 2u > sizeof(path)) {
      result = H2_PAL_ERR_IO;
      goto done;
    }
    strcpy(path, name_space->path);
    strcat(path, "/");
    strcat(path, info.name);
    lfs_file_t file;
    result = map_lfs_error(lfs_file_open(&pref_lfs, &file, path, LFS_O_RDONLY));
    int file_open = result == H2_PAL_OK;
    jieli_pref_header_t header;
    if (result == H2_PAL_OK &&
        lfs_file_read(&pref_lfs, &file, &header, sizeof(header)) !=
            (lfs_ssize_t)sizeof(header)) result = H2_PAL_ERR_IO;
    if (result == H2_PAL_OK &&
        (header.magic != H2_PREF_MAGIC || header.key_size > H2_PREF_KEY_MAX)) {
      result = H2_PAL_ERR_IO;
    }
    if (result == H2_PAL_OK &&
        lfs_file_read(&pref_lfs, &file, (*cursor)->key, header.key_size) !=
            header.key_size) result = H2_PAL_ERR_IO;
    if (file_open) {
      int close_result = map_lfs_error(lfs_file_close(&pref_lfs, &file));
      if (result == H2_PAL_OK) result = close_result;
    }
    if (result == H2_PAL_OK) {
      (*cursor)->key[header.key_size] = '\0';
      *out_entry = (h2_pal_pref_entry_t){
          .key = (*cursor)->key,
          .type = (h2_pal_pref_entry_type_t)header.type,
          .value_size = header.value_size,
      };
    }
  }
done:
  pref_unlock();
  return result;
}

static int pref_iterate_close(
    h2_pal_pref_namespace_t *raw, h2_pal_pref_cursor_t **cursor) {
  (void)raw;
  if (cursor == NULL || *cursor == NULL) return H2_PAL_OK;
  int result = pref_lock();
  if (result != H2_PAL_OK) return result;
  if ((*cursor)->open) {
    result = map_lfs_error(lfs_dir_close(&pref_lfs, &(*cursor)->directory));
  }
  free(*cursor);
  *cursor = NULL;
  pref_unlock();
  return result;
}

static void initialize_namespace(jieli_pref_namespace_t *name_space) {
  name_space->pal = (h2_pal_pref_namespace_t){
      .user = name_space,
      .close = pref_close,
      .get_blob = pref_get_blob,
      .set_blob = pref_set_blob,
      .get_string = pref_get_string,
      .set_string = pref_set_string,
      .get_u32 = pref_get_u32,
      .set_u32 = pref_set_u32,
      .get_i32 = pref_get_i32,
      .set_i32 = pref_set_i32,
      .get_bool = pref_get_bool,
      .set_bool = pref_set_bool,
      .remove = pref_remove,
      .clear = pref_clear,
      .commit = pref_commit,
      .iterate = pref_iterate,
      .iterate_close = pref_iterate_close,
  };
}

static int pref_open(
    void *user, const char *name_space, h2_pal_pref_open_mode_t mode,
    h2_pal_pref_namespace_t **out_namespace) {
  (void)user;
  pref_trace_key("OPEN_ENTER", name_space, (int)mode);
  if (out_namespace == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_namespace = NULL;
  if (!valid_name(name_space, H2_PREF_NAMESPACE_MAX) ||
      (mode != H2_PAL_PREF_OPEN_READ_ONLY &&
       mode != H2_PAL_PREF_OPEN_READ_WRITE)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  int result = pref_lock();
  if (result != H2_PAL_OK) return result;
  pref_trace("H2_JIELI_PREF_OK step=lock\r\n");
  result = pref_prepare_locked();
  jieli_pref_namespace_t *instance = NULL;
  if (result == H2_PAL_OK) {
    pref_trace("H2_JIELI_PREF_ENTER step=namespace_alloc\r\n");
    instance = calloc(1u, sizeof(*instance));
    if (instance == NULL) result = H2_PAL_ERR_NO_MEMORY;
  }
  if (result == H2_PAL_OK) {
    pref_trace("H2_JIELI_PREF_ENTER step=namespace_mkdir\r\n");
    instance->path[0] = '/';
    hex_encode(name_space, strlen(name_space), instance->path + 1u);
    int mkdir_result = lfs_mkdir(&pref_lfs, instance->path);
    if (mkdir_result != LFS_ERR_OK && mkdir_result != LFS_ERR_EXIST) {
      result = map_lfs_error(mkdir_result);
    }
  }
  pref_unlock();
  if (result != H2_PAL_OK) {
    free(instance);
    return result;
  }
  initialize_namespace(instance);
  instance->mode = mode;
  *out_namespace = &instance->pal;
  pref_trace_key("OPEN_EXIT", name_space, result);
  return H2_PAL_OK;
}

void h2_jieli_ac791n_devkit_pref_set_diagnostic(
    void (*write_line)(const char *line)) {
  pref_diagnostic = write_line;
}

const h2_pal_pref_api_t *h2_jieli_ac791n_devkit_pref_api(void) {
  static const h2_pal_pref_vtable_t vtable = {.open = pref_open};
  static const h2_pal_pref_api_t api = {.user = NULL, .vtable = &vtable};
  return &api;
}
