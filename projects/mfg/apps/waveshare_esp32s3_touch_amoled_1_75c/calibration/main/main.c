#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_target_task_policy.h"
#include "h2_aec_calibration.h"
#include "h2_mfg_aec_fixture.h"
#include "h2_ws175c_audio_diagnostics.h"

#include "esp_app_desc.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct calibration {
  h2_runtime_t* rt;
  h2_pal_mutex_t* mutex;
  h2_pal_semaphore_t* finished;
  int running, stopping, pending, active, retained, shutting_source;
  uint64_t run;
  char uid[13], peer_uid[13], peer_ip[16];
  h2_aec_calibration_pair_t pair;
  h2_aec_calibration_t* runner;
  int socket;
  uint32_t sequence;
} calibration_t;

static calibration_t calibration;

static int source(void* user, bool enabled, const h2_audio_pcm_format_t* format,
                  const int16_t* probe, size_t samples, uint32_t timeout_ms) {
  calibration_t* c = user;
  if (format->sample_rate_hz != 16000 || format->channels != 1 ||
      format->sample_format != H2_AUDIO_SAMPLE_S16LE ||
      samples != H2_MFG_AEC_PROBE_SAMPLES || timeout_ms == 0 || c->socket < 0)
    return H2_PAL_ERR_UNSUPPORTED;
  h2_mfg_aec_packet_t request = {
      .operation = c->shutting_source ? H2_MFG_AEC_SOURCE_SHUTDOWN
                   : enabled          ? H2_MFG_AEC_SOURCE_START
                                      : H2_MFG_AEC_SOURCE_STOP,
      .run_lo = (uint32_t)c->run,
      .run_hi = (uint32_t)(c->run >> 32),
      .sequence = ++c->sequence,
      .sample_rate = format->sample_rate_hz,
      .samples = enabled ? (uint32_t)samples : 0};
  memcpy(request.source_uid, c->uid, 12);
  memcpy(request.target_uid, c->peer_uid, 12);
  if (enabled) memcpy(request.pcm, probe, samples * sizeof(int16_t));
  h2_mfg_aec_packet_seal(&request);
  uint64_t start = 0;
  int rc = h2_pal_time_get_monotonic_ms(c->rt->time, &start);
  if (rc != 0) return rc;
  uint64_t last_sent = 0;
  for (;;) {
    uint64_t now = 0;
    rc = h2_pal_time_get_monotonic_ms(c->rt->time, &now);
    if (rc != 0) return rc;
    if (now - start >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
    if (last_sent == 0 || now - last_sent >= 250) {
      ssize_t sent =
          send(c->socket, &request, h2_mfg_aec_packet_size(&request), 0);
      if (sent != (ssize_t)h2_mfg_aec_packet_size(&request))
        return H2_PAL_ERR_IO;
      last_sent = now;
    }
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(c->socket, &readable);
    struct timeval wait = {.tv_sec = 0, .tv_usec = 100000};
    int selected = select(c->socket + 1, &readable, NULL, NULL, &wait);
    if (selected < 0) return H2_PAL_ERR_IO;
    if (selected == 0) continue;
    h2_mfg_aec_packet_t response;
    ssize_t bytes = recv(c->socket, &response, sizeof(response), 0);
    if (bytes < 0) return H2_PAL_ERR_IO;
    if (!h2_mfg_aec_packet_valid(&response, (size_t)bytes) ||
        response.operation != (request.operation | H2_MFG_AEC_SOURCE_ACK) ||
        response.run_hi != request.run_hi ||
        response.run_lo != request.run_lo ||
        response.sequence != request.sequence || response.samples != 0 ||
        memcmp(response.source_uid, c->peer_uid, 12) != 0 ||
        memcmp(response.target_uid, c->uid, 12) != 0)
      continue;
    printf("H2_MFG_CALIBRATION_SOURCE run=%016" PRIx64 " sequence=%" PRIu32
           " enabled=%d probe_checksum=%08" PRIx32 " peer=%s rc=%" PRId32 "\n",
           c->run, c->sequence, enabled, request.checksum, c->peer_uid,
           response.result);
    h2_ws175c_audio_controls_t controls;
    int observed = h2_ws175c_audio_probe_read_controls(&controls);
    if (observed == 0)
      printf("H2_MFG_CALIBRATION_CONTROLS run=%016" PRIx64 " sequence=%" PRIu32
             " dac=%02x adc=%02x,%02x,%02x,%02x\n",
             c->run, c->sequence, controls.dac_register,
             controls.adc_gain_registers[0], controls.adc_gain_registers[1],
             controls.adc_gain_registers[2], controls.adc_gain_registers[3]);
    else if (c->sequence > 1 && !c->shutting_source) {
      printf("H2_MFG_CALIBRATION_CONTROLS run=%016" PRIx64 " sequence=%" PRIu32
             " rc=%d\n",
             c->run, c->sequence, observed);
      fflush(stdout);
      return observed;
    }
    fflush(stdout);
    return response.result;
  }
}

static h2_pal_result_t emit(void* user, const char* line, size_t length) {
  (void)user;
  if (fwrite(line, 1, length, stdout) != length || fputc('\n', stdout) == EOF ||
      fflush(stdout) != 0)
    return H2_PAL_ERR_IO;
  return 0;
}

static int release_runner(calibration_t* c) {
  int rc = h2_aec_calibration_destroy(c->runner);
  if (rc != 0) return rc;
  c->runner = NULL;
  if (c->socket >= 0) {
    (void)close(c->socket);
    c->socket = -1;
  }
  return 0;
}

static int command(void* user, h2_loader_command_transport_t transport,
                   size_t argc, const char* const* argv, char* output,
                   size_t capacity) {
  calibration_t* c = user;
  (void)transport;
  int rc = h2_pal_mutex_lock(c->rt->sync, c->mutex);
  if (rc != 0) return rc;
  if (argc == 1 && strcmp(argv[0], "status") == 0) {
    rc = 0;
  } else if (argc == 1 && strcmp(argv[0], "source-shutdown") == 0) {
    if (c->active || c->pending || c->stopping || c->retained)
      rc = H2_PAL_ERR_BUSY;
    else if (c->peer_uid[0] == '\0' || c->peer_ip[0] == '\0')
      rc = H2_PAL_ERR_INVALID_STATE;
    else
      c->pending = 2;
  } else if (argc == 6 && strcmp(argv[0], "calibrate") == 0) {
    struct in_addr address;
    if (c->active || c->pending || c->stopping || c->retained)
      rc = H2_PAL_ERR_BUSY;
    else if (inet_aton(argv[1], &address) == 0 || strlen(argv[1]) > 15 ||
             strlen(argv[2]) == 0 || strlen(argv[2]) > 3 ||
             strlen(argv[3]) == 0 || strlen(argv[3]) > 3 ||
             strspn(argv[2], "0123456789") != strlen(argv[2]) ||
             strspn(argv[3], "0123456789") != strlen(argv[3]) ||
             strlen(argv[4]) != 16 ||
             strspn(argv[4], "0123456789abcdef") != 16 ||
             strlen(argv[5]) != 12 || strspn(argv[5], "0123456789abcdef") != 12)
      rc = H2_PAL_ERR_INVALID_ARG;
    else {
      unsigned long speaker = strtoul(argv[2], NULL, 10);
      unsigned long mic = strtoul(argv[3], NULL, 10);
      if (speaker == 0 || speaker > 100 || mic > 100)
        rc = H2_PAL_ERR_INVALID_ARG;
      else {
        c->pair = (h2_aec_calibration_pair_t){
            .speaker_percent = (uint32_t)speaker, .mic_percent = (uint32_t)mic};
        c->run = strtoull(argv[4], NULL, 16);
        memcpy(c->peer_uid, argv[5], 13);
        (void)snprintf(c->peer_ip, sizeof(c->peer_ip), "%s", argv[1]);
        c->pending = 1;
      }
    }
  } else {
    rc = H2_PAL_ERR_INVALID_ARG;
  }
  (void)snprintf(output, capacity,
                 "%s uid=%s active=%d pending=%d retained=%d"
                 " run=%016" PRIx64 " speaker=%" PRIu32 " mic=%" PRIu32
                 " result=%s code=%d",
                 argc == 1 && strcmp(argv[0], "status") == 0
                     ? "H2_MFG_STATUS"
                     : "H2_MFG_CALIBRATION_STATUS",
                 c->uid, c->active, c->pending, c->retained, c->run,
                 c->pair.speaker_percent, c->pair.mic_percent,
                 rc == 0 ? "OK" : "fail", rc);
  (void)h2_pal_mutex_unlock(c->rt->sync, c->mutex);
  return rc;
}

static int before_disruptive(void* user, h2_loader_disruptive_action_t action) {
  calibration_t* c = user;
  (void)action;
  int rc = h2_pal_mutex_lock(c->rt->sync, c->mutex);
  if (rc != 0) return rc;
  if (c->active || c->pending) {
    (void)h2_pal_mutex_unlock(c->rt->sync, c->mutex);
    return H2_PAL_ERR_BUSY;
  }
  rc = release_runner(c);
  if (rc != 0) {
    (void)h2_pal_mutex_unlock(c->rt->sync, c->mutex);
    return rc;
  }
  c->stopping = 1;
  int running = c->running;
  (void)h2_pal_mutex_unlock(c->rt->sync, c->mutex);
  return running ? h2_pal_semaphore_take(c->rt->sync, c->finished, 5000) : 0;
}

static void rearm(void* user) {
  calibration_t* c = user;
  if (h2_pal_mutex_lock(c->rt->sync, c->mutex) != 0) return;
  c->stopping = 0;
  (void)h2_pal_mutex_unlock(c->rt->sync, c->mutex);
}

static int run(void* user, h2_loader_t* loader,
               const h2_pal_sync_api_t* operation_sync,
               h2_pal_mutex_t* operation_mutex,
               const h2_pal_sync_api_t* wifi_sync, h2_pal_mutex_t* wifi_mutex) {
  calibration_t* c = user;
  (void)wifi_sync;
  (void)wifi_mutex;
  /* Finish Loader self-update before accepting calibration jobs. */
  h2_loader_status_t status;
  int rc = h2_loader_read_status(loader, &status);
  if (rc != 0) return rc;
  if (status.running_partition_id != 1 ||
      (status.boot_intent == H2_LOADER_BOOT_INTENT_AUTO && status.stage.valid &&
       status.stage.role == H2_LOADER_IMAGE_ROLE_H2LOADER))
    return H2_PAL_EXIT;
  (void)operation_sync;
  (void)operation_mutex;
  (void)h2_pal_mutex_lock(c->rt->sync, c->mutex);
  c->running = 1;
  (void)h2_pal_semaphore_take(c->rt->sync, c->finished, 0);
  (void)h2_pal_mutex_unlock(c->rt->sync, c->mutex);
  printf(
      "H2_MFG_CALIBRATION_READY uid=%s version=%s selection=speaker-first"
      " engine=three-band-two-level-v2\n",
      c->uid, esp_app_get_description()->version);
  fflush(stdout);
  for (;;) {
    (void)h2_pal_mutex_lock(c->rt->sync, c->mutex);
    int stop = c->stopping;
    int pending = c->pending;
    if (pending) {
      c->pending = 0;
      c->active = 1;
    }
    (void)h2_pal_mutex_unlock(c->rt->sync, c->mutex);
    if (stop) break;
    if (!pending) {
      (void)h2_pal_time_sleep_ms(c->rt->time, 20);
      continue;
    }
    /* Shutdown belongs to the last run and must follow its final STOP. */
    if (pending == 1) c->sequence = 0;
    c->shutting_source = pending == 2;
    c->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in peer = {.sin_family = AF_INET,
                               .sin_port = htons(H2_MFG_AEC_PORT)};
    if (c->socket < 0 || inet_aton(c->peer_ip, &peer.sin_addr) == 0 ||
        connect(c->socket, (struct sockaddr*)&peer, sizeof(peer)) != 0)
      rc = H2_PAL_ERR_IO;
    else
      rc = 0;
    if (c->shutting_source) {
      const h2_audio_pcm_format_t format = {
          .sample_rate_hz = 16000,
          .frame_samples_per_channel = 512,
          .channels = 1,
          .sample_format = H2_AUDIO_SAMPLE_S16LE};
      if (rc == 0) rc = source(c, false, &format, NULL, 512, 5000);
      int cleanup = release_runner(c);
      printf("H2_MFG_CALIBRATION_SOURCE_END uid=%s peer=%s rc=%d cleanup=%d\n",
             c->uid, c->peer_uid, rc, cleanup);
      fflush(stdout);
      (void)h2_pal_mutex_lock(c->rt->sync, c->mutex);
      c->active = 0;
      c->retained = cleanup != 0;
      (void)h2_pal_mutex_unlock(c->rt->sync, c->mutex);
      continue;
    }
    const h2_aec_calibration_config_t config = {
        .audio = c->rt->audio,
        .mem = c->rt->mem,
        .time = c->rt->time,
        .candidates = &c->pair,
        .candidate_count = 1,
        .selection = H2_AEC_CALIBRATION_SPEAKER_FIRST,
        .limits = h2_aec_calibration_default_limits(),
        .near_source = source,
        .near_source_user = c};
    if (rc == 0) rc = h2_aec_calibration_create(&config, &c->runner);
    const h2_aec_calibration_result_t* result = NULL;
    printf("H2_MFG_CALIBRATION_BEGIN uid=%s peer=%s ip=%s run=%016" PRIx64
           " speaker=%" PRIu32 " mic=%" PRIu32 " version=%s\n",
           c->uid, c->peer_uid, c->peer_ip, c->run, c->pair.speaker_percent,
           c->pair.mic_percent, esp_app_get_description()->version);
    fflush(stdout);
    if (rc == 0) rc = h2_aec_calibration_run(c->runner, &result);
    int reported = result == NULL
                       ? H2_PAL_ERR_UNAVAILABLE
                       : h2_aec_calibration_report(result, c->run, emit, NULL);
    int cleaned = release_runner(c);
    printf("H2_MFG_CALIBRATION_END uid=%s run=%016" PRIx64
           " rc=%d report=%d cleanup=%d retained=%d\n",
           c->uid, c->run, rc, reported, cleaned, cleaned != 0);
    fflush(stdout);
    (void)h2_pal_mutex_lock(c->rt->sync, c->mutex);
    c->retained = cleaned != 0;
    c->active = 0;
    (void)h2_pal_mutex_unlock(c->rt->sync, c->mutex);
  }
  (void)h2_pal_mutex_lock(c->rt->sync, c->mutex);
  c->running = 0;
  (void)h2_pal_semaphore_give(c->rt->sync, c->finished);
  (void)h2_pal_mutex_unlock(c->rt->sync, c->mutex);
  return H2_PAL_EXIT;
}

static void entry(void* unused) {
  (void)unused;
  calibration_t* c = &calibration;
  c->socket = -1;
  h2_runtime_config_t config;
  int rc = h2_esp_board_runtime_config(&config);
  if (rc == 0) rc = h2_runtime_init(&config, &c->rt);
  uint8_t mac[6];
  if (rc == 0 && esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) rc = H2_PAL_ERR_IO;
  if (rc == 0)
    (void)snprintf(c->uid, sizeof(c->uid), "%02x%02x%02x%02x%02x%02x", mac[0],
                   mac[1], mac[2], mac[3], mac[4], mac[5]);
  const h2_pal_mutex_config_t mutex = {
      .name = "mfg/aec-calibration", .allocator = rc == 0 ? c->rt->mem : NULL};
  const h2_pal_semaphore_config_t sem = {
      .name = "mfg/aec-finished",
      .max_count = 1,
      .allocator = rc == 0 ? c->rt->mem : NULL};
  if (rc == 0) rc = h2_pal_mutex_create(c->rt->sync, &mutex, &c->mutex);
  if (rc == 0) rc = h2_pal_semaphore_create(c->rt->sync, &sem, &c->finished);
  if (rc == 0) {
    const h2_esp_h2loader_config_t loader = {
        .board = c->rt->board,
        .user = c,
        .mfg_command = command,
        .run_mfg = run,
        .rearm_mfg = rearm,
        .before_disruptive = before_disruptive};
    h2_esp_h2loader_run_with_ble_config(c->rt, &loader);
  }
  printf("H2_MFG_CALIBRATION_SETUP_FAIL rc=%d\n", rc);
  fflush(stdout);
}

void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc == 0)
    rc = h2_esp_board_start_entry_task("mfg/aec-calibration", entry, NULL);
  if (rc != 0) printf("H2_MFG_CALIBRATION_SETUP_FAIL rc=%d\n", rc);
}
