#include "h2_esp_board_private.h"

#include "h2_esp_board_config.h"
#include "h2_esp_platform_safe_call.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_flash.h"
#include "esp_flash_spi_init.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "hal/spi_flash_types.h"

#include "h2_esp_board_extflash_recovery.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define H2_TIGA_ESP_EXTFLASH_BASE_PATH H2_ESP_BOARD_FS_BASE_PATH
#define H2_TIGA_ESP_EXTFLASH_DL_PATH H2_TIGA_ESP_EXTFLASH_BASE_PATH "/dl"
#define H2_TIGA_ESP_EXTFLASH_DATA_PATH H2_TIGA_ESP_EXTFLASH_BASE_PATH "/data"
#define H2_TIGA_ESP_EXTFLASH_PARTITION_LABEL "h2ext"
#define H2_TIGA_ESP_EXTFLASH_HOST SPI3_HOST
#define H2_TIGA_ESP_EXTFLASH_PATH_MAX 256u
#define H2_TIGA_ESP_EXTFLASH_TRANSFER_SIZE 4096u
#define H2_TIGA_ESP_EXTFLASH_SAFE_STACK_DEPTH 4096u

static const char* TAG = "h2_tiga_esp_extflash";
static esp_flash_t* s_extflash;
static const esp_partition_t* s_extflash_partition;
static uint32_t s_extflash_id;
static int s_extflash_bus_initialized;
static int s_extflash_mounted;

static void log_recovery_progress(const char* stage, size_t completed,
                                  size_t total) {
  const unsigned percent =
      total == 0u ? 0u : (unsigned)((uint64_t)completed * 100u / total);
  ESP_LOGW(TAG, "%s progress=%u%% bytes=%zu/%zu", stage, percent, completed,
           total);
}

static void log_scan_result(h2_extflash_format_t format,
                            const h2_extflash_probe_t* probe) {
  const h2_extflash_scan_info_t* scan = &probe->scan;
  const char* result = format == H2_EXTFLASH_BLANK ? "blank"
                       : format == H2_EXTFLASH_NON_LITTLEFS
                           ? "no-littlefs-marker"
                       : format == H2_EXTFLASH_LITTLEFS ? "littlefs-marker"
                                                        : "unknown";
  const char* action = format == H2_EXTFLASH_BLANK          ? "format-only"
                       : format == H2_EXTFLASH_NON_LITTLEFS ? "erase-and-format"
                                                            : "preserve";
  ESP_LOGW(TAG,
           "scan result=%s action=%s complete=%u bytes=%zu/%zu "
           "non_ff_bytes=%zu zero_bytes=%zu nonblank_blocks=%zu ranges=%zu",
           result, action,
           (unsigned)(scan->scanned_bytes == probe->partition->size &&
                      probe->read_error == ESP_OK),
           scan->scanned_bytes, (size_t)probe->partition->size,
           scan->non_ff_bytes, scan->zero_bytes, scan->nonblank_blocks,
           scan->nonblank_ranges);
  if (scan->non_ff_bytes != 0u) {
    ESP_LOGW(TAG, "scan nonblank first=0x%zx last=0x%zx",
             scan->first_non_ff_offset, scan->last_non_ff_offset);
  }
  size_t logged_ranges = scan->nonblank_ranges;
  if (logged_ranges > H2_EXTFLASH_SCAN_RANGE_LIMIT) {
    logged_ranges = H2_EXTFLASH_SCAN_RANGE_LIMIT;
  }
  for (size_t i = 0u; i < logged_ranges; ++i) {
    ESP_LOGW(TAG, "scan nonblank-block-range index=%zu offset=0x%zx size=%zu",
             i + 1u, scan->ranges[i].offset, scan->ranges[i].length);
  }
  if (scan->nonblank_ranges > logged_ranges) {
    ESP_LOGW(TAG, "scan nonblank-block-ranges omitted=%zu",
             scan->nonblank_ranges - logged_ranges);
  }
  if (format == H2_EXTFLASH_LITTLEFS) {
    ESP_LOGW(TAG, "scan littlefs-marker offset=0x%zx action=preserve",
             scan->littlefs_marker_offset);
  }
  if (probe->read_error != ESP_OK) {
    ESP_LOGW(TAG, "scan failed offset=0x%zx reason=%s error=%s action=preserve",
             probe->failure_offset,
             probe->read_mismatch ? "read-mismatch" : "read-error",
             esp_err_to_name(probe->read_error));
  }
}

