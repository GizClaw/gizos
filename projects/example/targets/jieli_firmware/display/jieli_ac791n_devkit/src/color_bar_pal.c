#include "app_config.h"

#include "asm/includes.h"
#include "asm/system_reset_reason.h"
#include "asm/wdt.h"

#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_ac791n_devkit_partitions.h"
#include "h2_jieli_wl82_platform_core.h"
#include "h2_loader_app_client.h"
#include "h2_loader_boot.h"
#include "jieli_h2loader_app_support.h"
#include "jieli_app_iostreamikcp.h"
#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/net/h2_pal_net.h"
#include "h2/pal/net/h2_pal_netif.h"

#include "device/device.h"
#include "device/ioctl_cmds.h"
#include "fs/fs.h"
#include "fs/sdfile.h"
#include "os/os_api.h"
#include "system/sys_common.h"
#include "system/timer.h"
#include "update/dual_bank_updata_api.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
  H2_LCD_MAX_WIDTH = 480,
  H2_AUDIO_PROBE_SAMPLES = 320,
  H2_DIAGNOSTIC_HEARTBEAT_MS = 5000,
};

static uint16_t color_line[H2_LCD_MAX_WIDTH];
static int display_result;
static int touch_result;
static h2_pal_periph_id_t active_key;
static int sd_online;
static uint32_t sd_blocks;
static uint32_t sd_block_size;
static int sd_fs_result = H2_PAL_ERR_UNAVAILABLE;
static int sd_rw_result = H2_PAL_ERR_UNAVAILABLE;
static int audio_info_result = H2_PAL_ERR_UNAVAILABLE;
static int pref_result = H2_PAL_ERR_UNAVAILABLE;
static int boot_info_result = H2_PAL_ERR_UNAVAILABLE;
static struct BootInfo boot_info;
static int16_t audio_probe_pcm[H2_AUDIO_PROBE_SAMPLES];
static unsigned wifi_scan_count;
static uint16_t trial_recovery_timer;
static h2_loader_app_client_t loader_client;
static h2_pal_firmware_info_t firmware_info;
static const h2_pal_touch_api_t *runtime_touch;
static const h2_pal_button_api_t *runtime_buttons;
static h2_pal_task_t *runtime_task;
static uint32_t next_boot_partition = H2_JIELI_PARTITION_APP;
static uint32_t boot_reset_reason;
static uint32_t assert_log_addr;
static h2_pal_fs_api_t loader_fs;

/* Image-specific launchers may replace the board diagnostic scene while
 * retaining its proven H2Loader trial, rollback and command transport flow. */
__attribute__((weak)) int h2_jieli_target_application_run(void) {
  return H2_PAL_ERR_UNSUPPORTED;
}

#define H2_JIELI_TRIAL_CHECKSUM_KEY "jieli_trial_checksum"
#define H2_JIELI_TRIAL_RESET_REASON_KEY "jieli_trial_reset_reason"

extern int vsnprintf(
    char *buffer, size_t size, const char *format, va_list arguments);
extern int snprintf(char *buffer, size_t size, const char *format, ...);
extern const char *os_current_task_rom(void);
/* WL82's production allocator lives in libsystem.a.  These are its exported
 * dlmalloc-compatible counters; the FreeRTOS names in system/malloc.h belong
 * to the optional mem_heap.c allocator and are not linked by this target. */
extern uint32_t get_malloc_remain_heap_size(void);
extern size_t malloc_max_footprint(void);
extern uint8_t HEAP_BEGIN;
extern uint8_t HEAP_END;

static int prepare_destructive_app_return(void *user);
static void usb_write_status(const char *format, ...);

static int app_power_get_running(
    void *user, h2_pal_power_boot_partition_t *out_partition) {
  (void)user;
  if (out_partition == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_partition, 0, sizeof(*out_partition));
  out_partition->id = H2_JIELI_PARTITION_APP;
  out_partition->flags = H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE |
                         H2_PAL_POWER_BOOT_PARTITION_FLAG_RUNNING |
                         H2_PAL_POWER_BOOT_PARTITION_FLAG_APP;
  memcpy(out_partition->name, "app", 4u);
  return H2_PAL_OK;
}

static int app_power_get_next(
    void *user, h2_pal_power_boot_partition_t *out_partition) {
  (void)user;
  if (out_partition == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_partition, 0, sizeof(*out_partition));
  out_partition->id = next_boot_partition;
  out_partition->flags = H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE |
                         H2_PAL_POWER_BOOT_PARTITION_FLAG_NEXT |
      (next_boot_partition == H2_JIELI_PARTITION_LOADER
           ? H2_PAL_POWER_BOOT_PARTITION_FLAG_RECOVERY
           : H2_PAL_POWER_BOOT_PARTITION_FLAG_APP);
  (void)snprintf(out_partition->name, sizeof(out_partition->name), "%s",
                 next_boot_partition == H2_JIELI_PARTITION_LOADER
                     ? "h2loader" : "app");
  return H2_PAL_OK;
}

