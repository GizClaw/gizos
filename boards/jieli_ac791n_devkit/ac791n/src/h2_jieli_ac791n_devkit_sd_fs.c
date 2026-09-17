#include "app_config.h"
#include "device/device.h"
#include "device/ioctl_cmds.h"
#include "fs/fs.h"
#include "system/includes.h"

#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_wl82_atomic.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define H2_JIELI_SD_MOUNT "storage/sd0"
#define H2_JIELI_SD_ROOT "storage/sd0/C/"
#ifdef CONFIG_JLFAT_ENABLE
#define H2_JIELI_SD_FS_TYPE "jlfat"
#else
#define H2_JIELI_SD_FS_TYPE "fat"
#endif
#define H2_JIELI_SD_PATH_MAX 192u
#define H2_JIELI_SD_CACHE_COUNT 32
#define H2_JIELI_SD_WRITE_TRACE_INTERVAL (16u * 1024u)
#define H2_JIELI_SD_READY_TIMEOUT_MS 5000u
extern void h2_jieli_sd_fs_trace_read(
    const char *stage, const void *data, size_t length, int result)
    __attribute__((weak));
extern void h2_jieli_sd_fs_trace_write(
    const char *stage, size_t offset, size_t length, int result)
    __attribute__((weak));
extern void h2_jieli_sd_fs_trace_mkdir(
    const char *path, int native_result, int lookup_result)
    __attribute__((weak));

struct h2_pal_fs_file {
  FILE *native;
  size_t write_offset;
  size_t next_write_trace;
  char path[H2_JIELI_SD_PATH_MAX];
  h2_pal_fs_open_mode_t mode;
  struct h2_pal_fs_file *next;
};

static h2_pal_fs_file_t *sd_open_files;
static uint32_t sd_files_gate;

static void sd_files_lock(void) {
  for (;;) {
    uint32_t expected = 0u;
    if (h2_jieli_atomic_cas_u32(&sd_files_gate, &expected, 1u)) return;
    os_time_dly(1u);
  }
}

static void sd_files_unlock(void) {
  h2_jieli_atomic_store_u32(&sd_files_gate, 0u);
}

/* Caller holds sd_files_gate; descendants match only at a separator. */
static int sd_path_busy(
    const char *path, int descendants, h2_pal_fs_open_mode_t mode) {
  size_t length = strlen(path);
  for (h2_pal_fs_file_t *file = sd_open_files; file != NULL; file = file->next) {
    if (strcmp(file->path, path) != 0 &&
        !(descendants && strncmp(file->path, path, length) == 0 &&
          file->path[length] == '/')) continue;
    if (mode != H2_PAL_FS_OPEN_READ ||
        file->mode == H2_PAL_FS_OPEN_WRITE_TRUNCATE) return 1;
  }
  return 0;
}

static int sd_mounted;
static int sd_mount_owned;
static const char *sd_last_stage = "idle";
static uint32_t sd_capacity;
static uint32_t sd_block_size;
static int sd_sector0_read;
static uint8_t sd_sector0[512];
static int sd_volume_read;
static uint8_t sd_volume[512];
static int sd_heap_probe_16k;
static int sd_heap_probe_32k;
static int sd_partition_device_open;
static int sd_fat1_read;
static int sd_fat2_read;
static uint8_t sd_fat1[512];
static uint8_t sd_fat2[512];

extern int snprintf(char *buffer, size_t size, const char *format, ...);

static uint32_t read_le32(const uint8_t *data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
         ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

static uint16_t read_le16(const uint8_t *data) {
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8u);
}