static int log_filesystem_usage(const char* stage) {
  size_t total = 0u;
  size_t used = 0u;
  esp_err_t err =
      esp_littlefs_partition_info(s_extflash_partition, &total, &used);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "%s info failed: %s", stage, esp_err_to_name(err));
    return H2_PAL_FS_ERR_IO;
  }
  ESP_LOGI(TAG, "%s id=%" PRIu32 " total=%zu used=%zu free=%zu", stage,
           s_extflash_id, total, used, total >= used ? total - used : 0u);
  return H2_PAL_FS_OK;
}

static int mount_filesystem(void) {
  uint8_t* scratch = NULL;
  size_t scratch_capacity = 0u;
  h2_pal_result_t safe_rc =
      h2_esp_platform_safe_io_acquire(&scratch, &scratch_capacity);
  if (safe_rc != H2_PAL_OK || scratch == NULL ||
      scratch_capacity < 2u * H2_EXTFLASH_SCAN_BYTES) {
    if (safe_rc == H2_PAL_OK) {
      h2_esp_platform_safe_io_release();
    }
    ESP_LOGE(TAG, "safe I/O scratch unavailable: %d", safe_rc);
    return H2_PAL_FS_ERR_IO;
  }
  h2_extflash_mount_call_t call = {
      .partition = s_extflash_partition,
      .scratch = scratch,
      .base_path = H2_TIGA_ESP_EXTFLASH_BASE_PATH,
      .mount_error = ESP_FAIL,
      .scan_error = ESP_OK,
      .erase_error = ESP_FAIL,
      .format_error = ESP_FAIL,
      .remount_error = ESP_FAIL,
      .progress = log_recovery_progress,
      .scan_report = log_scan_result,
  };
  safe_rc = h2_esp_platform_safe_call(mount_safe_callback, &call, sizeof(call),
                                      H2_TIGA_ESP_EXTFLASH_SAFE_STACK_DEPTH);
  h2_esp_platform_safe_io_release();
  if (safe_rc != H2_PAL_OK) {
    ESP_LOGE(TAG, "mount SafeCall failed: %d", safe_rc);
    return H2_PAL_FS_ERR_IO;
  }
  if (call.recovery_started) {
    ESP_LOGW(
        TAG,
        "mount failed (%s); recovery source=%s erase=%s format=%s remount=%s",
        esp_err_to_name(call.mount_error),
        call.detected_format == H2_EXTFLASH_NON_LITTLEFS ? "no-littlefs-marker"
                                                         : "blank",
        call.detected_format == H2_EXTFLASH_NON_LITTLEFS
            ? esp_err_to_name(call.erase_error)
            : "SKIP",
        call.erase_error == ESP_OK ? esp_err_to_name(call.format_error)
                                   : "SKIP",
        call.erase_error == ESP_OK && call.format_error == ESP_OK
            ? esp_err_to_name(call.remount_error)
            : "SKIP");
  }
  if (call.mounted) {
    s_extflash_mounted = 1;
    return log_filesystem_usage(call.provisioned ? "provisioned" : "mounted");
  }
  if (call.scan_error != ESP_OK) {
    ESP_LOGE(TAG, "format probe read failed or inconsistent at offset=%zu: %s",
             call.scan_failure_offset, esp_err_to_name(call.scan_error));
  }
  if (!call.recovery_started) {
    ESP_LOGE(TAG, "mount failed: %s reason=%s (partition left untouched)",
             esp_err_to_name(call.mount_error),
             call.detected_format == H2_EXTFLASH_LITTLEFS
                 ? "littlefs-marker-found"
             : call.mount_error != ESP_FAIL ? "mount-error"
                                            : "probe-failed");
    return H2_PAL_FS_ERR_IO;
  }
  if (call.erase_error != ESP_OK) {
    ESP_LOGE(TAG, "extflash erase failed: %s",
             esp_err_to_name(call.erase_error));
    return H2_PAL_FS_ERR_IO;
  }
  if (call.format_error != ESP_OK) {
    ESP_LOGE(TAG, "provisioning format failed after mount error %s: %s",
             esp_err_to_name(call.mount_error),
             esp_err_to_name(call.format_error));
    return H2_PAL_FS_ERR_IO;
  }
  if (call.remount_error != ESP_OK) {
    ESP_LOGE(TAG, "provisioning remount failed: %s",
             esp_err_to_name(call.remount_error));
    return H2_PAL_FS_ERR_IO;
  }
  return H2_PAL_FS_ERR_IO;
}