static int app_power_set_next(void *user, uint32_t partition_id) {
  (void)user;
  if (partition_id != H2_JIELI_PARTITION_LOADER &&
      partition_id != H2_JIELI_PARTITION_APP) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  next_boot_partition = partition_id;
  return H2_PAL_OK;
}

static int app_power_reboot(void *user, uint32_t reason) {
  (void)user;
  usb_write_status(
      "JIELI_APP_REBOOT source=power_api reason=%u next=%u\r\n",
      (unsigned)reason, (unsigned)next_boot_partition);
  os_time_dly(10u);
  if (next_boot_partition == H2_JIELI_PARTITION_LOADER &&
      flash_update_clr_boot_info(CLEAR_APP_RUNNING_BANK) != 0) {
    return H2_PAL_ERR_IO;
  }
  system_reset();
  return H2_PAL_OK;
}

static const h2_pal_power_api_t *app_power_api(void) {
  static const h2_pal_power_vtable_t vtable = {
      .get_running_boot_partition = app_power_get_running,
      .get_next_boot_partition = app_power_get_next,
      .set_next_boot_partition = app_power_set_next,
      .reboot = app_power_reboot,
  };
  static const h2_pal_power_api_t api = {.vtable = &vtable};
  return &api;
}

static int enter_trial_boot(void) {
  const h2_pal_pref_api_t *pref = h2_jieli_ac791n_devkit_pref_api();
  const h2_pal_mem_api_t *allocator = h2_jieli_wl82_platform_mem_api();
  h2_loader_status_t status;
  int result = h2_loader_read_pref_status(pref, allocator, &status);
  if (result != H2_PAL_OK) return result;
  if (!status.stage.valid || !status.partition_2.valid ||
      status.partition_2.role != H2_LOADER_IMAGE_ROLE_APP ||
      !h2_loader_metadata_image_equal(&status.stage, &status.partition_2)) {
    return H2_PAL_OK;
  }
  if (status.partition_2.image_checksum[0] == '\0') {
    return H2_PAL_ERR_INVALID_STATE;
  }

  h2_pal_pref_namespace_t *name_space = NULL;
  result = h2_pal_pref_open(
      pref, H2_LOADER_PREF_NAMESPACE, H2_PAL_PREF_OPEN_READ_WRITE,
      &name_space);
  char *previous_checksum = NULL;
  int repeated_trial = 0;
  if (result == H2_PAL_OK) {
    result = name_space->get_string(
        name_space, allocator, H2_JIELI_TRIAL_CHECKSUM_KEY,
        &previous_checksum);
    if (result == H2_PAL_ERR_NOT_FOUND) result = H2_PAL_OK;
  }
  if (result == H2_PAL_OK && previous_checksum != NULL) {
    repeated_trial =
        strcmp(previous_checksum, status.partition_2.image_checksum) == 0;
  }
  if (previous_checksum != NULL) {
    h2_pal_mem_free(allocator, previous_checksum);
  }
  if (result == H2_PAL_OK && !repeated_trial) {
    result = name_space->set_string(
        name_space, H2_JIELI_TRIAL_CHECKSUM_KEY,
        status.partition_2.image_checksum);
  }
  if (result == H2_PAL_OK && repeated_trial) {
    result = name_space->set_u32(
        name_space, H2_JIELI_TRIAL_RESET_REASON_KEY,
        boot_reset_reason);
  }
  if (result == H2_PAL_OK && name_space->commit != NULL) {
    result = name_space->commit(name_space);
  }
  if (name_space != NULL && name_space->close != NULL) {
    int close_result = name_space->close(name_space);
    if (result == H2_PAL_OK) result = close_result;
  }
  if (result != H2_PAL_OK || !repeated_trial) return result;

  if (flash_update_clr_boot_info(CLEAR_APP_RUNNING_BANK) != 0) {
    return H2_PAL_ERR_IO;
  }
  system_reset();
  return H2_PAL_ERR_INVALID_STATE;
}

