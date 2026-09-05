#include "app_config.h"
#include "device/device.h"
#include "device/ioctl_cmds.h"
#include "fs/fs.h"
#include "system/includes.h"

#include "h2_jieli_ac791n_devkit.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define H2_JIELI_SD_MOUNT "storage/sd0"
#define H2_JIELI_SD_ROOT "storage/sd0/C/"
#define H2_JIELI_SD_VOLUME_DEVICE "h2sdv"
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

struct h2_pal_fs_file {
  FILE *native;
  size_t write_offset;
  size_t next_write_trace;
};

static int sd_mounted;
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
static uint32_t sd_volume_open_count;
static int sd_volume_open_result;
static uint32_t sd_volume_read_count;
static uint32_t sd_volume_last_read_offset;
static uint32_t sd_volume_last_read_length;
static int sd_volume_last_read_result;
static uint32_t sd_volume_ioctl_count;
static uint32_t sd_volume_last_ioctl;
static int sd_volume_last_ioctl_result;

typedef struct h2_jieli_sd_volume_device {
  struct device device;
  void *backing;
  uint32_t offset;
  uint32_t sectors;
} h2_jieli_sd_volume_device_t;

static h2_jieli_sd_volume_device_t sd_volume_device;

extern int snprintf(char *buffer, size_t size, const char *format, ...);

static uint32_t read_le32(const uint8_t *data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
         ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

static uint16_t read_le16(const uint8_t *data) {
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8u);
}

static bool volume_online(const struct dev_node *node) {
  (void)node;
  return dev_online("sd0");
}

static int volume_init(const struct dev_node *node, void *arg) {
  (void)node;
  (void)arg;
  return 0;
}

static int volume_open(
    const char *name, struct device **out_device, void *arg) {
  (void)name;
  (void)arg;
  ++sd_volume_open_count;
  sd_volume_open_result = -1;
  if (out_device == NULL || sd_volume_device.backing != NULL) return -1;
  void *backing = dev_open("sd0", NULL);
  if (backing == NULL) return -1;
  uint8_t mbr[512];
  if (dev_bulk_read(backing, mbr, 0u, 1u) != 1 ||
      mbr[510] != 0x55u || mbr[511] != 0xaau) {
    dev_close(backing);
    return -1;
  }
  uint32_t offset = read_le32(&mbr[454]);
  uint32_t sectors = read_le32(&mbr[458]);
  if (offset == 0u || sectors == 0u) {
    dev_close(backing);
    return -1;
  }
  sd_volume_device.backing = backing;
  sd_volume_device.offset = offset;
  sd_volume_device.sectors = sectors;
  *out_device = &sd_volume_device.device;
  sd_volume_open_result = 0;
  return 0;
}

static int volume_read(
    struct device *device, void *buffer, uint32_t length, uint32_t offset) {
  h2_jieli_sd_volume_device_t *volume =
      (h2_jieli_sd_volume_device_t *)device;
  ++sd_volume_read_count;
  sd_volume_last_read_offset = offset;
  sd_volume_last_read_length = length;
  sd_volume_last_read_result = 0;
  if (volume == NULL || volume->backing == NULL ||
      offset > volume->sectors || length > volume->sectors - offset) {
    return 0;
  }
  sd_volume_last_read_result =
      dev_bulk_read(volume->backing, buffer, volume->offset + offset, length);
  return sd_volume_last_read_result;
}

static int volume_write(
    struct device *device, void *buffer, uint32_t length, uint32_t offset) {
  h2_jieli_sd_volume_device_t *volume =
      (h2_jieli_sd_volume_device_t *)device;
  if (volume == NULL || volume->backing == NULL ||
      offset > volume->sectors || length > volume->sectors - offset) {
    return 0;
  }
  return dev_bulk_write(
      volume->backing, buffer, volume->offset + offset, length);
}

static int volume_ioctl(struct device *device, uint32_t command, uint32_t arg) {
  h2_jieli_sd_volume_device_t *volume =
      (h2_jieli_sd_volume_device_t *)device;
  ++sd_volume_ioctl_count;
  sd_volume_last_ioctl = command;
  sd_volume_last_ioctl_result = -1;
  if (volume == NULL || volume->backing == NULL) return -1;
  switch (command) {
    case IOCTL_GET_CAPACITY:
    case IOCTL_GET_BLOCK_NUMBER:
      *(uint32_t *)arg = volume->sectors;
      sd_volume_last_ioctl_result = 0;
      return 0;
    case IOCTL_GET_BLOCK_SIZE:
    case IOCTL_GET_SECTOR_SIZE:
      *(uint32_t *)arg = 512u;
      sd_volume_last_ioctl_result = 0;
      return 0;
    case IOCTL_GET_STATUS:
      *(uint32_t *)arg = 1u;
      sd_volume_last_ioctl_result = 0;
      return 0;
    default:
      sd_volume_last_ioctl_result =
          dev_ioctl(volume->backing, (int)command, arg);
      return sd_volume_last_ioctl_result;
  }
}