static int ensure_directory(const char* path) {
  if (mkdir(path, 0775) == 0 || errno == EEXIST) {
    return H2_PAL_FS_OK;
  }
  ESP_LOGE(TAG, "mkdir %s failed: errno=%d", path, errno);
  return H2_PAL_FS_ERR_IO;
}

static int init_device(void) {
  if (s_extflash_partition != NULL) {
    return H2_PAL_FS_OK;
  }

  const gpio_config_t gpio_cfg = {
      .pin_bit_mask = (1ULL << H2_TIGA_ESP_EXTFLASH_CS_GPIO) |
                      (1ULL << H2_TIGA_ESP_EXTFLASH_CLK_GPIO) |
                      (1ULL << H2_TIGA_ESP_EXTFLASH_MOSI_GPIO) |
                      (1ULL << H2_TIGA_ESP_EXTFLASH_MISO_GPIO),
      .mode = GPIO_MODE_DISABLE,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&gpio_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gpio reset failed: %s", esp_err_to_name(err));
    return H2_PAL_FS_ERR_IO;
  }

  const spi_bus_config_t bus_config = {
      .mosi_io_num = H2_TIGA_ESP_EXTFLASH_MOSI_GPIO,
      .miso_io_num = H2_TIGA_ESP_EXTFLASH_MISO_GPIO,
      .sclk_io_num = H2_TIGA_ESP_EXTFLASH_CLK_GPIO,
      .quadwp_io_num = GPIO_NUM_NC,
      .quadhd_io_num = GPIO_NUM_NC,
      .max_transfer_sz = H2_TIGA_ESP_EXTFLASH_TRANSFER_SIZE,
  };
  err = spi_bus_initialize(H2_TIGA_ESP_EXTFLASH_HOST, &bus_config,
                           SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
    return H2_PAL_FS_ERR_IO;
  }
  s_extflash_bus_initialized = 1;

  const esp_flash_spi_device_config_t flash_config = {
      .host_id = H2_TIGA_ESP_EXTFLASH_HOST,
      .cs_id = 0,
      .cs_io_num = H2_TIGA_ESP_EXTFLASH_CS_GPIO,
      .io_mode = SPI_FLASH_FASTRD,
      .freq_mhz = H2_TIGA_ESP_EXTFLASH_FREQUENCY_MHZ,
  };
  err = spi_bus_add_flash_device(&s_extflash, &flash_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "add flash device failed: %s", esp_err_to_name(err));
    return H2_PAL_FS_ERR_IO;
  }
  err = esp_flash_init(s_extflash);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "flash init failed: %s", esp_err_to_name(err));
    return H2_PAL_FS_ERR_IO;
  }

  err = esp_flash_read_id(s_extflash, &s_extflash_id);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "read flash id failed: %s", esp_err_to_name(err));
    return H2_PAL_FS_ERR_IO;
  }
  uint32_t flash_size = 0u;
  err = esp_flash_get_size(s_extflash, &flash_size);
  if (err != ESP_OK || flash_size == 0u) {
    ESP_LOGE(TAG, "read flash size failed: %s", esp_err_to_name(err));
    return H2_PAL_FS_ERR_IO;
  }
  err = esp_partition_register_external(
      s_extflash, 0u, flash_size, H2_TIGA_ESP_EXTFLASH_PARTITION_LABEL,
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS,
      &s_extflash_partition);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "register partition failed: %s", esp_err_to_name(err));
    return H2_PAL_FS_ERR_IO;
  }
  ESP_LOGW(TAG,
           "initialized id=0x%06" PRIx32 " size=%" PRIu32
           " mode=fast-read freq_mhz=%u",
           s_extflash_id, flash_size, H2_TIGA_ESP_EXTFLASH_FREQUENCY_MHZ);
  return H2_PAL_FS_OK;
}

