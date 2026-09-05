#include "app_config.h"

#include "asm/includes.h"
#include "asm/system_reset_reason.h"
#include "asm/wdt.h"
#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_ac791n_devkit_partitions.h"
#include "h2_jieli_wl82_platform_core.h"
#include "h2_loader_app_client.h"
#include "h2_loader_boot.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_smoke_mp4_player.h"
#include "h2_tinyh264.h"
#include "jieli_h2loader_app_support.h"
#include "jieli_app_iostreamikcp.h"
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

#define H2_JIELI_TRIAL_CHECKSUM_KEY "jieli_trial_checksum"
#define H2_JIELI_TRIAL_RESET_REASON_KEY "jieli_trial_reset_reason"

static h2_loader_app_client_t loader_client;
static h2_pal_firmware_info_t firmware_info;
static h2_pal_task_t *runtime_task;
static uint16_t trial_recovery_timer;
static uint32_t next_boot_partition = H2_JIELI_PARTITION_APP;
static uint32_t boot_reset_reason;
static uint32_t assert_log_addr;
static h2_pal_fs_api_t mp4_fs;

/* This must be a link-time-overridable function rather than a weak const.
 * Defining a weak const in this translation unit lets LTO fold every check to
 * zero before the direct target's strong definition is resolved. */
__attribute__((weak)) int h2_jieli_mp4_is_direct_boot(void) {
  return 0;
}

typedef struct h2_jieli_mp4_boot_marker {
  char magic[8];
  uint32_t stage;
  int32_t result;
} h2_jieli_mp4_boot_marker_t;

static volatile h2_jieli_mp4_boot_marker_t last_boot_marker;

extern int vsnprintf(
    char *buffer, size_t size, const char *format, va_list arguments);
extern int snprintf(char *buffer, size_t size, const char *format, ...);
extern int printf(const char *format, ...);

static void boot_marker(uint32_t stage, int32_t result) {
  static const char magic[8] = {'H', '2', 'M', 'P', '4', 'D', 'B', 'G'};
  for (size_t index = 0u; index < sizeof(magic); ++index) {
    last_boot_marker.magic[index] = magic[index];
  }
  last_boot_marker.stage = stage;
  last_boot_marker.result = result;
}

/* Called by the repository-owned SDK init patch after board_early_init() has
 * registered the board, but before App initcalls and app_main().  Keep this
 * probe RAM-only: board Flash/Pref services are not safe in this phase. */
void h2_jieli_wl82_boot_probe(uint32_t stage) {
  boot_marker(stage, H2_PAL_OK);
  char line[80];
  int length = snprintf(line, sizeof(line),
                        "H2_JIELI_MP4_BOOT_STAGE stage=%u\r\n",
                        (unsigned)stage);
  if (length > 0 && (size_t)length < sizeof(line)) {
    (void)h2_jieli_ac791n_devkit_console_write(
        line, (size_t)length, 100u);
  }
}

extern const char *os_current_task_rom(void);
extern uint32_t get_malloc_remain_heap_size(void);
extern size_t malloc_max_footprint(void);
extern uint8_t HEAP_BEGIN;
extern uint8_t HEAP_END;
extern const h2_pal_audio_api_t *h2_pal_unsupported_audio_api(void);
extern const h2_pal_audio_decoder_api_t *h2_linux_fdk_aac_decoder_api(void);

static int prepare_app_return(void *user);

static void emit(const char *format, ...) {
  /* Each call owns its formatting buffer and the board console serializes the
   * complete line.  Early boot diagnostics must not depend on the App command
   * transport, which is initialized later in app_main(). */
  char status_line[320];
  va_list arguments;
  va_start(arguments, format);
  int length = vsnprintf(status_line, sizeof(status_line), format, arguments);
  va_end(arguments);
  if (length > 0 && (size_t)length < sizeof(status_line)) {
    (void)h2_jieli_ac791n_devkit_console_write(
        status_line, (size_t)length, 100u);
  }
}