static int volume_close(struct device *device) {
  h2_jieli_sd_volume_device_t *volume =
      (h2_jieli_sd_volume_device_t *)device;
  if (volume == NULL || volume->backing == NULL) return 0;
  int result = dev_close(volume->backing);
  volume->backing = NULL;
  volume->offset = 0u;
  volume->sectors = 0u;
  return result;
}

const struct device_operations h2_jieli_ac791n_devkit_sd_volume_ops = {
    .online = volume_online,
    .init = volume_init,
    .open = volume_open,
    .read = volume_read,
    .write = volume_write,
    .ioctl = volume_ioctl,
    .close = volume_close,
};

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
      "fateq=%d fat54=%.5s vopen=%u/%d vread=%u@%u+%u/%d "
      "vioctl=%u:%u/%d",
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
      &sd_volume[54], (unsigned)sd_volume_open_count,
      sd_volume_open_result, (unsigned)sd_volume_read_count,
      (unsigned)sd_volume_last_read_offset,
      (unsigned)sd_volume_last_read_length, sd_volume_last_read_result,
      (unsigned)sd_volume_ioctl_count, (unsigned)sd_volume_last_ioctl,
      sd_volume_last_ioctl_result) >= 0
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
  /* JLFAT's frename API accepts only a native filename for the destination.
   * Keep Loader's public paths unchanged while mapping its transactional
   * files to 8.3 names that are safe for native rename. */
  if (strcmp(path, "/dl/update.tar.zlib.tmp") == 0) {
    suffix = "/dl/H2STAGE.TMP";
  } else if (strcmp(path, "/dl/update.tar.zlib.prev") == 0) {
    suffix = "/dl/H2PREV.BIN";
  } else if (strcmp(path, "/dl/update.tar.zlib") == 0) {
    suffix = "/dl/H2STAGE.BIN";
  } else if (strcmp(path, "/data/.h2loader-image.tmp") == 0) {
    suffix = "/data/H2IMG.TMP";
  } else if (strcmp(path, "/data/.h2loader-image-1") == 0) {
    suffix = "/data/H2IMG1.BIN";
  } else if (strcmp(path, "/data/.h2loader-image-2") == 0) {
    suffix = "/data/H2IMG2.BIN";
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
  if (root_len + suffix_len > H2_JIELI_SD_PATH_MAX) {
    return H2_PAL_ERR_NO_SPACE;
  }
  memcpy(out, H2_JIELI_SD_ROOT, root_len);
  memcpy(out + root_len, suffix + 1, suffix_len);
  return H2_PAL_OK;
}

