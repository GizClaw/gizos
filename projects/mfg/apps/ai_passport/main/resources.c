#include "resources.h"
#include "recording.h"
#include "h2_atomic_static.h"
#if defined(H2_PASSPORT_RESOURCES_HOST_TEST)
#include "resources_sdk.h"
#else
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_platform_core.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#include <stdio.h>

/* Only the USB serve owner touches service handles. MFG publishes requests and
 * returns WOULD_BLOCK, releasing its state/operation locks before any joins. */
H2_ATOMIC_DEFINE_STATIC(int, desired, 0);
H2_ATOMIC_DEFINE_STATIC(int, active, 0); /* 0=off, 1=ready, 2=cleanup retained. */
H2_ATOMIC_DEFINE_STATIC(int, service_error, 0);
H2_ATOMIC_DEFINE_STATIC(size, failed_size, 0);
H2_ATOMIC_DEFINE_STATIC(uint, failed_caps, 0);

#if !defined(H2_PASSPORT_RESOURCES_HOST_TEST)
static void allocation_failed(size_t size, uint32_t caps, const char* function) {
  (void)function;
  h2_atomic_uint_store(&failed_caps, caps, H2_ATOMIC_RELAXED);
  h2_atomic_size_store(&failed_size, size, H2_ATOMIC_RELEASE);
}
#endif
static h2loader_app_command_service_api_t api;
static void* service;
static h2_runtime_t* rt;
static const h2_pal_sync_api_t* wifi_sync;
static h2_pal_mutex_t* wifi_mutex;
static uint64_t waiting_since;
static int waiting;
static int logged_check = -1;
static const h2loader_ble_command_service_config_t ble_config = {
    .inline_commands = true,
    .server_task_stack_size = 32768,
    .stream_buffer_size = 2048,
    .stream_window = 8,
    .input_frame_capacity = 8};

