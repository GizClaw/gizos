#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_target_task_policy.h"
#include "h2_mfg.h"
#include "h2_mosaico_loader_usb.h"
#include "h2_mosaico_usb_console.h"
#include "bsp/esp_mosaico.h"

#include <stdio.h>
#include <string.h>

extern const h2_mfg_profile_t h2_mfg_board_profile;

/* The compensated overflow code and live indication belong to this app. */
#define MAGNET_INVALID_MGAUSS (-327680)
#define MAGNET_DELTA_MGAUSS 80
#define FIXTURE_WIFI_TIMEOUT_MS 15000u

typedef struct magnet_observation {
  h2_pal_imu_reading_t current;
  int32_t baseline[3], last[3], forwarded[3];
  unsigned char valid[3];
  int present, responded, error;
} magnet_observation_t;

typedef struct mosaico_mfg {
  const h2_mfg_profile_t* profile;
  const h2_pal_imu_api_t* imu;
  magnet_observation_t magnets[H2_MFG_MAGNETOMETER_MAX];
  const h2_pal_audio_api_t* audio;
  int audio_config_error;
} mosaico_mfg_t;

/* MFG owns both this adapter and the UI callback on its controller task.
 * Runtime input polling is not started by this image. No second reader or
 * background sampler is needed for the live presentation. */
static int read_magnet(void* user, uint32_t index, int* present, int* error) {
  const mosaico_mfg_t* app = user;
  if (app == NULL || index >= app->profile->magnetometer_count ||
      present == NULL || error == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *present = app->magnets[index].present;
  *error = app->magnets[index].error;
  return H2_PAL_OK;
}

static h2_pal_result_t read_imu(void* user, h2_pal_periph_id_t id,
                                h2_pal_imu_reading_t* out) {
  mosaico_mfg_t* app = user;
  int rc = h2_pal_imu_read(app->imu, id, out);
  uint32_t index = 0;
  while (index < app->profile->magnetometer_count &&
         app->profile->magnetometers[index] != id)
    ++index;
  if (index == app->profile->magnetometer_count) {
    /* The normal IMU step precedes magnets, including after a full reset. */
    if (id == app->profile->imu) memset(app->magnets, 0, sizeof(app->magnets));
    return rc;
  }

  magnet_observation_t* magnet = &app->magnets[index];
  magnet->present = 0;
  if (rc == H2_PAL_OK) {
    magnet->current = *out;
    if (!(out->flags & H2_PAL_IMU_HAS_MAG)) rc = H2_PAL_ERR_FORMAT;
  }
  magnet->error = rc;
  if (rc != H2_PAL_OK) {
    /* Shared detection stops at the first failed sensor. Invalidate the entire
     * app observation snapshot so an unpolled side cannot show stale contact.
     */
    for (unsigned i = 0; i < app->profile->magnetometer_count; ++i) {
      app->magnets[i].present = 0;
      app->magnets[i].error = rc;
      memset(app->magnets[i].valid, 0, sizeof(app->magnets[i].valid));
    }
    return rc;
  }

  const int32_t axes[] = {out->mag_mgauss.x, out->mag_mgauss.y,
                          out->mag_mgauss.z};
  unsigned valid_axes = 0;
  for (unsigned axis = 0; axis < 3; ++axis) {
    if (axes[axis] == MAGNET_INVALID_MGAUSS) {
      magnet->valid[axis] = 0;
      continue;
    }
    ++valid_axes;
    if (!magnet->valid[axis]) {
      magnet->baseline[axis] = magnet->last[axis] = axes[axis];
      magnet->valid[axis] = 1;
      continue;
    }
    int64_t delta = (int64_t)axes[axis] - magnet->baseline[axis];
    if (delta >= MAGNET_DELTA_MGAUSS || delta <= -MAGNET_DELTA_MGAUSS)
      magnet->present = 1;
    /* Preserve valid consecutive movement in mGauss, with a stable output
     * through overflow and its first recovery sample. Starting unknown axes
     * at zero prevents their first usable value from creating movement in
     * the shared min/max check. Raw readings remain in current for the app. */
    int64_t next =
        (int64_t)magnet->forwarded[axis] + axes[axis] - magnet->last[axis];
    magnet->forwarded[axis] = next > INT32_MAX   ? INT32_MAX
                              : next < INT32_MIN ? INT32_MIN
                                                 : (int32_t)next;
    magnet->last[axis] = axes[axis];
  }
  magnet->responded |= magnet->present;
  out->mag_mgauss.x = magnet->forwarded[0];
  out->mag_mgauss.y = magnet->forwarded[1];
  out->mag_mgauss.z = magnet->forwarded[2];
  if (valid_axes == 0) {
    for (unsigned i = 0; i < app->profile->magnetometer_count; ++i) {
      app->magnets[i].present = 0;
      app->magnets[i].error = H2_PAL_ERR_FORMAT;
      memset(app->magnets[i].valid, 0, sizeof(app->magnets[i].valid));
    }
  }
  return magnet->error;
}

static const h2_pal_imu_vtable_t imu_vtable = {.read_imu = read_imu};

#define AUDIO_FORWARD0(name)                \
  static int app_##name(void* user) {       \
    mosaico_mfg_t* app = user;              \
    return h2_pal_audio_##name(app->audio); \
  }