static int prepare_destructive_app_return(void *user) {
  const h2_pal_pref_api_t *pref = user;
  h2_pal_pref_namespace_t *name_space = NULL;
  h2_pal_fs_api_t fs;
  static const char *const installed_keys[] = {
      "installed_version", "installed_checksum", "installed_size",
  };
  int result = h2_jieli_ac791n_devkit_sd_fs_init(&fs);
  if (result == H2_PAL_OK) {
    result = h2_pal_fs_remove(&fs, "/data/.h2loader-image-2");
    if (result == H2_PAL_ERR_NOT_FOUND) result = H2_PAL_OK;
  }
  if (result != H2_PAL_OK) return result;
  result = h2_pal_pref_open(
      pref, H2_LOADER_PREF_NAMESPACE, H2_PAL_PREF_OPEN_READ_WRITE,
      &name_space);
  if (result != H2_PAL_OK) return result;
  if (name_space == NULL || name_space->remove == NULL ||
      name_space->set_bool == NULL || name_space->commit == NULL) {
    result = H2_PAL_ERR_UNSUPPORTED;
  }
  for (size_t index = 0u;
       result == H2_PAL_OK && index < sizeof(installed_keys) / sizeof(installed_keys[0]);
       ++index) {
    result = name_space->remove(name_space, installed_keys[index]);
    if (result == H2_PAL_ERR_NOT_FOUND) result = H2_PAL_OK;
  }
  if (result == H2_PAL_OK) {
    result = name_space->set_bool(name_space, "app_confirmed", 0);
  }
  if (result == H2_PAL_OK) result = name_space->commit(name_space);
  int close_result = name_space != NULL && name_space->close != NULL
                         ? name_space->close(name_space)
                         : H2_PAL_OK;
  return result == H2_PAL_OK ? close_result : result;
}

static int app_return_to_loader(void) {
  return h2_loader_reboot_h2loader_with_transition(
      &loader_client.loader, NULL, NULL);
}

static void return_to_loader(void *user) {
  (void)user;
  trial_recovery_timer = 0u;
  usb_write_status("JIELI_TRIAL_TIMER state=fired uptime_ms=%u\r\n",
                   (unsigned)timer_get_ms());
  int result = app_return_to_loader();
  usb_write_status("JIELI_TRIAL_TIMER state=returned result=%d\r\n", result);
}

static void usb_write_status(const char *format, ...) {
  /* Formatting must also be per-call: the timer and runtime can log at once. */
  char status_line[320];
  va_list arguments;
  va_start(arguments, format);
  int length = vsnprintf(status_line, sizeof(status_line), format, arguments);
  va_end(arguments);
  if (length > 0 && (size_t)length < sizeof(status_line)) {
#if defined CONFIG_H2_UART1_DEBUG_ENABLE
    /* Boot diagnostics cannot depend on the App command task already running. */
    (void)h2_jieli_ac791n_devkit_console_write(
        status_line, (size_t)length, 100u);
#else
    (void)h2_jieli_app_iostreamikcp_log(status_line, (size_t)length);
#endif
  }
}

void h2_jieli_wl82_boot_probe(uint32_t stage) {
  usb_write_status("JIELI_BOOT_STAGE stage=%u\r\n", (unsigned)stage);
}

void h2_jieli_sd_fs_trace_write(
    const char *stage, size_t offset, size_t length, int result) {
  usb_write_status(
      "JIELI_APP_SD_WRITE phase=%s offset=%u length=%u result=%d\r\n",
      stage, (unsigned)offset, (unsigned)length, result);
}

static void report_previous_exception(void) {
  FILE *file = fopen("mnt/sdfile/EXT_RESERVED/log", "r");
  if (file == NULL) {
    usb_write_status("JIELI_EXCEPTION_PROBE stage=open result=-1\r\n");
    return;
  }
  struct vfs_attr attributes;
  memset(&attributes, 0, sizeof(attributes));
  int result = fget_attrs(file, &attributes);
  (void)fclose(file);
  if (result != 0 || attributes.sclust == 0u) {
    usb_write_status(
        "JIELI_EXCEPTION_PROBE stage=attrs result=%d address=0x%x\r\n",
        result, (unsigned)attributes.sclust);
    return;
  }
  assert_log_addr = attributes.sclust;

  char exception_log[641];
  memset(exception_log, 0, sizeof(exception_log));
  result = sdfile_reserve_zone_read(
      exception_log, attributes.sclust, 640u, 0);
  exception_log[640] = '\0';
  usb_write_status(
      "JIELI_EXCEPTION_PROBE stage=read result=%d address=0x%x first=0x%x\r\n",
      result, (unsigned)attributes.sclust, (unsigned)(uint8_t)exception_log[0]);
  if (result != 640 || (uint8_t)exception_log[0] == UINT8_C(0xff)) return;
  usb_write_status(
      "JIELI_EXCEPTION_LOG address=0x%x read=%d\r\n",
      (unsigned)attributes.sclust, result);
  size_t remaining = strnlen(exception_log, 640u);
  size_t offset = 0u;
  while (offset < remaining) {
    size_t chunk = remaining - offset;
    if (chunk > 120u) chunk = 120u;
    (void)h2_jieli_app_iostreamikcp_log(exception_log + offset, chunk);
    offset += chunk;
  }
  (void)h2_jieli_app_iostreamikcp_log("\r\n", 2u);
  (void)sdfile_reserve_zone_erase(
      attributes.sclust, SDFILE_SECTOR_SIZE, 0);
}