static int ensure_directory(const char *path) {
  char folder[H2_JIELI_SD_PATH_MAX];
  size_t root_len = strlen(H2_JIELI_SD_ROOT);
  if (strncmp(path, H2_JIELI_SD_ROOT, root_len) != 0 ||
      strlen(path + root_len) + 1u > sizeof(folder)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (flen_dir(path) >= 0) return H2_PAL_OK;
  strcpy(folder, path + root_len - 1u);
  if (fmk_dir(H2_JIELI_SD_ROOT, folder, 0) == 0 || flen_dir(path) >= 0) {
    return H2_PAL_OK;
  }
  /* JLFAT reports an empty existing directory as a negative flen_dir() and
   * fmk_dir() then reports "already exists".  Directory creation is only a
   * preparatory, idempotent operation; the following file open is the
   * authoritative writable check. */
  return H2_PAL_OK;
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
  sd_last_stage = "fopen-enter";
  FILE *native = fopen_by_utf8(mapped, native_mode);
  sd_last_stage = "fopen-return";
  if (native == NULL) {
    return mode == H2_PAL_FS_OPEN_READ ? H2_PAL_ERR_NOT_FOUND
                                       : H2_PAL_ERR_IO;
  }
  h2_pal_fs_file_t *file = malloc(sizeof(*file));
  if (file == NULL) {
    fclose(native);
    return H2_PAL_ERR_NO_MEMORY;
  }
  file->native = native;
  file->write_offset = 0u;
  file->next_write_trace = 0u;
  *out_file = file;
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
  int result = fclose(file->native);
  if (trace) {
    h2_jieli_sd_fs_trace_write("close-return", file->write_offset, 0u, result);
  }
  file->native = NULL;
  free(file);
  return map_error(result);
}

static int fs_stat(void *user, const char *path, h2_pal_fs_stat_t *out_stat) {
  (void)user;
  char mapped[H2_JIELI_SD_PATH_MAX];
  if (out_stat == NULL) return H2_PAL_ERR_INVALID_ARG;
  int result = translate_path(path, mapped);
  if (result != H2_PAL_OK) return result;
  if (strcmp(path, "/dl") == 0 || strcmp(path, "/data") == 0) {
    long long size = flen_dir(mapped);
    if (size < 0) return H2_PAL_ERR_NOT_FOUND;
    *out_stat = (h2_pal_fs_stat_t){.size = (uint64_t)size, .is_dir = 1};
    return H2_PAL_OK;
  }
  FILE *file = fopen_by_utf8(mapped, "r");
  if (file == NULL) return H2_PAL_ERR_NOT_FOUND;
  uint32_t size = flen(file);
  fclose(file);
  *out_stat = (h2_pal_fs_stat_t){.size = size, .is_dir = 0};
  return H2_PAL_OK;
}

static int fs_clear(void *user, const char *path) {
  (void)user;
  char mapped[H2_JIELI_SD_PATH_MAX];
  int result = translate_path(path, mapped);
  if (result != H2_PAL_OK) return result;
  result = fdelete_dir(mapped);
  if (result != 0) return H2_PAL_ERR_IO;
  return ensure_directory(mapped);
}

static int fs_remove(void *user, const char *path) {
  (void)user;
  char mapped[H2_JIELI_SD_PATH_MAX];
  int result = translate_path(path, mapped);
  if (result != H2_PAL_OK) return result;
  FILE *file = fopen_by_utf8(mapped, "r");
  if (file == NULL) return H2_PAL_ERR_NOT_FOUND;
  /* Jieli's fdelete() removes the entry represented by the already-open
   * handle and closes that handle atomically.  Closing it first and then
   * resolving the path again through fdelete_by_name() can leave JLFAT
   * waiting on stale metadata after an interrupted stage write. */
  result = fdelete(file);
  /* fdelete() commits the directory mutation while closing the handle.  Do
   * not follow it with f_free_cache(): on WL82/JLFAT that whole-volume cache
   * flush can block forever after a previously interrupted write. */
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
  FILE *file = fopen_by_utf8(old_mapped, "r");
  if (file == NULL) return H2_PAL_ERR_NOT_FOUND;
  result = frename(file, new_name + 1);
  (void)fclose(file);
  if (result != 0 || f_free_cache(H2_JIELI_SD_ROOT) != 0) {
    return H2_PAL_ERR_IO;
  }
  file = fopen_by_utf8(new_mapped, "r");
  if (file == NULL) return H2_PAL_ERR_IO;
  (void)fclose(file);
  return H2_PAL_OK;
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
    if (fmount_exist(H2_JIELI_SD_MOUNT)) {
      sd_mounted = 1;
    } else {
      void *heap_probe = malloc(32768u);
      sd_heap_probe_32k = heap_probe != NULL;
      free(heap_probe);
      heap_probe = malloc(16384u);
      sd_heap_probe_16k = heap_probe != NULL;
      free(heap_probe);
      sd_last_stage = "mount";
      struct imount *mounted = mount(
          "sd0", H2_JIELI_SD_MOUNT,
          H2_JIELI_SD_FS_TYPE, H2_JIELI_SD_CACHE_COUNT, NULL);
      if (mounted == NULL) {
        if (sd_volume_device.backing != NULL) {
          (void)volume_close(&sd_volume_device.device);
        }
        capture_sd_diagnostic();
        return H2_PAL_ERR_IO;
      }
      sd_mounted = 1;
    }
    sd_last_stage = "mkdir_dl";
    if (ensure_directory(H2_JIELI_SD_ROOT "dl") != H2_PAL_OK) {
      (void)unmount(H2_JIELI_SD_MOUNT);
      sd_mounted = 0;
      return H2_PAL_ERR_IO;
    }
    sd_last_stage = "mkdir_data";
    if (ensure_directory(H2_JIELI_SD_ROOT "data") != H2_PAL_OK) {
      (void)unmount(H2_JIELI_SD_MOUNT);
      sd_mounted = 0;
      return H2_PAL_ERR_IO;
    }
  }
  sd_last_stage = "ready";
  *out_api = (h2_pal_fs_api_t){.user = NULL, .vtable = &vtable};
  return H2_PAL_OK;
}

h2_pal_result_t h2_jieli_ac791n_devkit_sd_fs_deinit(void) {
  if (!sd_mounted) return H2_PAL_OK;
  int result = unmount(H2_JIELI_SD_MOUNT);
  if (result == 0) sd_mounted = 0;
  return map_error(result);
}