AUDIO_FORWARD0(start_mic)
AUDIO_FORWARD0(stop_mic)
AUDIO_FORWARD0(start_speaker)
AUDIO_FORWARD0(stop_speaker)
#define AUDIO_FORWARD(name, type)                  \
  static int app_##name(void* user, type value) {  \
    mosaico_mfg_t* app = user;                     \
    return h2_pal_audio_##name(app->audio, value); \
  }
/* Mosaico MFG uses 12 dB PGA through the board's 0..30 dB PAL mapping.
 * Reject an unapplied setting without preventing Loader recovery. */
static int configure_mic_gain(const h2_pal_audio_api_t* audio) {
  uint32_t actual = 0;
  int rc = h2_pal_audio_set_mic_gain_percent(audio, 40);
  if (rc == 0) rc = h2_pal_audio_get_mic_gain_percent(audio, &actual);
  if (rc == 0 && actual != 40) rc = H2_PAL_ERR_IO;
  printf("H2_MOSAICO_MIC_GAIN requested=40 actual=%u code=%d\n",
         (unsigned)actual, rc);
  return rc;
}
static int app_get_info(void* user, h2_audio_info_t* info) {
  mosaico_mfg_t* app = user;
  if (app->audio_config_error != 0) {
    memset(info, 0, sizeof(*info));
    return app->audio_config_error;
  }
  return h2_pal_audio_get_info(app->audio, info);
}
AUDIO_FORWARD(get_speaker_volume_percent, uint32_t*)
AUDIO_FORWARD(set_speaker_volume_percent, uint32_t)
AUDIO_FORWARD(get_mic_gain_percent, uint32_t*)
AUDIO_FORWARD(set_aec_observer, const h2_audio_aec_observer_t*)
AUDIO_FORWARD(set_mic_gain_percent, uint32_t)
#undef AUDIO_FORWARD
#undef AUDIO_FORWARD0
static int app_mic_read(void* user, h2_audio_frame_t* frame, uint32_t timeout) {
  mosaico_mfg_t* app = user;
  return h2_pal_audio_mic_read(app->audio, frame, timeout);
}
static int app_create_track(void* user, const h2_audio_track_config_t* config,
                            h2_pal_audio_track_t** out) {
  mosaico_mfg_t* app = user;
  if (config != NULL && config->name != NULL &&
      strcmp(config->name, "mfg/aec") == 0) {
    /* Board-owned source level. The shared observers measure actual PCM,
     * and the same track level applies to the operator's voice replay. */
    h2_audio_track_config_t track = *config;
    track.volume_factor_milli = 250;
    return h2_pal_audio_create_track(app->audio, &track, out);
  }
  return h2_pal_audio_create_track(app->audio, config, out);
}
static const h2_pal_audio_vtable_t audio_vtable = {
    .get_info = app_get_info,
    .start_mic = app_start_mic,
    .stop_mic = app_stop_mic,
    .start_speaker = app_start_speaker,
    .stop_speaker = app_stop_speaker,
    .mic_read = app_mic_read,
    .create_track = app_create_track,
    .get_speaker_volume_percent = app_get_speaker_volume_percent,
    .set_speaker_volume_percent = app_set_speaker_volume_percent,
    .get_mic_gain_percent = app_get_mic_gain_percent,
    .set_mic_gain_percent = app_set_mic_gain_percent,
    .set_aec_observer = app_set_aec_observer};

/* Public manufacturing fixture. PAL connect never saves these credentials.
 * Run before Loader owns Wi-Fi; both waits are bounded and failure still
 * enters Loader so USB/BLE recovery remains available. */