static void capture_sd_diagnostic(void) {
  void *device = dev_open("sd0", NULL);
  if (device == NULL) {
    sd_sector0_read = H2_PAL_ERR_UNAVAILABLE;
    return;
  }
  memset(sd_sector0, 0, sizeof(sd_sector0));
  sd_capacity = 0u;
  sd_block_size = 0u;
  (void)dev_ioctl(device, IOCTL_GET_CAPACITY, (uint32_t)&sd_capacity);
  (void)dev_ioctl(device, IOCTL_GET_BLOCK_SIZE, (uint32_t)&sd_block_size);
  sd_sector0_read = dev_bulk_read(device, sd_sector0, 0u, 1u);
  memset(sd_volume, 0, sizeof(sd_volume));
  sd_volume_read = sd_sector0_read > 0
                       ? dev_bulk_read(
                             device, sd_volume,
                             read_le32(&sd_sector0[454]), 1u)
                       : sd_sector0_read;
  uint32_t fat1_lba = read_le32(&sd_sector0[454]) +
                      read_le16(&sd_volume[14]);
  uint32_t fat_sectors = read_le16(&sd_volume[22]);
  sd_fat1_read = dev_bulk_read(device, sd_fat1, fat1_lba, 1u);
  sd_fat2_read = dev_bulk_read(
      device, sd_fat2, fat1_lba + fat_sectors, 1u);
  dev_close(device);
  void *partition_device = dev_open("sd0.0", NULL);
  sd_partition_device_open = partition_device != NULL;
  if (partition_device != NULL) dev_close(partition_device);
}

const char *h2_jieli_ac791n_devkit_sd_fs_last_stage(void) {
  return sd_last_stage;
}

int h2_jieli_ac791n_devkit_sd_fs_diagnostic(
    char *out, size_t out_size) {
  if (out == NULL || out_size == 0u) return H2_PAL_ERR_INVALID_ARG;
  uint32_t bytes_per_sector = read_le16(&sd_volume[11]);
  uint32_t sectors_per_cluster = sd_volume[13];
  uint32_t reserved_sectors = read_le16(&sd_volume[14]);
  uint32_t fat_count = sd_volume[16];
  uint32_t root_entries = read_le16(&sd_volume[17]);
  uint32_t total_sectors = read_le16(&sd_volume[19]);
  if (total_sectors == 0u) total_sectors = read_le32(&sd_volume[32]);
  uint32_t fat_sectors = read_le16(&sd_volume[22]);
  uint32_t root_sectors = bytes_per_sector == 0u
                              ? 0u
                              : (root_entries * 32u + bytes_per_sector - 1u) /
                                    bytes_per_sector;
  uint32_t overhead = reserved_sectors + fat_count * fat_sectors + root_sectors;
  uint32_t clusters = sectors_per_cluster == 0u || total_sectors < overhead
                          ? 0u
                          : (total_sectors - overhead) / sectors_per_cluster;
  return snprintf(
      out, out_size,
      "capacity=%u block=%u read0=%d sig=%02x%02x p0_type=%02x "
      "p0_status=%02x p0_lba=%u p0_sectors=%u volread=%d volsig=%02x%02x "
      "bps=%u spc=%u reserved=%u fats=%u root=%u fatsecs=%u media=%02x "
      "hidden=%u total=%u clusters=%u heap16=%d heap32=%d subdev=%d "
      "fatread=%d/%d fathead=%02x%02x%02x%02x/%02x%02x%02x%02x "
      "fateq=%d fat54=%.5s",
      (unsigned)sd_capacity, (unsigned)sd_block_size, sd_sector0_read,
      sd_sector0[510], sd_sector0[511], sd_sector0[450], sd_sector0[446],
      (unsigned)read_le32(&sd_sector0[454]),
      (unsigned)read_le32(&sd_sector0[458]), sd_volume_read,
      sd_volume[510], sd_volume[511],
      (unsigned)bytes_per_sector, (unsigned)sectors_per_cluster,
      (unsigned)reserved_sectors, (unsigned)fat_count,
      (unsigned)root_entries, (unsigned)fat_sectors, sd_volume[21],
      (unsigned)read_le32(&sd_volume[28]), (unsigned)total_sectors,
      (unsigned)clusters, sd_heap_probe_16k, sd_heap_probe_32k,
      sd_partition_device_open,
      sd_fat1_read, sd_fat2_read,
      sd_fat1[0], sd_fat1[1], sd_fat1[2], sd_fat1[3],
      sd_fat2[0], sd_fat2[1], sd_fat2[2], sd_fat2[3],
      memcmp(sd_fat1, sd_fat2, sizeof(sd_fat1)) == 0,
      &sd_volume[54]) >= 0
             ? H2_PAL_OK
             : H2_PAL_ERR_IO;
}