static int usb_log_write(
    void *user, h2_pal_log_level_t level, const char *scope,
    const char *message) {
  (void)user;
  char level_char = level == H2_PAL_LOG_ERROR ? 'E' :
                    level == H2_PAL_LOG_WARN ? 'W' :
                    level == H2_PAL_LOG_DEBUG ? 'D' : 'I';
  emit("[%c][%s] %s\r\n", level_char,
       scope != NULL ? scope : "mp4-player", message);
  return H2_PAL_OK;
}

static const h2_pal_log_api_t *usb_log_api(void) {
  static const h2_pal_log_vtable_t vtable = {.write = usb_log_write};
  static const h2_pal_log_api_t api = {.user = NULL, .vtable = &vtable};
  return &api;
}

static int power_get_running(
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

static int power_get_next(
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

static int power_set_next(void *user, uint32_t partition_id) {
  (void)user;
  if (partition_id != H2_JIELI_PARTITION_LOADER &&
      partition_id != H2_JIELI_PARTITION_APP) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (partition_id == H2_JIELI_PARTITION_LOADER) {
    int result = prepare_app_return(
        (void *)h2_jieli_ac791n_devkit_pref_api());
    if (result != H2_PAL_OK) return result;
  }
  next_boot_partition = partition_id;
  return H2_PAL_OK;
}

static int power_reboot(void *user, uint32_t reason) {
  (void)user;
  emit("JIELI_APP_REBOOT source=power_api reason=%u next=%u\r\n",
       (unsigned)reason, (unsigned)next_boot_partition);
  os_time_dly(10u);
  if (next_boot_partition == H2_JIELI_PARTITION_LOADER &&
      flash_update_clr_boot_info(CLEAR_APP_RUNNING_BANK) != 0) {
    return H2_PAL_ERR_IO;
  }
  system_reset();
  return H2_PAL_OK;
}

static const h2_pal_power_api_t *power_api(void) {
  static const h2_pal_power_vtable_t vtable = {
      .get_running_boot_partition = power_get_running,
      .get_next_boot_partition = power_get_next,
      .set_next_boot_partition = power_set_next,
      .reboot = power_reboot,
  };
  static const h2_pal_power_api_t api = {.vtable = &vtable};
  return &api;
}

static int prepare_app_return(void *user) {
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
       result == H2_PAL_OK &&
       index < sizeof(installed_keys) / sizeof(installed_keys[0]);
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

static int enter_trial_boot(int *out_trial) {
  if (out_trial == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_trial = 0;
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
  *out_trial = 1;
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
        name_space, H2_JIELI_TRIAL_RESET_REASON_KEY, boot_reset_reason);
  }
  if (result == H2_PAL_OK && name_space->commit != NULL) {
    result = name_space->commit(name_space);
  }
  if (name_space != NULL && name_space->close != NULL) {
    int close_result = name_space->close(name_space);
    if (result == H2_PAL_OK) result = close_result;
  }
  if (result != H2_PAL_OK || !repeated_trial) return result;

  /* The Loader owns recovery of INSTALLED_PENDING_CONFIRM.  Do not mount SD
   * or mutate the package lifecycle here: this is the last-resort path after
   * a watchdog reset, so it must contain no operation that can block before
   * the running App bank is invalidated. */
  if (flash_update_clr_boot_info(CLEAR_APP_RUNNING_BANK) != 0) {
    return H2_PAL_ERR_IO;
  }
  system_reset();
  return H2_PAL_ERR_INVALID_STATE;
}

static void early_trial_timeout(void *user) {
  (void)user;
  trial_recovery_timer = 0u;
  /* Keep timeout recovery independent of SD, Pref and the App transport. */
  (void)flash_update_clr_boot_info(CLEAR_APP_RUNNING_BANK);
  system_reset();
}

/* H2Loader-layout SDK hook: run immediately after board_early_init(), before
 * every SDK initcall. Arm recovery before reading Pref: early flash or Pref
 * access is itself part of the trial and must not be able to bypass rollback. */
int h2_jieli_ac791n_devkit_early_app_boot(void) {
  boot_reset_reason = system_reset_reason_get();
  emit("H2_JIELI_MP4_EARLY step=enter reset_reason=0x%x\r\n",
       (unsigned)boot_reset_reason);

  int direct_boot = h2_jieli_mp4_is_direct_boot();
  emit("H2_JIELI_MP4_EARLY step=direct-check direct=%d\r\n", direct_boot);
  if (direct_boot) {
    boot_marker(1u, H2_PAL_OK);
    /* The factory/direct image has no Loader bank to recover to.  Keep the
     * watchdog disabled until this standalone bring-up can expose USB logs;
     * otherwise an SD or decoder startup failure becomes an opaque 4-second
     * reboot loop. */
    wdt_close();
    return H2_PAL_OK;
  }

  /* Do not query JieLi boot info here.  Its update module has not reached its
   * initcall yet, and querying it from this early hook causes an immediate
   * soft-reset loop.  Direct and Loader-managed targets are already
   * distinguished by the link-time h2_jieli_mp4_is_direct_boot() override. */
  wdt_init(WDT_32S);
  emit("H2_JIELI_MP4_EARLY step=watchdog-armed\r\n");

  /* A watchdog reset before trial confirmation must return to the canonical
   * Loader without touching Pref, SD, USB or the diagnostic partition.  This
   * also covers a stall inside those facilities on the preceding boot. */
  if ((boot_reset_reason & SYS_RST_WDT) != 0u) {
    int clear_result =
        flash_update_clr_boot_info(CLEAR_APP_RUNNING_BANK);
    emit("H2_JIELI_MP4_EARLY step=wdt-rollback clear=%d\r\n",
         clear_result);
    system_reset();
    return H2_PAL_ERR_INVALID_STATE;
  }

  trial_recovery_timer = sys_timeout_add_to_task(
      "sys_timer", NULL, early_trial_timeout, 30000u);
  emit("H2_JIELI_MP4_EARLY step=trial-timer timer=%u\r\n",
       (unsigned)trial_recovery_timer);
  if (trial_recovery_timer == 0u) {
    int clear_result =
        flash_update_clr_boot_info(CLEAR_APP_RUNNING_BANK);
    emit("H2_JIELI_MP4_EARLY step=timer-rollback clear=%d\r\n",
         clear_result);
    system_reset();
    return H2_PAL_ERR_TASK;
  }
  boot_marker(1u, H2_PAL_OK);
  /* Pref and Loader metadata are not safe until the SDK initcalls complete.
   * app_main performs the trial-state read before it starts the player. */
  emit("H2_JIELI_MP4_EARLY step=ready trial=pending\r\n");
  return H2_PAL_OK;
}

static h2_pal_result_t memory_stats_read(
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
      },
  };
  return H2_PAL_OK;
}