static int connect_fixture_wifi(h2_runtime_t* runtime) {
  const h2_pal_wifi_sta_config_t fixture = {
      .ssid = "HAIVIVI-MFG",
      .ssid_len = sizeof("HAIVIVI-MFG") - 1u,
      .password = "!haivivi",
      .password_len = sizeof("!haivivi") - 1u,
  };
  int rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, &fixture,
                                   FIXTURE_WIFI_TIMEOUT_MS);
  if (rc != H2_PAL_OK) return rc;
  uint64_t started = 0;
  rc = h2_pal_time_get_monotonic_ms(runtime->time, &started);
  if (rc != H2_PAL_OK) return rc;
  for (unsigned polls = 0; polls <= FIXTURE_WIFI_TIMEOUT_MS / 100u; ++polls) {
    h2_pal_wifi_sta_status_t status = {0};
    rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
    if (rc != H2_PAL_OK) return rc;
    if (status.state == H2_PAL_WIFI_STA_STATE_GOT_IP && status.ip_valid &&
        status.ip.ip4 != 0 && status.ssid_len == fixture.ssid_len &&
        memcmp(status.ssid, fixture.ssid, fixture.ssid_len) == 0)
      return H2_PAL_OK;
    if (status.state == H2_PAL_WIFI_STA_STATE_FAILED ||
        status.state == H2_PAL_WIFI_STA_STATE_DISCONNECTED)
      return H2_PAL_ERR_IO;
    uint64_t now = 0;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
    if (rc != H2_PAL_OK) return rc;
    if (now - started >= FIXTURE_WIFI_TIMEOUT_MS ||
        polls == FIXTURE_WIFI_TIMEOUT_MS / 100u)
      return H2_PAL_ERR_TIMEOUT;
    rc = h2_pal_time_sleep_ms(runtime->time, 100);
    if (rc != H2_PAL_OK) return rc;
  }
  return H2_PAL_ERR_TIMEOUT;
}

static void image_entry(void* unused) {
  (void)unused;
  h2_runtime_config_t config;
  h2_runtime_t* runtime = NULL;
  h2_mfg_t* mfg = NULL;
  /* These borrowed objects outlive Loader and all MFG cleanup retries. */
  h2_mfg_profile_t profile = h2_mfg_board_profile;
  h2_mfg_ui_config_t ui = *profile.ui;
  mosaico_mfg_t app = {.profile = &profile};
  const h2_pal_imu_api_t imu = {.user = &app, .vtable = &imu_vtable};
  const h2_pal_audio_api_t audio = {.user = &app, .vtable = &audio_vtable};
  ui.read_magnet = read_magnet;
  ui.user = &app;
  profile.ui = &ui;
  int rc = h2_esp_board_runtime_config(&config);
  if (rc == H2_PAL_OK) rc = h2_runtime_init(&config, &runtime);
  if (rc == H2_PAL_OK) {
    app.imu = runtime->imu;
    app.audio = runtime->audio;
    app.audio_config_error = configure_mic_gain(app.audio);
    runtime->imu = &imu;
    runtime->audio = &audio;
    rc = h2_mfg_create(runtime, &profile, &mfg);
  }
  if (rc != H2_PAL_OK) {
    printf("H2_MFG_INIT_FAIL board=%s code=%d\n", h2_mfg_board_profile.board,
           rc);
    if (runtime != NULL) h2_runtime_deinit(runtime);
    return;
  }

  h2_esp_h2loader_config_t loader = {
      .board = h2_mfg_board_profile.board,
      .mfg_required_total = h2_mfg_board_profile.total,
      .user = mfg,
      .mfg_command = h2_mfg_command,
      .run_mfg = h2_mfg_run,
      .rearm_mfg = h2_mfg_rearm,
      .before_disruptive = h2_mfg_before_disruptive,
      .install_progress = NULL,
      .show_installing = NULL,
      .show_install_failed = NULL,
      .show_launching = NULL,
  };
  int wifi_rc = connect_fixture_wifi(runtime);
  printf("H2_MFG_WIFI_FIXTURE board=%s code=%d\n", profile.board, wifi_rc);
  h2_esp_h2loader_run_with_ble_config(runtime, &loader);
  /* Returning also tears down the board; retain it until MFG releases every
   * provider and worker, including after a failed cleanup attempt. */
  while ((rc = h2_mfg_destroy(&mfg)) != H2_PAL_OK) {
    printf("H2_MFG_CLEANUP_FAIL board=%s code=%d\n", h2_mfg_board_profile.board,
           rc);
    (void)h2_pal_time_sleep_ms(runtime->time, 1000);
  }
  h2_runtime_deinit(runtime);
}

void app_main(void) {
  bsp_board_variant_t variant;
  int rc = bsp_board_variant_get(&variant);
  if (rc == ESP_OK) rc = h2_esp_target_task_policy_install();
  if (rc == H2_PAL_OK) rc = h2_mosaico_usb_console_init();
  if (rc == H2_PAL_OK) rc = h2_mosaico_loader_usb_init();
  if (rc == H2_PAL_OK)
    rc = h2_esp_board_start_entry_task("mosaico/mfg", image_entry, NULL);
  if (rc != H2_PAL_OK)
    printf("H2_MFG_ENTRY_FAIL board=esp_mosaico code=%d\n", rc);
}