static void memory(const char* phase) {
  printf("H2_PASSPORT_RESOURCES phase=%s free=%u largest=%u stack_free=%u\n",
         phase,
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
         (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

int h2_passport_resources_init(h2_runtime_t* runtime) {
  if (runtime == NULL || service != NULL ||
      h2_atomic_int_load(&active, H2_ATOMIC_ACQUIRE) != 0)
    return H2_PAL_ERR_INVALID_STATE;
#if !defined(H2_PASSPORT_RESOURCES_HOST_TEST)
  if (heap_caps_register_failed_alloc_callback(allocation_failed) != 0)
    return H2_PAL_ERR_INVALID_STATE;
#endif
  int rc = h2_esp_h2loader_ble_command_service_with_config(&ble_config, &api);
  if (rc != 0) return rc;
  rt = runtime;
  wifi_sync = NULL;
  wifi_mutex = NULL;
  waiting = 0;
  logged_check = -1;
  h2_atomic_int_store(&desired, 0, H2_ATOMIC_RELEASE);
  h2_atomic_int_store(&service_error, 0, H2_ATOMIC_RELEASE);
  return 0;
}

void h2_passport_resources_bind_wifi(const h2_pal_sync_api_t* sync,
                                    h2_pal_mutex_t* mutex) {
  wifi_sync = sync;
  wifi_mutex = mutex;
}

static int stop_wifi(void) {
  int rc = wifi_mutex == NULL ? 0 : h2_pal_mutex_try_lock(wifi_sync, wifi_mutex);
  if (rc == H2_PAL_ERR_BUSY) rc = H2_PAL_ERR_WOULD_BLOCK;
  if (rc != 0) return rc;
  rc = h2_esp_platform_wifi_stop();
  if (wifi_mutex != NULL) {
    int unlock = h2_pal_mutex_unlock(wifi_sync, wifi_mutex);
    if (rc == 0) rc = unlock;
  }
  return rc == H2_PAL_ERR_BUSY ? H2_PAL_ERR_WOULD_BLOCK : rc;
}

static int pending(void) {
  uint64_t now = 0;
  int rc = h2_pal_time_get_monotonic_ms(rt->time, &now);
  if (rc != 0) return rc;
  if (!waiting) {
    waiting = 1;
    waiting_since = now;
  }
  if (now - waiting_since < 15000) return H2_PAL_ERR_WOULD_BLOCK;
  h2_atomic_int_store(&desired, 0, H2_ATOMIC_RELEASE);
  waiting = 0;
  return H2_PAL_ERR_TIMEOUT;
}

int h2_passport_prepare_check(void* user, h2_mfg_check_t check) {
  (void)user;
  if (rt == NULL) return H2_PAL_ERR_INVALID_STATE;
  int error = h2_atomic_int_exchange(&service_error, 0, H2_ATOMIC_ACQ_REL);
  if (error != 0) {
    h2_atomic_int_store(&desired, 0, H2_ATOMIC_RELEASE);
    waiting = 0;
    return error;
  }
  int ble = check == H2_MFG_BLE;
  if (!ble) {
    h2_atomic_int_store(&desired, 0, H2_ATOMIC_RELEASE);
    if (h2_atomic_int_load(&active, H2_ATOMIC_ACQUIRE) != 0) return pending();
  }
  int audio = check == H2_MFG_SPEAKER || check == H2_MFG_MIC || check == H2_MFG_AEC;
  int rc = audio ? h2_esp_board_audio_set_aec_enabled(check == H2_MFG_AEC)
                 : h2_esp_board_audio_release();
  if (rc == 0 && check != H2_MFG_WIFI) rc = stop_wifi();
  if (rc == 0 && check == H2_MFG_AEC) rc = h2_passport_recording_prepare();
  if (rc == H2_PAL_ERR_BUSY || rc == H2_PAL_ERR_WOULD_BLOCK) return pending();
  if (rc != 0) return rc;
  if (ble) {
    h2_atomic_int_store(&desired, 1, H2_ATOMIC_RELEASE);
    if (h2_atomic_int_load(&active, H2_ATOMIC_ACQUIRE) != 1) return pending();
  }
  waiting = 0;
  if (logged_check != (int)check) {
    char phase[24];
    (void)snprintf(phase, sizeof(phase), "check-%d", (int)check);
    memory(phase);
    logged_check = (int)check;
  }
  return 0;
}

int h2_passport_release_check(void* user) {
  (void)user;
  size_t size = h2_atomic_size_exchange(&failed_size, 0, H2_ATOMIC_ACQ_REL);
  if (size != 0)
    printf("H2_PASSPORT_ALLOC_FAIL size=%u caps=%08x\n", (unsigned)size,
           h2_atomic_uint_load(&failed_caps, H2_ATOMIC_ACQUIRE));
  memory("before-audio-release");
  int rc = h2_esp_board_audio_release();
  if (rc == 0) memory("after-audio-release");
  return rc;
}

static int close_service(h2_runtime_t* runtime) {
  if (service != NULL) {
    int rc = api.close(api.user, service);
    if (rc != 0) return rc;
    service = NULL;
  }
  int rc = h2_pal_ble_stop(runtime->ble_host);
  if (rc == 0) {
    h2_atomic_int_store(&active, 0, H2_ATOMIC_RELEASE);
    memory("ble-stopped");
  }
  return rc;
}

int h2_passport_resources_poll(void* user, h2_runtime_t* runtime,
                              const h2_loader_command_config_t* command) {
  (void)user;
  int state = h2_atomic_int_load(&active, H2_ATOMIC_ACQUIRE);
  if (state == 2 || (state == 1 &&
                    h2_atomic_int_load(&desired, H2_ATOMIC_ACQUIRE) == 0)) {
    int rc = close_service(runtime);
    if (rc != 0) {
      h2_atomic_int_store(&active, 2, H2_ATOMIC_RELEASE);
      if (rc != H2_PAL_ERR_BUSY && rc != H2_PAL_ERR_WOULD_BLOCK)
        h2_atomic_int_store(&service_error, rc, H2_ATOMIC_RELEASE);
    }
    return 0;
  }
  if (state == 0 && h2_atomic_int_load(&desired, H2_ATOMIC_ACQUIRE) == 1 &&
      h2_atomic_int_load(&service_error, H2_ATOMIC_ACQUIRE) == 0) {
    h2_atomic_int_store(&active, 2, H2_ATOMIC_RELEASE);
    int rc = api.open(api.user, runtime, command, runtime->board,
                     command->loader->config.hardware_capabilities, &service);
    if (rc == 0) {
      h2_atomic_int_store(&active, 1, H2_ATOMIC_RELEASE);
      memory("ble-ready");
    } else {
      (void)close_service(runtime);
      h2_atomic_int_store(&service_error, rc, H2_ATOMIC_RELEASE);
    }
  }
  return 0;
}

int h2_passport_resources_stop(void* user, h2_runtime_t* runtime) {
  (void)user;
  h2_atomic_int_store(&desired, 0, H2_ATOMIC_RELEASE);
  return h2_atomic_int_load(&active, H2_ATOMIC_ACQUIRE) == 0
             ? 0 : close_service(runtime);
}