static void report_previous_exception(void) {
  FILE *file = fopen("mnt/sdfile/EXT_RESERVED/log", "r");
  if (file == NULL) return;
  struct vfs_attr attributes;
  memset(&attributes, 0, sizeof(attributes));
  int result = fget_attrs(file, &attributes);
  (void)fclose(file);
  if (result != 0 || attributes.sclust == 0u) return;
  assert_log_addr = attributes.sclust;
  char log[641];
  memset(log, 0, sizeof(log));
  result = sdfile_reserve_zone_read(log, attributes.sclust, 640u, 0);
  log[640] = '\0';
  if (result != 640 || (uint8_t)log[0] == UINT8_C(0xff)) return;
  emit("JIELI_EXCEPTION_LOG %s\r\n", log);
  (void)sdfile_reserve_zone_erase(
      attributes.sclust, SDFILE_SECTOR_SIZE, 0);
}

static h2_pal_result_t confirm_ready(void *user) {
  (void)user;
  if (h2_jieli_mp4_is_direct_boot()) {
    emit("H2_JIELI_MP4_READY rc=0 mode=direct heap_free=%u heap_peak=%u\r\n",
         (unsigned)get_malloc_remain_heap_size(),
         (unsigned)malloc_max_footprint());
    return H2_PAL_OK;
  }
  h2_pal_result_t result = (h2_pal_result_t)
      h2_jieli_app_loader_confirm(&loader_client.config);
  boot_marker(10u, result);
  emit("H2_JIELI_MP4_READY rc=%d heap_free=%u heap_peak=%u\r\n",
       result, (unsigned)get_malloc_remain_heap_size(),
       (unsigned)malloc_max_footprint());
  if (result == H2_PAL_OK && trial_recovery_timer != 0u) {
    uint16_t timer_to_delete = trial_recovery_timer;
    sys_timeout_del(trial_recovery_timer);
    trial_recovery_timer = 0u;
    emit("JIELI_TRIAL_TIMER state=deleted id=%u\r\n",
         (unsigned)timer_to_delete);
  }
  /* The trial boot already armed the watchdog before SDK initialization.
   * Reinitializing the live watchdog here can stall inside the JieLi driver
   * while DAC playback is active. Playback progress continues to feed the
   * existing watchdog through mp4_watchdog_poll(). */
  return result;
}