static int draw_color_bars(const h2_pal_display_api_t *display) {
  static const uint16_t colors[] = {
      UINT16_C(0xffff), UINT16_C(0xffe0), UINT16_C(0x07ff),
      UINT16_C(0x07e0), UINT16_C(0xf81f), UINT16_C(0xf800),
      UINT16_C(0x001f), UINT16_C(0x0000),
  };
  h2_display_info_t info = {0};
  int info_result = h2_pal_display_get_info(display, &info);
  if (info_result != H2_DISPLAY_OK) return info_result;
  if (info.width == 0 || info.width > H2_LCD_MAX_WIDTH || info.height == 0) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  for (uint32_t x = 0u; x < info.width; ++x) {
    color_line[x] = colors[x * 8u / info.width];
  }
  for (int y = 0; y < info.height; ++y) {
    h2_display_rect_t rect = {.x = 0, .y = y, .width = info.width, .height = 1};
    int result = h2_pal_display_draw_bitmap(
        display, &rect, color_line, sizeof(color_line),
        H2_DISPLAY_PIXEL_RGB565);
    if (result != H2_DISPLAY_OK) return result;
  }
  return h2_pal_display_present(display);
}

static int probe_sd_filesystem(void) {
  static const char path[] = "/data/H2PROBE.TXT";
  static const uint8_t expected[] = "h2-jieli-ac791n-sd-ok\n";
  uint8_t actual[sizeof(expected)];
  h2_pal_fs_api_t fs;
  h2_pal_fs_file_t *file = NULL;
  size_t transferred = 0u;

  sd_fs_result = h2_jieli_ac791n_devkit_sd_fs_init(&fs);
  if (sd_fs_result != H2_PAL_OK) return sd_fs_result;
  uint32_t total_kib = 0u;
  if (fget_physical_total_space("storage/sd0/C/", &total_kib) == 0) {
    sd_blocks = total_kib;
    sd_block_size = 1024u;
  }

  int result = h2_pal_fs_open(
      &fs, path, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file);
  if (result == H2_PAL_OK) {
    result = h2_pal_fs_write(
        &fs, file, expected, sizeof(expected), &transferred);
  }
  if (result == H2_PAL_OK && transferred != sizeof(expected)) {
    result = H2_PAL_ERR_IO;
  }
  if (result == H2_PAL_OK) result = h2_pal_fs_sync(&fs, file);
  if (file != NULL) {
    int close_result = h2_pal_fs_close(&fs, file);
    file = NULL;
    if (result == H2_PAL_OK) result = close_result;
  }
  if (result != H2_PAL_OK) return result;

  result = h2_pal_fs_open(&fs, path, H2_PAL_FS_OPEN_READ, &file);
  if (result == H2_PAL_OK) {
    transferred = 0u;
    memset(actual, 0, sizeof(actual));
    result = h2_pal_fs_read(&fs, file, actual, sizeof(actual), &transferred);
  }
  if (result == H2_PAL_OK &&
      (transferred != sizeof(expected) ||
       memcmp(actual, expected, sizeof(expected)) != 0)) {
    result = H2_PAL_ERR_IO;
  }
  if (file != NULL) {
    int close_result = h2_pal_fs_close(&fs, file);
    if (result == H2_PAL_OK) result = close_result;
  }
  return result;
}

static h2_pal_result_t jieli_memory_stats_read(
    void *user, h2_loader_memory_stats_t *out_stats) {
  (void)user;
  if (out_stats == NULL) return H2_PAL_ERR_INVALID_ARG;
  size_t total = (size_t)(&HEAP_END - &HEAP_BEGIN);
  size_t peak = malloc_max_footprint();
  *out_stats = (h2_loader_memory_stats_t){
      .internal = {
          .total_bytes = total,
          .free_bytes = get_malloc_remain_heap_size(),
          .minimum_free_bytes = peak < total ? total - peak : 0u,
          /* The WL82 allocator does not expose its largest free block. */
          .largest_free_block_bytes = 0u,
      },
  };
  return H2_PAL_OK;
}