static int map_error(int result) {
  return result == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static int translate_path(
    const char *path, char out[H2_JIELI_SD_PATH_MAX]) {
  const char *suffix;
  if (path == NULL || out == NULL) return H2_PAL_ERR_INVALID_ARG;
  /* Validate public components before native FAT can interpret separators or
   * parent traversal. A leading dot in a normal filename remains valid. */
  if (path[0] != '/') return H2_PAL_ERR_INVALID_ARG;
  const char *component = path + 1;
  size_t component_units = 0u;
  for (const char *cursor = component;; ++cursor) {
    if (*cursor == '\\') return H2_PAL_ERR_INVALID_ARG;
    if (*cursor != '/' && *cursor != '\0') {
      unsigned char byte = (unsigned char)*cursor;
      /* JLFAT silently truncates components beyond 130 UTF-16 units. */
      if ((byte & 0xc0u) != 0x80u) ++component_units;
      if (byte >= 0xf0u && byte <= 0xf7u) ++component_units;
      if (component_units > 130u) return H2_PAL_ERR_NO_SPACE;
      continue;
    }
    const size_t length = (size_t)(cursor - component);
    if (length == 0u ||
        (length == 1u && component[0] == '.') ||
        (length == 2u && component[0] == '.' && component[1] == '.')) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    if (*cursor == '\0') break;
    component = cursor + 1;
    component_units = 0u;
  }
  /* JLFAT's frename API accepts only a native filename for the destination.
   * Keep Loader's public paths unchanged while mapping its transactional
   * files to 8.3 names that are safe for native rename. */
  if (strcmp(path, "/dl/update.tar.zlib.tmp") == 0) {
    suffix = "/dl/H2STAGE.TMP";
  } else if (strcmp(path, "/dl/update.tar.zlib.prev") == 0) {
    suffix = "/dl/H2PREV.BIN";
  } else if (strcmp(path, "/dl/update.tar.zlib") == 0) {
    suffix = "/dl/H2STAGE.BIN";
  } else if (strcmp(path, "/dl/.h2loader-image.tmp") == 0) {
    suffix = "/dl/H2IMG.TMP";
  } else if (strcmp(path, "/dl/.h2loader-image-1") == 0) {
    suffix = "/dl/H2IMG1.BIN";
  } else if (strcmp(path, "/dl/.h2loader-image-2") == 0) {
    suffix = "/dl/H2IMG2.BIN";
  } else if (strcmp(path, "/data/.checksum") == 0) {
    suffix = "/data/H2CHECK.SUM";
  } else if (strcmp(path, "/dl") == 0) {
    suffix = "/dl";
  } else if (strncmp(path, "/dl/", 4u) == 0) {
    suffix = path;
  } else if (strcmp(path, "/data") == 0) {
    suffix = "/data";
  } else if (strncmp(path, "/data/", 6u) == 0) {
    suffix = path;
  } else {
    return H2_PAL_ERR_INVALID_ARG;
  }
  size_t root_len = strlen(H2_JIELI_SD_ROOT);
  size_t suffix_len = strlen(suffix);
  /* Full written length includes the NUL replacing the leading slash:
   * at most H2_JIELI_SD_PATH_MAX bytes, the size of the mapped[] buffers. */
  if (root_len + suffix_len > H2_JIELI_SD_PATH_MAX) {
    return H2_PAL_ERR_NO_SPACE;
  }
  memcpy(out, H2_JIELI_SD_ROOT, root_len);
  memcpy(out + root_len, suffix + 1, suffix_len - 1u);
  out[root_len + suffix_len - 1u] = '\0';
  return H2_PAL_OK;
}

/* SDK fopen already encodes UTF-8 components for JLFAT. fopen_by_utf8 would
 * encode them again when /data or /dl is the first (short) component, losing
 * the remainder at an embedded UTF-16 NUL. Use fopen for every mapped path. */
static int directory_status(const char *path) {
  FILE *file = fopen(path, "r");
  if (file == NULL) return H2_PAL_ERR_NOT_FOUND;
  int attributes = 0;
  int result = fget_attr(file, &attributes);
  int close_result = fclose(file);
  if (result != 0 || close_result != 0) return H2_PAL_ERR_IO;
  return (attributes & F_ATTR_DIR) != 0
      ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}

static int create_directory_component(char *path) {
  int existing = directory_status(path);
  if (existing != H2_PAL_ERR_NOT_FOUND) return existing;
  size_t length = strlen(path);
  /* JLFAT fmk_dir uses the short-name parser, which stops after eight base
   * characters and reports FR_NO_PATH for a long component. SDK fopen's LFN
   * parser creates a directory when the component ends in a slash. */
  path[length] = '/';
  path[length + 1u] = '\0';
  FILE *directory = fopen(path, "w+");
  path[length] = '\0';
  int native_result = directory == NULL ? H2_PAL_ERR_IO : H2_PAL_OK;
  if (directory != NULL) {
    int attributes = 0;
    if (fget_attr(directory, &attributes) != 0) {
      native_result = H2_PAL_ERR_IO;
    } else if ((attributes & F_ATTR_DIR) == 0) {
      native_result = H2_PAL_ERR_INVALID_STATE;
    }
    if (fclose(directory) != 0) native_result = H2_PAL_ERR_IO;
  }
  /* The post-create lookup is authoritative, including when a concurrent
   * creator wins or the trailing-slash creation handle reports no DIR bit.
   * Native handle errors do not invalidate a verified directory entry. */
  int created = directory_status(path);
  if (h2_jieli_sd_fs_trace_mkdir != NULL)
    h2_jieli_sd_fs_trace_mkdir(path, native_result, created);
  return created == H2_PAL_ERR_NOT_FOUND ? H2_PAL_ERR_IO : created;
}

static int ensure_directory(const char *path) {
  /* Leave room for the native directory separator and its terminator. */
  char component[H2_JIELI_SD_PATH_MAX + 1u];
  size_t root_len = strlen(H2_JIELI_SD_ROOT);
  size_t length = strlen(path);
  if (strncmp(path, H2_JIELI_SD_ROOT, root_len) != 0 ||
      length <= root_len || length >= H2_JIELI_SD_PATH_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memcpy(component, path, length + 1u);
  char *leaf = strrchr(component, '/');
  /* PAL mkdir is single-level. SDK fopen can create intermediate directories,
   * so reject a missing or non-directory parent before invoking it. The SD
   * volume root itself is established by mount, not a normal directory entry. */
  if ((size_t)(leaf - component) != root_len - 1u) {
    *leaf = '\0';
    int result = directory_status(component);
    if (result != H2_PAL_OK) return result;
    *leaf = '/';
  }
  return create_directory_component(component);
}

static int fs_mkdir(void *user, const char *path) {
  (void)user;
  char mapped[H2_JIELI_SD_PATH_MAX];
  int result = translate_path(path, mapped);
  return result == H2_PAL_OK ? ensure_directory(mapped) : result;
}

static int fs_open(
    void *user, const char *path, h2_pal_fs_open_mode_t mode,
    h2_pal_fs_file_t **out_file) {
  (void)user;
  char mapped[H2_JIELI_SD_PATH_MAX];
  if (out_file == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_file = NULL;
  int result = translate_path(path, mapped);
  if (result != H2_PAL_OK) return result;
  const char *native_mode = mode == H2_PAL_FS_OPEN_READ
                                ? "r"
                                : mode == H2_PAL_FS_OPEN_WRITE_TRUNCATE ? "w+"
                                                                        : NULL;
  if (native_mode == NULL) return H2_PAL_ERR_INVALID_ARG;
  sd_files_lock();
  if (sd_path_busy(mapped, 0, mode)) {
    sd_files_unlock();
    return H2_PAL_ERR_BUSY;
  }
  /* JLFAT can open a directory for writing, exposing its data to writes.
   * Inspect an existing entry before allowing a native write open. */
  if (mode == H2_PAL_FS_OPEN_WRITE_TRUNCATE) {
    FILE *existing = fopen(mapped, "r");
    if (existing != NULL) {
      int attributes = 0;
      int attr_result = fget_attr(existing, &attributes);
      int close_result = fclose(existing);
      if (attr_result != 0 || close_result != 0 ||
          (attributes & F_ATTR_DIR) != 0) {
        sd_files_unlock();
        return attr_result != 0 || close_result != 0
            ? H2_PAL_ERR_IO : H2_PAL_ERR_INVALID_STATE;
      }
    }
  }
  sd_last_stage = "fopen-enter";
  FILE *native = fopen(mapped, native_mode);
  sd_last_stage = "fopen-return";
  if (native == NULL) {
    sd_files_unlock();
    return mode == H2_PAL_FS_OPEN_READ ? H2_PAL_ERR_NOT_FOUND
                                       : H2_PAL_ERR_IO;
  }
  int attributes = 0;
  int attr_result = fget_attr(native, &attributes);
  if (attr_result != 0 || (attributes & F_ATTR_DIR) != 0) {
    int close_result = fclose(native);
    sd_files_unlock();
    return attr_result != 0 || close_result != 0
        ? H2_PAL_ERR_IO : H2_PAL_ERR_INVALID_STATE;
  }
  h2_pal_fs_file_t *file = malloc(sizeof(*file));
  if (file == NULL) {
    (void)fclose(native);
    sd_files_unlock();
    return H2_PAL_ERR_NO_MEMORY;
  }
  file->native = native;
  file->write_offset = 0u;
  file->next_write_trace = 0u;
  memcpy(file->path, mapped, strlen(mapped) + 1u);
  file->mode = mode;
  file->next = sd_open_files;
  sd_open_files = file;
  *out_file = file;
  sd_files_unlock();
  return H2_PAL_OK;
}

static int fs_read(
    void *user, h2_pal_fs_file_t *file, void *data, size_t length,
    size_t *out_read) {
  (void)user;
  if (file == NULL || file->native == NULL || out_read == NULL ||
      (data == NULL && length != 0u) || length > UINT32_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const uint32_t native_length = (uint32_t)length;
  if (h2_jieli_sd_fs_trace_read != NULL) {
    h2_jieli_sd_fs_trace_read("fread-enter", data, native_length, 0);
  }
  int read = fread(data, 1u, native_length, file->native);
  if (h2_jieli_sd_fs_trace_read != NULL) {
    h2_jieli_sd_fs_trace_read("fread-return", data, native_length, read);
  }
  if (read < 0) return H2_PAL_ERR_IO;
  *out_read = (size_t)read;
  return H2_PAL_OK;
}

static int fs_seek(void *user, h2_pal_fs_file_t *file, uint64_t position) {
  (void)user;
  if (file == NULL || file->native == NULL || position > UINT32_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return map_error(fseek(file->native, (uint32_t)position, SEEK_SET));
}

static int fs_write(
    void *user, h2_pal_fs_file_t *file, const void *data, size_t length,
    size_t *out_written) {
  (void)user;
  if (file == NULL || file->native == NULL || out_written == NULL ||
      (data == NULL && length != 0u) || length > UINT32_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const int trace = h2_jieli_sd_fs_trace_write != NULL &&
                    file->write_offset >= file->next_write_trace;
  if (trace) {
    h2_jieli_sd_fs_trace_write(
        "fwrite-enter", file->write_offset, length, 0);
  }
  int written = fwrite((void *)data, 1u, (uint32_t)length, file->native);
  if (h2_jieli_sd_fs_trace_write != NULL &&
      (trace || written < 0 || (size_t)written != length)) {
    h2_jieli_sd_fs_trace_write(
        "fwrite-return", file->write_offset, length, written);
  }
  if (written < 0) return H2_PAL_ERR_IO;
  *out_written = (size_t)written;
  file->write_offset += (size_t)written;
  if (trace) {
    file->next_write_trace =
        file->write_offset + H2_JIELI_SD_WRITE_TRACE_INTERVAL;
  }
  return (size_t)written == length ? H2_PAL_OK : H2_PAL_ERR_NO_SPACE;
}

static int fs_sync(void *user, h2_pal_fs_file_t *file) {
  (void)user;
  if (file == NULL || file->native == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (h2_jieli_sd_fs_trace_write != NULL) {
    h2_jieli_sd_fs_trace_write("sync-enter", file->write_offset, 0u, 0);
  }
  int result = f_free_cache(H2_JIELI_SD_ROOT);
  if (h2_jieli_sd_fs_trace_write != NULL) {
    h2_jieli_sd_fs_trace_write("sync-return", file->write_offset, 0u, result);
  }
  return map_error(result);
}

static int fs_close(void *user, h2_pal_fs_file_t *file) {
  (void)user;
  if (file == NULL || file->native == NULL) return H2_PAL_ERR_INVALID_ARG;
  const int trace = h2_jieli_sd_fs_trace_write != NULL && file->write_offset != 0u;
  if (trace) {
    h2_jieli_sd_fs_trace_write("close-enter", file->write_offset, 0u, 0);
  }
  sd_files_lock();
  int result = fclose(file->native);
  if (trace) {
    h2_jieli_sd_fs_trace_write("close-return", file->write_offset, 0u, result);
  }
  h2_pal_fs_file_t **link = &sd_open_files;
  while (*link != NULL && *link != file) link = &(*link)->next;
  if (*link == file) *link = file->next;
  file->native = NULL;
  free(file);
  sd_files_unlock();
  return map_error(result);
}

static int fs_stat(void *user, const char *path, h2_pal_fs_stat_t *out_stat) {
  (void)user;
  char mapped[H2_JIELI_SD_PATH_MAX];
  if (out_stat == NULL) return H2_PAL_ERR_INVALID_ARG;
  int result = translate_path(path, mapped);
  if (result != H2_PAL_OK) return result;
  /* SDK fdir_exist() tests fopen(path, "r"), which also succeeds for regular
   * files. Inspect the opened entry's FAT attributes instead. */
  FILE *file = fopen(mapped, "r");
  if (file == NULL) return H2_PAL_ERR_NOT_FOUND;
  int attributes = 0;
  int attr_result = fget_attr(file, &attributes);
  uint32_t file_size = flen(file);
  int close_result = fclose(file);
  if (attr_result != 0 || close_result != 0) return H2_PAL_ERR_IO;
  if ((attributes & F_ATTR_DIR) != 0) {
    long long size = flen_dir(mapped);
    /* Empty JLFAT directories can have a negative aggregate length even
     * though their directory entry exists. Do not report them missing. */
    *out_stat = (h2_pal_fs_stat_t){.size = size < 0 ? 0u : (uint64_t)size, .is_dir = 1};
    return H2_PAL_OK;
  }
  *out_stat = (h2_pal_fs_stat_t){.size = file_size, .is_dir = 0};
  return H2_PAL_OK;
}

static int fs_clear(void *user, const char *path) {
  (void)user;
  char mapped[H2_JIELI_SD_PATH_MAX];
  int result = translate_path(path, mapped);
  if (result != H2_PAL_OK) return result;
  sd_files_lock();
  if (sd_path_busy(mapped, 1, H2_PAL_FS_OPEN_WRITE_TRUNCATE)) {
    sd_files_unlock();
    return H2_PAL_ERR_BUSY;
  }
  result = fdelete_dir(mapped);
  result = result == 0 ? ensure_directory(mapped) : H2_PAL_ERR_IO;
  sd_files_unlock();
  return result;
}

static int fs_remove(void *user, const char *path) {
  (void)user;
  char mapped[H2_JIELI_SD_PATH_MAX];
  int result = translate_path(path, mapped);
  if (result != H2_PAL_OK) return result;
  sd_files_lock();
  if (sd_path_busy(mapped, 0, H2_PAL_FS_OPEN_WRITE_TRUNCATE)) {
    sd_files_unlock();
    return H2_PAL_ERR_BUSY;
  }
  FILE *file = fopen(mapped, "r");
  if (file == NULL) {
    sd_files_unlock();
    return H2_PAL_ERR_NOT_FOUND;
  }
  /* Jieli's fdelete() removes the entry represented by the already-open
   * handle and closes that handle atomically.  Closing it first and then
   * resolving the path again through fdelete_by_name() can leave JLFAT
   * waiting on stale metadata after an interrupted stage write. */
  result = fdelete(file);
  /* fdelete() commits the directory mutation while closing the handle.  Do
   * not follow it with f_free_cache(): on WL82/JLFAT that whole-volume cache
   * flush can block forever after a previously interrupted write. */
  sd_files_unlock();
  return map_error(result);
}

static int fs_rename(
    void *user, const char *old_path, const char *new_path) {
  (void)user;
  char old_mapped[H2_JIELI_SD_PATH_MAX];
  char new_mapped[H2_JIELI_SD_PATH_MAX];
  int result = translate_path(old_path, old_mapped);
  if (result != H2_PAL_OK) return result;
  result = translate_path(new_path, new_mapped);
  if (result != H2_PAL_OK) return result;
  const char *old_name = strrchr(old_mapped, '/');
  const char *new_name = strrchr(new_mapped, '/');
  if (old_name == NULL || new_name == NULL ||
      (size_t)(old_name - old_mapped) != (size_t)(new_name - new_mapped) ||
      memcmp(old_mapped, new_mapped, (size_t)(old_name - old_mapped)) != 0) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  sd_files_lock();
  if (strcmp(old_mapped, new_mapped) == 0) {
    FILE *file = fopen(old_mapped, "r");
    result = file == NULL ? H2_PAL_ERR_NOT_FOUND : map_error(fclose(file));
    sd_files_unlock();
    return result;
  }
  /* Keep the registry gate through the mutation: another open must not gain
   * a stale JLFAT directory-entry pointer between the check and rename. */
  if (sd_path_busy(old_mapped, 0, H2_PAL_FS_OPEN_WRITE_TRUNCATE) ||
      sd_path_busy(new_mapped, 0, H2_PAL_FS_OPEN_WRITE_TRUNCATE)) {
    sd_files_unlock();
    return H2_PAL_ERR_BUSY;
  }
  FILE *file = fopen(old_mapped, "r");
  if (file == NULL) {
    sd_files_unlock();
    return H2_PAL_ERR_NOT_FOUND;
  }
  FILE *destination = fopen(new_mapped, "r");
  if (destination != NULL) {
    int attributes = 0;
    int attr_result = fget_attr(destination, &attributes);
    if (attr_result != 0 || (attributes & F_ATTR_DIR) != 0) {
      int destination_close = fclose(destination);
      int source_close = fclose(file);
      sd_files_unlock();
      return attr_result != 0 || destination_close != 0 || source_close != 0
          ? H2_PAL_ERR_IO : H2_PAL_ERR_INVALID_STATE;
    }
    /* fdelete consumes the destination handle even when unlink fails. */
    if (fdelete(destination) != 0) {
      (void)fclose(file);
      sd_files_unlock();
      return H2_PAL_ERR_IO;
    }
  }
  result = frename(file, new_name + 1);
  int close_result = fclose(file);
  if (result != 0 || close_result != 0) {
    sd_files_unlock();
    return H2_PAL_ERR_IO;
  }
  file = fopen(new_mapped, "r");
  result = file == NULL ? H2_PAL_ERR_IO : map_error(fclose(file));
  sd_files_unlock();
  return result;
}

static int wait_sd_online(void) {
  /* SDK card detection is asynchronous: app_main can run before sd0 online.
   * Yield to detection instead of turning this normal startup race into a
   * permanent App startup failure. A missing card still has a bounded exit. */
  uint32_t started = timer_get_ms();
  while (!dev_online("sd0")) {
    if ((uint32_t)(timer_get_ms() - started) >= H2_JIELI_SD_READY_TIMEOUT_MS) {
      return H2_PAL_ERR_UNAVAILABLE;
    }
    os_time_dly(5u);
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_jieli_ac791n_devkit_sd_fs_init(h2_pal_fs_api_t *out_api) {
  static const h2_pal_fs_vtable_t vtable = {
      .mkdir = fs_mkdir,
      .open = fs_open,
      .read = fs_read,
      .seek = fs_seek,
      .write = fs_write,
      .sync = fs_sync,
      .close = fs_close,
      .stat = fs_stat,
      .clear = fs_clear,
      .remove = fs_remove,
      .rename = fs_rename,
  };
  if (out_api == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!sd_mounted) {
    sd_last_stage = "online";
    int ready_result = wait_sd_online();
    if (ready_result != H2_PAL_OK) return ready_result;
    sd_last_stage = "mount";
    if (!fmount_exist(H2_JIELI_SD_MOUNT)) {
      void *heap_probe = malloc(32768u);
      sd_heap_probe_32k = heap_probe != NULL;
      free(heap_probe);
      heap_probe = malloc(16384u);
      sd_heap_probe_16k = heap_probe != NULL;
      free(heap_probe);
      sd_last_stage = "mount";
      /* The SDK FAT mount runs mbr_scan and check_fs on each partition's
       * boot sector. Pass the whole sd0 disk, not an offset block wrapper. */
      struct imount *mounted = mount(
          "sd0", H2_JIELI_SD_MOUNT,
          H2_JIELI_SD_FS_TYPE, H2_JIELI_SD_CACHE_COUNT, NULL);
      if (mounted == NULL) {
        capture_sd_diagnostic();
        return H2_PAL_ERR_IO;
      }
      sd_mount_owned = 1;
    }
    sd_last_stage = "mkdir_dl";
    if (ensure_directory(H2_JIELI_SD_ROOT "dl") != H2_PAL_OK) {
      if (sd_mount_owned && unmount(H2_JIELI_SD_MOUNT) == 0) {
        sd_mount_owned = 0;
      }
      return H2_PAL_ERR_IO;
    }
    sd_last_stage = "mkdir_data";
    if (ensure_directory(H2_JIELI_SD_ROOT "data") != H2_PAL_OK) {
      if (sd_mount_owned && unmount(H2_JIELI_SD_MOUNT) == 0) {
        sd_mount_owned = 0;
      }
      return H2_PAL_ERR_IO;
    }
  }
  sd_mounted = 1;
  sd_last_stage = "ready";
  *out_api = (h2_pal_fs_api_t){.user = NULL, .vtable = &vtable};
  return H2_PAL_OK;
}

h2_pal_result_t h2_jieli_ac791n_devkit_sd_fs_deinit(void) {
  if (!sd_mount_owned) {
    sd_mounted = 0;
    return H2_PAL_OK;
  }
  int result = unmount(H2_JIELI_SD_MOUNT);
  if (result == 0) {
    sd_mounted = 0;
    sd_mount_owned = 0;
  }
  return map_error(result);
}