static int mp4_watchdog_poll(void *user) {
  (void)user;
  /* The common player invokes should_stop while waiting for and pacing video
   * frames.  Tie the board watchdog to real playback progress: a healthy
   * pipeline feeds it, while a blocked video path still resets and lets the
   * Loader recover an unconfirmed trial image. */
  wdt_clear();
  return 0;
}

static h2_pal_result_t mp4_runtime_config(h2_runtime_config_t *out_config) {
  if (out_config == NULL) return H2_PAL_ERR_INVALID_ARG;

  /* app_main can run before the asynchronous SD probe reports sd0 online.
   * The common player deliberately treats FS initialization failures as
   * terminal, so keep this board-specific readiness wait in the target
   * launcher rather than adding retry policy to the shared App. */
  unsigned sd_wait_ticks = 0u;
  while (!dev_online("sd0") && sd_wait_ticks < 500u) {
    os_time_dly(1u);
    ++sd_wait_ticks;
  }
  emit("H2_JIELI_MP4_SD_READY online=%d wait_ticks=%u\r\n",
       dev_online("sd0") != 0, sd_wait_ticks);
  if (!dev_online("sd0")) return H2_PAL_ERR_NOT_FOUND;

  memset(&mp4_fs, 0, sizeof(mp4_fs));
  h2_pal_result_t result = h2_jieli_ac791n_devkit_sd_fs_init(&mp4_fs);
  if (result != H2_PAL_OK) return result;

  /* This target only plays a local, video-only MP4.  Supplying the board's
   * full Runtime surface here pulls the JieLi Wi-Fi, LwIP and Bluetooth
   * archives into the image, including their pre-app_main registrations and
   * several hundred KiB of static state.  Keep the public Runtime contract
   * complete while linking real providers only for the playback path. */
  *out_config = (h2_runtime_config_t){
      .board = "jieli_ac791n_devkit",
      .target = "wl82",
      .chip = "ac791n",
      .firmware_info = h2_jieli_wl82_platform_firmware_info_api(),
      .mem = h2_jieli_wl82_platform_mem_api(),
      .log = usb_log_api(),
      .time = h2_jieli_wl82_platform_time_api(),
      .timer = h2_jieli_wl82_platform_timer_api(),
      .task = h2_jieli_wl82_platform_task_api(),
      .queue = h2_jieli_wl82_platform_queue_api(),
      .sync = h2_jieli_wl82_platform_sync_api(),
      .fs = &mp4_fs,
      .disk = h2_jieli_ac791n_devkit_disk_api(),
      .pref = h2_jieli_ac791n_devkit_pref_api(),
      .crypto = h2_pal_unsupported_crypto_api(),
      .http = h2_pal_unsupported_http_api(),
      .net = h2_pal_unsupported_net_api(),
      .netif = h2_pal_unsupported_netif_api(),
      .mqtt = h2_pal_unsupported_mqtt_api(),
      .webrtc = h2_pal_unsupported_webrtc_api(),
      .wifi_sta = h2_pal_unsupported_wifi_sta_api(),
      .wifi_ap = h2_pal_unsupported_wifi_ap_api(),
      .wifi_csi = h2_pal_unsupported_wifi_csi_api(),
      .wifi_settings = h2_pal_unsupported_wifi_settings_api(),
      .ble_host = h2_pal_unsupported_ble_host_api(),
      .modem = h2_pal_unsupported_modem_api(),
      .power = h2_pal_unsupported_power_api(),
      .display = h2_jieli_ac791n_devkit_display_api(),
      /* The bundled media is 16 kHz mono AAC.  Decode it through FDK AAC and
       * send the resulting S16LE PCM to the development board DAC/PA. */
      .audio = h2_jieli_ac791n_devkit_audio_api(),
      .audio_decoder = h2_linux_fdk_aac_decoder_api(),
      .periph = h2_pal_unsupported_periph_api(),
      .button = h2_pal_unsupported_button_api(),
      .touch = h2_pal_unsupported_touch_api(),
      .buzzer = h2_pal_unsupported_buzzer_api(),
      .nfc = h2_pal_unsupported_nfc_api(),
      .nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api(),
      .imu = h2_pal_unsupported_imu_api(),
      .gpio_irq = h2_pal_unsupported_gpio_irq_api(),
      .led = h2_pal_unsupported_led_api(),
      .switch_api = h2_pal_unsupported_switch_api(),
      .pwm_switch = h2_pal_unsupported_pwm_switch_api(),
      .input = h2_pal_unsupported_input_api(),
      .system_event = h2_pal_unsupported_system_event_api(),
      .video_decoder = h2_tinyh264_video_decoder_api(),
  };
  return H2_PAL_OK;
}