static void probe_audio(void) {
  const h2_pal_audio_api_t *audio = h2_jieli_ac791n_devkit_audio_api();
  h2_audio_info_t info;
  int mic_start = H2_AUDIO_ERR_UNAVAILABLE;
  int mic_read = H2_AUDIO_ERR_UNAVAILABLE;
  int mic_stop = H2_AUDIO_ERR_UNAVAILABLE;
  int speaker_start = H2_AUDIO_ERR_UNAVAILABLE;
  int track_create = H2_AUDIO_ERR_UNAVAILABLE;
  int track_write = H2_AUDIO_ERR_UNAVAILABLE;
  int track_drain = H2_AUDIO_ERR_UNAVAILABLE;
  int track_close = H2_AUDIO_ERR_UNAVAILABLE;
  int speaker_stop = H2_AUDIO_ERR_UNAVAILABLE;
  int peak = 0;

  audio_info_result = h2_pal_audio_get_info(audio, &info);
  mic_start = h2_pal_audio_start_mic(audio);
  if (mic_start == H2_AUDIO_OK) {
    h2_audio_frame_t frame = h2_audio_frame_for_buffer(
        audio_probe_pcm, sizeof(audio_probe_pcm), info.mic_format);
    mic_read = h2_pal_audio_mic_read(audio, &frame, 1000u);
    if (mic_read == H2_AUDIO_OK) {
      for (size_t index = 0u; index < frame.bytes / sizeof(int16_t); ++index) {
        int sample = audio_probe_pcm[index];
        if (sample < 0) sample = -sample;
        if (sample > peak) peak = sample;
      }
    }
    mic_stop = h2_pal_audio_stop_mic(audio);
  }

  speaker_start = h2_pal_audio_start_speaker(audio);
  if (speaker_start == H2_AUDIO_OK) {
    const h2_audio_track_config_t config = {
        .name = "board-probe",
        .format = info.playback_format,
        .volume_factor_milli = 500u,
        .buffer_frames = 4u,
    };
    h2_pal_audio_track_t *track = NULL;
    track_create = h2_pal_audio_create_track(audio, &config, &track);
    if (track_create == H2_AUDIO_OK) {
      for (size_t index = 0u; index < H2_AUDIO_PROBE_SAMPLES; ++index) {
        audio_probe_pcm[index] = (index % 32u) < 16u ? 5000 : -5000;
      }
      h2_audio_frame_t frame = h2_audio_frame_for_buffer(
          audio_probe_pcm, sizeof(audio_probe_pcm), info.playback_format);
      frame.bytes = sizeof(audio_probe_pcm);
      for (unsigned repeat = 0u; repeat < 10u; ++repeat) {
        track_write = h2_pal_audio_track_write(track, &frame, 1000u);
        if (track_write != H2_AUDIO_OK) break;
      }
      if (track_write == H2_AUDIO_OK) {
        track_drain = h2_pal_audio_track_drain(track, 1000u);
      }
      track_close = h2_pal_audio_track_close(track);
    }
    speaker_stop = h2_pal_audio_stop_speaker(audio);
  }
  usb_write_status(
      "JIELI_AUDIO info=%d mic=%d/%d/%d/%d spk=%d/%d/%d/%d/%d/%d\r\n",
      audio_info_result, mic_start, mic_read, peak, mic_stop, speaker_start,
      track_create, track_write, track_drain, track_close, speaker_stop);
}

static void probe_pref(void) {
  h2_pal_pref_namespace_t *name_space = NULL;
  uint32_t value = 0u;
  pref_result = h2_pal_pref_open(
      h2_jieli_ac791n_devkit_pref_api(), "board-probe",
      H2_PAL_PREF_OPEN_READ_WRITE, &name_space);
  if (pref_result == H2_PAL_OK) {
    int read_result = name_space->get_u32(name_space, "boot-count", &value);
    if (read_result == H2_PAL_ERR_NOT_FOUND) value = 0u;
    if (read_result != H2_PAL_OK && read_result != H2_PAL_ERR_NOT_FOUND) {
      pref_result = read_result;
    }
  }
  if (pref_result == H2_PAL_OK) {
    pref_result = name_space->set_u32(name_space, "boot-count", value + 1u);
  }
  if (pref_result == H2_PAL_OK) pref_result = name_space->commit(name_space);
  if (pref_result == H2_PAL_OK) {
    pref_result = name_space->get_u32(name_space, "boot-count", &value);
  }
  if (name_space != NULL) {
    int close_result = name_space->close(name_space);
    if (pref_result == H2_PAL_OK) pref_result = close_result;
  }
  usb_write_status(
      "JIELI_PREF result=%d count=%u area=0x700000+0x40000\r\n",
      pref_result, (unsigned)value);
}

static bool report_wifi_scan(
    void *user, const h2_pal_wifi_scan_entry_t *entry) {
  (void)user;
  ++wifi_scan_count;
  usb_write_status(
      "JIELI_WIFI_SCAN i=%u ssid=%.*s ch=%u rssi=%d sec=%u\r\n",
      wifi_scan_count, (int)entry->ssid_len, entry->ssid,
      (unsigned)entry->channel, entry->rssi, (unsigned)entry->security);
  return true;
}