int h2_esp_board_extflash_mount_data(void) {
  if (s_extflash_mounted) {
    return H2_PAL_FS_OK;
  }
  int rc = init_device();
  if (rc == H2_PAL_FS_OK) {
    rc = mount_filesystem();
  }
  if (rc == H2_PAL_FS_OK) {
    rc = ensure_directory(H2_TIGA_ESP_EXTFLASH_DL_PATH);
  }
  if (rc == H2_PAL_FS_OK) {
    rc = ensure_directory(H2_TIGA_ESP_EXTFLASH_DATA_PATH);
  }
  if (rc != H2_PAL_FS_OK) {
    (void)h2_esp_board_extflash_unmount_data();
  }
  return rc;
}

int h2_esp_board_extflash_unmount_data(void) {
  if (s_extflash_mounted) {
    esp_err_t err = esp_vfs_littlefs_unregister_partition(s_extflash_partition);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "unmount failed: %s", esp_err_to_name(err));
      return H2_PAL_FS_ERR_IO;
    }
    s_extflash_mounted = 0;
  }
  if (s_extflash_partition != NULL) {
    esp_err_t err = esp_partition_deregister_external(s_extflash_partition);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "deregister partition failed: %s", esp_err_to_name(err));
      return H2_PAL_FS_ERR_IO;
    }
    s_extflash_partition = NULL;
  }
  if (s_extflash != NULL) {
    esp_err_t err = spi_bus_remove_flash_device(s_extflash);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "remove flash device failed: %s", esp_err_to_name(err));
      return H2_PAL_FS_ERR_IO;
    }
    s_extflash = NULL;
    s_extflash_id = 0u;
  }
  if (s_extflash_bus_initialized) {
    esp_err_t err = spi_bus_free(H2_TIGA_ESP_EXTFLASH_HOST);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "free SPI bus failed: %s", esp_err_to_name(err));
      return H2_PAL_FS_ERR_IO;
    }
    s_extflash_bus_initialized = 0;
  }
  return H2_PAL_FS_OK;
}

static int clear_directory_contents(const char* path) {
  DIR* dir = opendir(path);
  if (dir == NULL) {
    return errno == ENOENT ? ensure_directory(path) : H2_PAL_FS_ERR_IO;
  }
  int rc = H2_PAL_FS_OK;
  struct dirent* entry;
  while (rc == H2_PAL_FS_OK && (entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    char child[H2_TIGA_ESP_EXTFLASH_PATH_MAX];
    int len = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (len < 0 || (size_t)len >= sizeof(child)) {
      rc = H2_PAL_FS_ERR_NO_SPACE;
      break;
    }
    struct stat status;
    if (stat(child, &status) != 0) {
      rc = H2_PAL_FS_ERR_IO;
    } else if (S_ISDIR(status.st_mode)) {
      rc = clear_directory_contents(child);
      if (rc == H2_PAL_FS_OK && rmdir(child) != 0) {
        rc = H2_PAL_FS_ERR_IO;
      }
    } else if (unlink(child) != 0) {
      rc = H2_PAL_FS_ERR_IO;
    }
  }
  if (closedir(dir) != 0 && rc == H2_PAL_FS_OK) {
    rc = H2_PAL_FS_ERR_IO;
  }
  if (rc != H2_PAL_FS_OK) {
    ESP_LOGE(TAG, "clear %s failed: errno=%d", path, errno);
  }
  return rc;
}

int h2_esp_board_storage_usage(size_t* out_total, size_t* out_used) {
  if (out_total == NULL || out_used == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_total = *out_used = 0u;
  if (!s_extflash_mounted || s_extflash_partition == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  if (esp_littlefs_partition_info(s_extflash_partition, out_total, out_used) !=
      ESP_OK) {
    *out_total = *out_used = 0u;
    return H2_PAL_FS_ERR_IO;
  }
  return H2_PAL_FS_OK;
}

int h2_esp_board_extflash_clear_data(void) {
  int rc = h2_esp_board_extflash_mount_data();
  if (rc != H2_PAL_FS_OK) {
    return rc;
  }
  rc = clear_directory_contents(H2_TIGA_ESP_EXTFLASH_DATA_PATH);
  return rc == H2_PAL_FS_OK ? log_filesystem_usage("cleared /data") : rc;
}