static void mp4_runtime(void *user) {
  (void)user;
  boot_marker(7u, H2_PAL_OK);
  h2_runtime_config_t config;
  h2_runtime_t *runtime = NULL;
  h2_pal_result_t result = mp4_runtime_config(&config);
  boot_marker(8u, result);
  if (result == H2_PAL_OK) {
    result = h2_runtime_init(&config, &runtime);
  }
  boot_marker(9u, result);
  emit("H2_JIELI_MP4_RUNTIME rc=%d heap_free=%u\r\n", result,
       (unsigned)get_malloc_remain_heap_size());
  if (result == H2_PAL_OK) {
    /* JieLi's DevKitBoard A/V example initializes the LCD/EMI bus before it
     * opens the DAC decoder.  Preserve that board ordering here; the shared
     * player opens audio first, and opening EMI after the DAC is active can
     * stall inside the panel bring-up path.  display_open is idempotent, so
     * the shared player may safely open it again. */
    emit("H2_JIELI_MP4_DISPLAY stage=preopen-before\r\n");
    result = (h2_pal_result_t)h2_pal_display_open(runtime->display);
    emit("H2_JIELI_MP4_DISPLAY stage=preopen-after rc=%d\r\n", result);
  }
  if (result == H2_PAL_OK) {
    const h2_smoke_mp4_player_config_t player_config = {
        .media_path = "/data/media/showcase.mp4",
        .acquire_timeout_ms = 2000u,
        .looping = 1,
        .display_mode = H2_SMOKE_MP4_PLAYER_DISPLAY_CENTER,
        .require_audio = 1,
        .should_stop = mp4_watchdog_poll,
        .on_ready = confirm_ready,
        .ready_user = runtime,
    };
    emit("H2_JIELI_MP4_WATCHDOG mode=playback-progress\r\n");
    result = h2_smoke_mp4_player_run(runtime, &player_config);
  }
  emit("H2_JIELI_MP4_FAIL stage=run rc=%d heap_free=%u\r\n", result,
       (unsigned)get_malloc_remain_heap_size());
  if (h2_jieli_mp4_is_direct_boot()) {
    /* Preserve the first useful failure and USB transport on a standalone
     * image.  Loader-installed trial images intentionally retain watchdog
     * rollback semantics here. */
    wdt_close();
  }
  for (;;) os_time_dly(100u);
}