static void __attribute__((unused)) probe_wifi(void) {
  const h2_pal_wifi_sta_api_t *wifi =
      h2_jieli_ac791n_devkit_wifi_sta_api();
  uint8_t mac[6] = {0};
  h2_pal_wifi_sta_status_t status;
  memset(&status, 0, sizeof(status));
  int mac_result = h2_pal_wifi_sta_get_mac(wifi, mac);
  int status_result = h2_pal_wifi_sta_get_status(wifi, &status);
  int has_saved = 0;
  int settings_result = h2_pal_wifi_settings_has_saved_sta_config(
      h2_jieli_ac791n_devkit_wifi_settings_api(), &has_saved);
  wifi_scan_count = 0u;
  int scan_result =
      h2_pal_wifi_sta_scan(wifi, NULL, report_wifi_scan, NULL, 15000u);
  usb_write_status(
      "JIELI_WIFI mac=%d/%02x:%02x:%02x:%02x:%02x:%02x "
      "state=%d/%u saved=%d/%d scan=%d/%u\r\n",
      mac_result, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
      status_result, (unsigned)status.state, settings_result, has_saved,
      scan_result, wifi_scan_count);
}

static void __attribute__((unused)) probe_net(void) {
  const h2_pal_net_api_t *net = h2_jieli_ac791n_devkit_net_api();
  h2_pal_net_addr_t host;
  h2_pal_net_addr_t bound;
  h2_pal_net_socket_t socket_fd = -1;
  memset(&host, 0, sizeof(host));
  memset(&bound, 0, sizeof(bound));
  int host_result = h2_pal_net_get_host_addr(net, "wifi", &host);
  int udp_result = h2_pal_net_udp_open_bound(
      net, H2_PAL_NET_FAMILY_IPV4, 0u, NULL, &socket_fd, &bound);
  if (socket_fd >= 0) h2_pal_net_close(net, socket_fd);
  h2_pal_netif_status_t netif_status;
  memset(&netif_status, 0, sizeof(netif_status));
  int netif_result = h2_pal_netif_get_status(
      h2_jieli_ac791n_devkit_netif_api(), NULL, &netif_status);
  usb_write_status(
      "JIELI_NET host=%d/%u.%u.%u.%u udp=%d/%u netif=%d/0x%x dns=%u\r\n",
      host_result, host.ip[0], host.ip[1], host.ip[2], host.ip[3],
      udp_result, (unsigned)bound.port, netif_result,
      (unsigned)netif_status.flags, (unsigned)netif_status.dns_count);
}

static void report_status(void) {
  uint32_t sd_mib = sd_block_size == 0u
                        ? 0u
                        : (uint32_t)(((uint64_t)sd_blocks * sd_block_size) >> 20u);
  usb_write_status(
      "JIELI_PAL display=%d touch=%d key=8/%u sd=%d/%u/%d/%d "
      "audio=%d pref=%d\r\n",
      display_result, touch_result, (unsigned)active_key, sd_online,
      (unsigned)sd_mib, sd_fs_result, sd_rw_result, audio_info_result,
      pref_result);
  usb_write_status(
      "JIELI_BOOT_INF2 result=%d reset_reason=0x%x base=0x%x bytes=%u version=%u "
      "recovery=RECOVERY\r\n",
      boot_info_result, (unsigned)boot_reset_reason,
      (unsigned)boot_info.baseAddress,
      (unsigned)boot_info.codeLength, (unsigned)boot_info.version);
}

static void color_bar_runtime(void *user) {
  (void)user;
  uint32_t last_heartbeat_ms = timer_get_ms();
  for (;;) {
    h2_pal_touch_event_t touch_event;
    if (touch_result == H2_PAL_OK && runtime_touch != NULL &&
        h2_pal_touch_poll_event(runtime_touch, &touch_event) == H2_PAL_OK) {
      usb_write_status(
          "JIELI_TOUCH action=%u x=%d y=%d\r\n",
          (unsigned)touch_event.kind, (int)touch_event.x, (int)touch_event.y);
    }

    h2_pal_radio_button_group_reading_t keys;
    if (runtime_buttons != NULL &&
        h2_pal_button_read_radio_button_group(
            runtime_buttons, H2_JIELI_AC791N_ADKEY_GROUP_ID, &keys) ==
            H2_PAL_OK &&
        keys.pressed_button_id != active_key) {
      active_key = keys.pressed_button_id;
      usb_write_status("JIELI_ADKEY active=%u\r\n", (unsigned)active_key);
    }
    wdt_clear();
    uint32_t now_ms = timer_get_ms();
    if (now_ms - last_heartbeat_ms >= H2_DIAGNOSTIC_HEARTBEAT_MS) {
      last_heartbeat_ms = now_ms;
      usb_write_status(
          "JIELI_HEARTBEAT uptime_ms=%u heap_free=%u heap_peak=%u\r\n",
          (unsigned)now_ms, (unsigned)get_malloc_remain_heap_size(),
          (unsigned)malloc_max_footprint());
    }
    os_time_dly(1u);
  }
}

static void fail_app_startup(const char *step, int result) {
  usb_write_status("JIELI_APP_INIT_FAILED step=%s result=%d action=rollback\r\n",
                   step, result);
  /* This path precedes client initialization. Do not call through its empty
   * config. Invalidate the trial bank; the common Loader detects rollback. */
  int rollback_result = flash_update_clr_boot_info(CLEAR_APP_RUNNING_BANK);
  usb_write_status("JIELI_APP_INIT_ROLLBACK result=%d\r\n", rollback_result);
  if (rollback_result == 0) {
    os_time_dly(10u);
    system_reset();
  }
  /* If the ROM-bank operation/reset fails, keep reporting the failure. */
  for (;;) {
    usb_write_status("JIELI_APP_INIT_FAILED step=%s result=%d rollback=%d\r\n",
                     step, result, rollback_result);
    os_time_dly(100u);
  }
}

void app_main(void) {
  usb_write_status("JIELI_APP_INIT step=enter\r\n");
  const h2_pal_display_api_t *display =
      h2_jieli_ac791n_devkit_display_api();
  const h2_pal_touch_api_t *touch = h2_jieli_ac791n_devkit_touch_api();
  const h2_pal_button_api_t *buttons = h2_jieli_ac791n_devkit_button_api();
  runtime_touch = touch;
  runtime_buttons = buttons;
  boot_reset_reason = system_reset_reason_get();
  usb_write_status("JIELI_APP_INIT step=trial-enter\r\n");
  int trial_result = enter_trial_boot();
  usb_write_status("JIELI_APP_INIT step=trial-return result=%d\r\n", trial_result);
  if (trial_result != H2_PAL_OK) {
    (void)flash_update_clr_boot_info(CLEAR_APP_RUNNING_BANK);
    system_reset();
    return;
  }
  int firmware_info_result = h2_pal_firmware_info_get_current(
      h2_jieli_wl82_platform_firmware_info_api(), &firmware_info);
  usb_write_status("JIELI_APP_INIT step=firmware-info result=%d\r\n", firmware_info_result);
  if (firmware_info_result != H2_PAL_OK) {
    const h2_pal_pref_api_t *pref = h2_jieli_ac791n_devkit_pref_api();
    (void)prepare_destructive_app_return((void *)pref);
    (void)flash_update_clr_boot_info(CLEAR_APP_RUNNING_BANK);
    system_reset();
    return;
  }
  memset(&loader_fs, 0, sizeof(loader_fs));
  usb_write_status("JIELI_APP_INIT step=fs-enter\r\n");
  int loader_result = h2_jieli_ac791n_devkit_sd_fs_init(&loader_fs);
  usb_write_status("JIELI_APP_INIT step=fs-return result=%d\r\n", loader_result);
  if (loader_result != H2_PAL_OK) {
    fail_app_startup("fs", loader_result);
    return;
  }
  h2_loader_app_client_config_t loader_client_config;
  loader_result = h2_jieli_app_loader_config_init(
      &loader_client_config, &loader_fs, app_power_api(),
      (h2_loader_memory_stats_api_t){.read = jieli_memory_stats_read},
      H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI |
          H2_LOADER_CAPABILITY_BLE);
  usb_write_status("JIELI_APP_INIT step=loader-config result=%d\r\n", loader_result);
  if (loader_result != H2_PAL_OK) {
    fail_app_startup("loader-config", loader_result);
    return;
  }
  loader_result = h2_loader_app_client_init(
      &loader_client, &loader_client_config);
  usb_write_status("JIELI_APP_INIT step=loader-client result=%d\r\n", loader_result);
  if (loader_result != H2_PAL_OK) {
    fail_app_startup("loader-client", loader_result);
    return;
  }

  /* A newly-installed app is a trial until the host confirms that USB and
   * the board runtime are alive. If the test session disappears, discard the
   * running App bank and return to the previous loader bank automatically. */
  trial_recovery_timer = sys_timeout_add_to_task(
      "sys_timer", NULL, return_to_loader, 120000u);
  usb_write_status(
      "JIELI_TRIAL_TIMER state=armed id=%u delay_ms=120000\r\n",
      (unsigned)trial_recovery_timer);

  loader_result = h2_jieli_app_iostreamikcp_start(
      &loader_client,
      h2_jieli_wl82_platform_task_api(),
      h2_jieli_wl82_platform_mem_api());
  usb_write_status("JIELI_APP_INIT step=transport result=%d\r\n", loader_result);
  if (loader_result != H2_PAL_OK) {
    (void)app_return_to_loader();
    return;
  }
  report_previous_exception();

  /* This standalone launcher does not call h2_runtime_init(). Initialize
   * the shared event provider before BLE subscribes, as Runtime does. */
  loader_result = h2_pal_system_event_init(
      h2_jieli_wl82_platform_system_event_api());
  usb_write_status("JIELI_APP_INIT step=system-event result=%d\r\n", loader_result);
  if (loader_result != H2_PAL_OK) {
    (void)app_return_to_loader();
    return;
  }
  loader_result = h2_jieli_app_loader_ble_start(&loader_client.config);
  usb_write_status("JIELI_APP_INIT step=ble-transport result=%d\r\n", loader_result);
  if (loader_result != H2_PAL_OK) {
    (void)app_return_to_loader();
    return;
  }

  int target_result = h2_jieli_target_application_run();
  if (target_result != H2_PAL_ERR_UNSUPPORTED) {
    usb_write_status("JIELI_TARGET_APP result=%d\r\n", target_result);
    int confirm_result = target_result == H2_PAL_OK
                             ? h2_jieli_app_loader_confirm(
                                   &loader_client.config)
                             : H2_PAL_ERR_INVALID_STATE;
    usb_write_status(
        "JIELI_APP_CONFIRM result=%s code=%d target=%d transport=%d\r\n",
        confirm_result == H2_PAL_OK ? "OK" : "fail", confirm_result,
        target_result, loader_result);
    if (confirm_result == H2_PAL_OK && trial_recovery_timer != 0u) {
      uint16_t timer_to_delete = trial_recovery_timer;
      sys_timeout_del(trial_recovery_timer);
      trial_recovery_timer = 0u;
      usb_write_status(
          "JIELI_TRIAL_TIMER state=deleted id=%u\r\n",
          (unsigned)timer_to_delete);
    }
    return;
  }

  display_result = h2_pal_display_open(display);
  if (display_result == H2_DISPLAY_OK) {
    display_result = draw_color_bars(display);
  }
  touch_result = h2_pal_touch_open(touch);
  /* Do not open and close sd0 before mounting it.  On WL82 that tears down
   * the backing device immediately before JLFAT tries to claim it. */
  sd_online = dev_online("sd0") != 0;
  if (sd_online) {
    sd_rw_result = probe_sd_filesystem();
    if (sd_rw_result != H2_PAL_OK) {
      char diagnostic[320];
      memset(diagnostic, 0, sizeof(diagnostic));
      (void)h2_jieli_ac791n_devkit_sd_fs_diagnostic(
          diagnostic, sizeof(diagnostic));
      usb_write_status(
          "JIELI_SD_FAIL stage=%s code=%d diag=%s\r\n",
          h2_jieli_ac791n_devkit_sd_fs_last_stage(), sd_rw_result,
          diagnostic);
    }
  }
  probe_audio();
  probe_pref();
  memset(&boot_info, 0, sizeof(boot_info));
  boot_info_result = get_current_boot_info(&boot_info);
  report_status();

  const h2_pal_task_options_t runtime_options = {
      .name = "h2color/runtime",
      .min_stack_size = 8192u,
  };
  int runtime_result = h2_pal_task_start(
      h2_jieli_wl82_platform_task_api(), &runtime_options,
      color_bar_runtime, NULL, &runtime_task);
  usb_write_status("JIELI_RUNTIME_TASK result=%d\r\n", runtime_result);

  /* Match ESP/BK: confirmation belongs to the App and happens only after the
   * public command transport and the target's essential hardware are alive.
   * A failure before this point leaves the trial unconfirmed, so the timer
   * returns to the still-valid Loader bank. */
  int confirm_result = display_result == H2_DISPLAY_OK &&
                               runtime_result == H2_PAL_OK
                           ? h2_jieli_app_loader_confirm(
                                 &loader_client.config)
                           : H2_PAL_ERR_INVALID_STATE;
  usb_write_status(
      "JIELI_APP_CONFIRM result=%s code=%d display=%d transport=%d\r\n",
      confirm_result == H2_PAL_OK ? "OK" : "fail", confirm_result,
      display_result, loader_result);
  if (confirm_result == H2_PAL_OK && trial_recovery_timer != 0u) {
    uint16_t timer_to_delete = trial_recovery_timer;
    sys_timeout_del(trial_recovery_timer);
    trial_recovery_timer = 0u;
    usb_write_status(
        "JIELI_TRIAL_TIMER state=deleted id=%u\r\n",
        (unsigned)timer_to_delete);
  }

}