void app_main(void) {
  if (h2_jieli_mp4_is_direct_boot()) {
    /* Give the host CDC driver enough time to open before the media pipeline
     * starts.  A hard exception in decoder/audio startup otherwise resets the
     * whole USB device before its diagnostic output can be observed. */
    for (unsigned second = 0u; second < 20u; ++second) {
      emit("H2_JIELI_MP4_BOOT_WAIT second=%u\r\n", second);
      os_time_dly(100u);
    }
  }
  int result = h2_pal_firmware_info_get_current(
      h2_jieli_wl82_platform_firmware_info_api(), &firmware_info);
  boot_marker(3u, result);
  if (result != H2_PAL_OK) {
    (void)prepare_app_return((void *)h2_jieli_ac791n_devkit_pref_api());
    (void)flash_update_clr_boot_info(CLEAR_APP_RUNNING_BANK);
    system_reset();
    return;
  }
  if (h2_jieli_mp4_is_direct_boot()) {
    report_previous_exception();
    emit("H2_JIELI_MP4_BOOT version=%s mode=direct heap_free=%u\r\n",
         firmware_info.version, (unsigned)get_malloc_remain_heap_size());
    const h2_pal_task_options_t direct_options = {
        .name = "h2mp4/runtime",
        .min_stack_size = 131072u,
    };
    result = h2_pal_task_start(
        h2_jieli_wl82_platform_task_api(), &direct_options,
        mp4_runtime, NULL, &runtime_task);
    boot_marker(6u, result);
    emit("H2_JIELI_MP4_TASK rc=%d mode=direct\r\n", result);
    return;
  }
  memset(&mp4_fs, 0, sizeof(mp4_fs));
  result = h2_jieli_ac791n_devkit_sd_fs_init(&mp4_fs);
  if (result != H2_PAL_OK) return;
  int is_trial = 0;
  result = enter_trial_boot(&is_trial);
  boot_marker(2u, result);
  emit("H2_JIELI_MP4_TRIAL step=enter result=%d trial=%d\r\n",
       result, is_trial);
  if (result != H2_PAL_OK) {
    int clear_result =
        flash_update_clr_boot_info(CLEAR_APP_RUNNING_BANK);
    emit("H2_JIELI_MP4_TRIAL step=rollback clear=%d\r\n",
         clear_result);
    system_reset();
    return;
  }
  if (!is_trial && trial_recovery_timer != 0u) {
    sys_timeout_del(trial_recovery_timer);
    trial_recovery_timer = 0u;
    wdt_init(WDT_32S);
  }
  h2_loader_app_client_config_t client_config;
  result = h2_jieli_app_loader_config_init(
      &client_config, &mp4_fs, power_api(),
      (h2_loader_memory_stats_api_t){.read = memory_stats_read},
      H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI |
          H2_LOADER_CAPABILITY_BLE);
  if (result != H2_PAL_OK) return;
  result = h2_loader_app_client_init(&loader_client, &client_config);
  boot_marker(4u, result);
  if (result != H2_PAL_OK) return;
  result = h2_jieli_app_iostreamikcp_start(
      &loader_client, h2_jieli_wl82_platform_task_api(),
      h2_jieli_wl82_platform_mem_api());
  boot_marker(5u, result);
  if (result != H2_PAL_OK) {
    (void)h2_loader_reboot_h2loader_with_transition(
        &loader_client.loader, NULL, NULL);
    return;
  }
  report_previous_exception();
  emit("H2_JIELI_MP4_BOOT version=%s timer=%u heap_free=%u\r\n",
       firmware_info.version, (unsigned)trial_recovery_timer,
       (unsigned)get_malloc_remain_heap_size());
  const h2_pal_task_options_t options = {
      .name = "h2mp4/runtime",
      .min_stack_size = 131072u,
  };
  result = h2_pal_task_start(
      h2_jieli_wl82_platform_task_api(), &options,
      mp4_runtime, NULL, &runtime_task);
  boot_marker(6u, result);
  emit("H2_JIELI_MP4_TASK rc=%d\r\n", result);
  if (result != H2_PAL_OK) {
    (void)h2_loader_reboot_h2loader_with_transition(
        &loader_client.loader, NULL, NULL);
  }
}
