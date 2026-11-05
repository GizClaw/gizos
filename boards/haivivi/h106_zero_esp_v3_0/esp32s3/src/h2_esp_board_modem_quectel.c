#include "h2_esp_board_modem_handshake.h"
#include "h2_esp_board_modem_lifecycle.h"
#include "h2_esp_board_modem_audio.h"
#include "h2_esp_board_modem_ri.h"
#include "h2_esp_board_modem_sleep.h"
#include "h2_esp_board_modem_poll.h"
#include "h2_esp_board_private.h"
#include "h2_esp_board_sleep_gpio.h"

#include "h2_esp_board_config.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_zero_esp_board_internal.h"
#include "h2_apn.h"
#include "h2_quectel_modem.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/uart.h"
#include "soc/soc.h"
#include "esp_event.h"
#include "esp_err.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_modem_api.h"
#include "esp_modem_c_api_types.h"
#include "esp_modem_config.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <inttypes.h>
#include <stdbool.h>
#include "h2_atomic.h"
#include "h2_atomic_static.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* TAG = "h2_zero_esp_modem";

static const uart_port_t H2_ZERO_ESP_MODEM_UART = UART_NUM_1;
static const gpio_num_t H2_ZERO_ESP_MODEM_ENABLE_GPIO = GPIO_NUM_12;
static const gpio_num_t H2_ZERO_ESP_MODEM_TX_GPIO = GPIO_NUM_17;
static const gpio_num_t H2_ZERO_ESP_MODEM_RX_GPIO = GPIO_NUM_18;
static const int H2_ZERO_ESP_MODEM_BAUD = 115200;
static const int H2_ZERO_ESP_MODEM_CMUX_BAUD = 921600;

#define H2_ZERO_ESP_MODEM_PPP_GOT_IP_BIT BIT0
#define H2_ZERO_ESP_MODEM_PPP_STOPPED_BIT BIT1
#define H2_ZERO_ESP_MODEM_PPP_ERROR_BIT BIT2
#define H2_ZERO_ESP_MODEM_CALL_MONITOR_STOP_BIT BIT0
#define H2_ZERO_ESP_MODEM_CALL_MONITOR_EXITED_BIT BIT1
#define H2_ZERO_ESP_MODEM_CALL_MONITOR_CONTROLLER_UPDATE_BIT BIT0
#define H2_ZERO_ESP_MODEM_CALL_MONITOR_CONTROLLER_WORKER_EXITED_BIT BIT1
#define H2_ZERO_ESP_MODEM_CALL_MONITOR_CONTROLLER_STOP_BIT BIT2
#define H2_ZERO_ESP_MODEM_CALL_MONITOR_INTERVAL_MS 1000u
#define H2_ZERO_ESP_MODEM_CALL_MONITOR_STOP_TIMEOUT_MS 6000u
#define H2_ZERO_ESP_MODEM_CALL_MONITOR_TASK_STACK_SIZE 8192u
#define H2_ZERO_ESP_MODEM_CALL_MONITOR_CONTROLLER_STACK_SIZE 8192u
#define H2_ZERO_ESP_MODEM_URC_PORT_TIMEOUT_MS 1000u
#define H2_ZERO_ESP_MODEM_URC_PORT_ATTEMPTS 3u
#define H2_ZERO_ESP_MODEM_URC_PORT_RETRY_DELAY_MS 100u
#ifndef CONFIG_ESP_MODEM_C_API_STR_MAX
#define CONFIG_ESP_MODEM_C_API_STR_MAX 128
#endif
#define H2_ZERO_ESP_MODEM_AT_RESPONSE_SIZE \
  ((size_t)CONFIG_ESP_MODEM_C_API_STR_MAX)

/* AT/PPP workers own this state; the RI ISR uses only its Internal mailbox
 * below. */
static EXT_RAM_BSS_ATTR h2_quectel_modem_t s_modem;
static bool s_modem_initialized;
/* Static ISR mailbox: coalesced edges survive busy AT transactions. */
static portMUX_TYPE s_ri_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_ri_pending;
H2_ATOMIC_DEFINE_STATIC(bool, s_ri_paused, false);
H2_ATOMIC_DEFINE_STATIC(int, s_sleep_state, H2_ZERO_ESP_MODEM_SLEEP_PENDING);
H2_ATOMIC_DEFINE_STATIC(bool, s_modem_accessed, false);
H2_ATOMIC_DEFINE_STATIC(bool, s_modem_powered, false);
static RTC_NOINIT_ATTR uint32_t s_modem_power_marker;
/* Held only around GPIO/atomic operations, never AT, logging or worker waits.
 */
static portMUX_TYPE s_power_gpio_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_ri_installed;
H2_ATOMIC_DEFINE_STATIC(bool, s_ri_handler_installed, false);
/* The Main-thread mailbox owns final input restoration and level latching. */
static bool s_standby_ri_input_ready;
static bool s_standby_ri_was_low;
static bool s_standby_ri_restore_error_logged;
static h2_pal_modem_call_status_t s_incoming_posted;
void h2_esp_board_modem_poll(void);

static void IRAM_ATTR modem_ri_isr(void* user) {
  (void)user;
  taskENTER_CRITICAL_ISR(&s_ri_lock);
  s_ri_pending = true;
  taskEXIT_CRITICAL_ISR(&s_ri_lock);
}

static void modem_ri_notify(void) {
  taskENTER_CRITICAL(&s_ri_lock);
  s_ri_pending = true;
  taskEXIT_CRITICAL(&s_ri_lock);
}

static bool modem_ri_take(void) {
  taskENTER_CRITICAL(&s_ri_lock);
  const bool pending = s_ri_pending;
  s_ri_pending = false;
  taskEXIT_CRITICAL(&s_ri_lock);
  return pending;
}

static esp_err_t modem_ri_start(void) {
  const esp_err_t err = h2_board_modem_ri_start(&s_ri_installed, modem_ri_isr);
  h2_atomic_store(&s_ri_handler_installed, s_ri_installed);
  if (err != ESP_OK) return err;
  h2_atomic_store(&s_ri_paused, false);
  return ESP_OK;
}

static esp_modem_dce_t* s_dce;
static esp_netif_t* s_ppp_netif;
static EventGroupHandle_t s_ppp_events;
static esp_event_handler_instance_t s_ip_got_instance;
static esp_event_handler_instance_t s_ip_lost_instance;
static esp_event_handler_instance_t s_ppp_status_instance;
static portMUX_TYPE s_call_monitor_lock = portMUX_INITIALIZER_UNLOCKED;
static h2_pal_modem_call_status_t s_call_monitor_status;
static h2_pal_result_t (*s_provider_get_call_status)(
    void*, h2_pal_modem_call_status_t*);
static h2_pal_result_t modem_get_call_status(void* user,
                                             h2_pal_modem_call_status_t* out);
H2_ATOMIC_DEFINE_STATIC(bool, s_modem_call_audio_active, false);
H2_ATOMIC_DEFINE_STATIC(bool, s_codec_pa_requested, false);
H2_ATOMIC_DEFINE_STATIC(uint, s_call_audio_route_generation, 0u);
static bool s_cmux_enabled;
/* Transport state survives provider SIM invalidation of data_status. */
static bool s_ppp_data_mode;
H2_ATOMIC_DEFINE_STATIC(bool, s_ppp_close_requested, false);
static bool s_apn_auto_selected;
static bool s_dce_apn_configured;
/* Module baud is independent of the lifetime of the local UART transport. */
static int s_modem_baud = 115200;

#ifdef CONFIG_ESP_MODEM_USE_INFLATABLE_BUFFER_IF_NEEDED
#error "CMUX RX tap assumes the URC hook receives each CMUX fragment once"
#endif
#define H2_ZERO_ESP_MODEM_RX_COMMAND_MAX 48u

typedef enum {
  ZERO_ESP_MODEM_RX_OFF = 0,
  ZERO_ESP_MODEM_RX_UART,
  ZERO_ESP_MODEM_RX_CMUX,
} zero_esp_modem_rx_source_t;

static portMUX_TYPE s_rx_lock = portMUX_INITIALIZER_UNLOCKED;
static zero_esp_modem_rx_source_t s_rx_source;
static uint32_t s_rx_epoch;
static char s_rx_command[H2_ZERO_ESP_MODEM_RX_COMMAND_MAX];
/* Owned by the esp_modem UART task. */
static h2_modem_rx_t s_rx_receiver;
static uint32_t s_rx_receiver_epoch;
static TickType_t s_rx_full_log_ticks;

static h2_pal_result_t zero_esp_modem_deinit(void* user);
static h2_pal_result_t modem_destroy_transport(uint32_t timeout_ms);
static h2_pal_result_t zero_esp_modem_set_apn(
    h2_pal_modem_t* platform, const h2_pal_modem_apn_config_t* config);
static h2_pal_result_t zero_esp_modem_open(h2_pal_modem_t* platform,
                                           uint32_t timeout_ms);
static h2_pal_result_t zero_esp_modem_close(h2_pal_modem_t* platform,
                                            uint32_t timeout_ms);
static h2_pal_result_t zero_esp_modem_data_open(h2_pal_modem_t* platform,
                                                uint32_t timeout_ms);
static h2_pal_result_t zero_esp_modem_data_close(h2_pal_modem_t* platform,
                                                 uint32_t timeout_ms);
static void zero_esp_modem_post_event(h2_pal_system_event_type_t type,
                                      const void* payload, size_t payload_size);

/* Only the explicit PAL open adapter grants permission to energize the rail.
 * Background AT, SIM maintenance and data-open cannot create a transport. */
H2_ATOMIC_DEFINE_STATIC(bool, s_power_requested, false);
H2_ATOMIC_DEFINE_STATIC(bool, s_cleanup_required, false);
H2_ATOMIC_DEFINE_STATIC(bool, s_open_ready, false);
H2_ATOMIC_DEFINE_STATIC(ptr, s_open_task, NULL);
static TickType_t s_open_started;
static uint32_t s_open_timeout_ms;
static uint32_t remaining_timeout_ms(uint32_t timeout_ms,
                                     TickType_t start_ticks);
static h2_pal_result_t modem_enable_set_level(int level);

H2_ATOMIC_DEFINE_STATIC(ptr, s_sdk_wait_task, NULL);

static h2_pal_result_t modem_operation_lock(uint32_t timeout_ms) {
  /* GizOS creates a recursive operation mutex. Open/close/data adapters and
   * provider operations can re-enter command on the same task; recursive
   * try-lock succeeds immediately and each acquisition has one release. */
  if (timeout_ms == 0u)
    return h2_pal_mutex_try_lock(s_modem.config.sync_api,
                                 s_modem.operation_lock);
  const TickType_t start = xTaskGetTickCount();
  for (;;) {
    const h2_pal_result_t rc =
        h2_pal_mutex_try_lock(s_modem.config.sync_api, s_modem.operation_lock);
    if (rc != H2_PAL_ERR_WOULD_BLOCK) return rc;
    if (remaining_timeout_ms(timeout_ms, start) == 0u)
      return H2_PAL_ERR_TIMEOUT;
    vTaskDelay(1);
  }
}

static h2_pal_result_t modem_operation_unlock(void) {
  return h2_pal_mutex_unlock(s_modem.config.sync_api, s_modem.operation_lock);
}

static h2_pal_result_t modem_operation_allowed(void* user) {
  (void)user;
  if (!h2_zero_esp_modem_sleep_admit(&s_sleep_state)) return H2_PAL_ERR_CLOSED;
  if (!h2_atomic_load(&s_power_requested)) return H2_PAL_ERR_CLOSED;
  return H2_PAL_OK;
}

static h2_pal_modem_t* zero_esp_modem_platform_from_user(void* user) {
  h2_quectel_modem_t* modem = (h2_quectel_modem_t*)user;
  return modem != NULL ? &modem->platform : NULL;
}

static h2_pal_result_t zero_esp_modem_set_apn_adapter(
    void* user, const h2_pal_modem_apn_config_t* config) {
  const h2_pal_result_t admission = modem_operation_allowed(NULL);
  if (admission != H2_PAL_OK) return admission;
  /* Share the provider's recursive operation lock across the entire board
   * transaction: individual AT commands cannot serialize baud/CMUX changes. */
  h2_pal_result_t rc = modem_operation_lock(15000u);
  if (rc != H2_PAL_OK) return rc;
  rc = zero_esp_modem_set_apn(zero_esp_modem_platform_from_user(user), config);
  const h2_pal_result_t unlock_rc = modem_operation_unlock();
  return rc != H2_PAL_OK ? rc : unlock_rc;
}

static h2_pal_result_t zero_esp_modem_open_run(void* user,
                                               uint32_t timeout_ms) {
  const TickType_t start = xTaskGetTickCount();
  h2_atomic_ptr_store(&s_open_task, xTaskGetCurrentTaskHandle(),
                      H2_ATOMIC_SEQ_CST);
  h2_pal_result_t rc = modem_operation_lock(timeout_ms);
  if (rc != H2_PAL_OK) {
    h2_atomic_ptr_store(&s_open_task, NULL, H2_ATOMIC_SEQ_CST);
    return rc;
  }
  rc = h2_pal_mutex_lock(s_modem.config.sync_api, s_modem.lock);
  if (rc == H2_PAL_OK) {
    const bool ota_hold = s_modem.ota_hold != 0u;
    rc = h2_pal_mutex_unlock(s_modem.config.sync_api, s_modem.lock);
    if (rc == H2_PAL_OK && ota_hold) rc = H2_PAL_ERR_BUSY;
  }
  if (rc != H2_PAL_OK) {
    (void)modem_operation_unlock();
    h2_atomic_ptr_store(&s_open_task, NULL, H2_ATOMIC_SEQ_CST);
    return rc;
  }
  if (!h2_atomic_load(&s_power_requested)) {
    (void)modem_operation_unlock();
    h2_atomic_ptr_store(&s_open_task, NULL, H2_ATOMIC_SEQ_CST);
    return H2_PAL_ERR_CLOSED;
  }
  if (h2_atomic_load(&s_cleanup_required)) {
    ESP_LOGW(TAG, "H2_H106_CELLULAR stage=stale_cleanup_before_open");
    rc = zero_esp_modem_close(zero_esp_modem_platform_from_user(user),
                              timeout_ms);
    if (rc != H2_PAL_OK) {
      h2_atomic_ptr_store(&s_open_task, NULL, H2_ATOMIC_SEQ_CST);
      const h2_pal_result_t unlock_rc = modem_operation_unlock();
      if (unlock_rc != H2_PAL_OK)
        ESP_LOGE(TAG, "modem unlock failed rc=%d", unlock_rc);
      return rc;
    }
    if ((uint32_t)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS) >=
        timeout_ms) {
      h2_atomic_ptr_store(&s_open_task, NULL, H2_ATOMIC_SEQ_CST);
      const h2_pal_result_t unlock_rc = modem_operation_unlock();
      if (unlock_rc != H2_PAL_OK)
        ESP_LOGE(TAG, "modem unlock failed rc=%d", unlock_rc);
      return H2_PAL_ERR_TIMEOUT;
    }
  }
  h2_atomic_ptr_store(&s_open_task, xTaskGetCurrentTaskHandle(),
                      H2_ATOMIC_SEQ_CST);
  s_open_started = start;
  s_open_timeout_ms = timeout_ms;
  const uint32_t elapsed = (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
  ESP_LOGW(TAG, "H2_H106_CELLULAR stage=open_begin task=%s",
           pcTaskGetName(NULL));
  rc = elapsed >= timeout_ms
           ? H2_PAL_ERR_TIMEOUT
           : zero_esp_modem_open(zero_esp_modem_platform_from_user(user),
                                 timeout_ms - elapsed);
  if (rc == H2_PAL_OK && !h2_atomic_load(&s_power_requested))
    rc = H2_PAL_ERR_CLOSED;
  if (rc != H2_PAL_OK) {
    h2_atomic_store(&s_cleanup_required, true);
    h2_atomic_store(&s_power_requested, false);
    (void)modem_enable_set_level(0);
    const h2_pal_result_t cleanup_rc = zero_esp_modem_close(
        zero_esp_modem_platform_from_user(user), timeout_ms);
    ESP_LOGW(TAG,
             "H2_H106_CELLULAR stage=open_cleanup rc=%d dce=%p cmux=%d ppp=%d",
             cleanup_rc, (void*)s_dce, s_cmux_enabled, s_ppp_data_mode);
  }
  h2_atomic_ptr_store(&s_open_task, NULL, H2_ATOMIC_SEQ_CST);
  s_open_started = 0;
  s_open_timeout_ms = 0u;
  const h2_pal_result_t unlock_rc = modem_operation_unlock();
  if (unlock_rc != H2_PAL_OK)
    ESP_LOGE(TAG, "modem unlock failed rc=%d", unlock_rc);
  return rc != H2_PAL_OK ? rc : unlock_rc;
}

typedef struct {
  void* user;
  TickType_t started;
} modem_close_context_t;

static h2_pal_result_t modem_close_admit(void) {
  return h2_quectel_modem_transport_closing(&s_modem);
}

static h2_pal_result_t modem_close_admitted(void* user, uint32_t timeout_ms) {
  const modem_close_context_t* context = user;
  h2_atomic_store(&s_cleanup_required, true);
  h2_atomic_store(&s_power_requested, false);
  const uint32_t remaining =
      timeout_ms == 0u ? 15000u
                       : remaining_timeout_ms(timeout_ms, context->started);
  return remaining == 0u
             ? H2_PAL_ERR_TIMEOUT
             : zero_esp_modem_close(
                   zero_esp_modem_platform_from_user(context->user), remaining);
}

static h2_pal_result_t zero_esp_modem_close_run(void* user,
                                                uint32_t timeout_ms) {
  modem_close_context_t context = {user, xTaskGetTickCount()};
  const h2_board_modem_close_ops_t ops = {
      modem_operation_lock, modem_close_admit, modem_close_admitted,
      modem_operation_unlock};
  return h2_board_modem_close_run(&ops, &context, timeout_ms);
}

static void modem_failed_open(void* user, h2_pal_result_t error) {
  (void)user;
  h2_atomic_store(&s_cleanup_required, true);
  h2_atomic_store(&s_power_requested, false);
  (void)modem_enable_set_level(0);
  ESP_LOGW(TAG, "H2_H106_CELLULAR stage=lifecycle_failed on=1 rc=%d", error);
}

/* The app modem task owns the complete lifecycle, including SDK teardown. */
static h2_pal_result_t modem_lifecycle(void* user, uint32_t timeout_ms,
                                       bool opening) {
  portENTER_CRITICAL(&s_power_gpio_lock);
  const bool allowed = h2_zero_esp_modem_sleep_admit(&s_sleep_state);
  if (allowed) {
    if (!h2_atomic_load(&s_modem_accessed))
      h2_atomic_store(&s_modem_powered, h2_esp_board_modem_is_powered());
    h2_atomic_store(&s_modem_accessed, true);
    if (opening) h2_atomic_store(&s_power_requested, true);
  }
  portEXIT_CRITICAL(&s_power_gpio_lock);
  if (!allowed) return H2_PAL_ERR_CLOSED;
  if (opening && h2_atomic_load(&s_open_ready) &&
      h2_atomic_load(&s_power_requested) &&
      !h2_atomic_load(&s_cleanup_required)) {
    ESP_LOGW(TAG, "H2_H106_CELLULAR stage=open_already_ready rc=0");
    return H2_PAL_OK;
  }
  const h2_board_modem_lifecycle_ops_t ops = {
      zero_esp_modem_open_run, zero_esp_modem_close_run, modem_failed_open};
  return h2_board_modem_lifecycle_run(&ops, user, timeout_ms, opening);
}

static h2_pal_result_t zero_esp_modem_open_adapter(void* user,
                                                   uint32_t timeout_ms) {
  return modem_lifecycle(user, timeout_ms, true);
}

static h2_pal_result_t zero_esp_modem_close_adapter(void* user,
                                                    uint32_t timeout_ms) {
  return modem_lifecycle(user, timeout_ms, false);
}

static h2_pal_result_t zero_esp_modem_data_open_adapter(void* user,
                                                        uint32_t timeout_ms) {
  const h2_pal_result_t admission = modem_operation_allowed(NULL);
  if (admission != H2_PAL_OK) return admission;
  /* The bounded data attempt includes waiting for the shared operation lock. */
  const TickType_t start = xTaskGetTickCount();
  h2_pal_result_t rc = modem_operation_lock(timeout_ms);
  if (rc != H2_PAL_OK) return rc;
  const uint32_t remaining = remaining_timeout_ms(timeout_ms, start);
  rc = remaining == 0u
           ? H2_PAL_ERR_TIMEOUT
           : zero_esp_modem_data_open(zero_esp_modem_platform_from_user(user),
                                      remaining);
  const h2_pal_result_t unlock_rc = modem_operation_unlock();
  return rc != H2_PAL_OK ? rc : unlock_rc;
}

static h2_pal_result_t zero_esp_modem_data_close_adapter(void* user,
                                                         uint32_t timeout_ms) {
  const h2_pal_result_t admission = modem_operation_allowed(NULL);
  if (admission != H2_PAL_OK) return admission;
  /* Share the provider's recursive operation lock across the entire board
   * transaction: individual AT commands cannot serialize baud/CMUX changes. */
  h2_pal_result_t rc = modem_operation_lock(timeout_ms);
  if (rc != H2_PAL_OK) return rc;
  rc = zero_esp_modem_data_close(zero_esp_modem_platform_from_user(user),
                                 timeout_ms);
  const h2_pal_result_t unlock_rc = modem_operation_unlock();
  return rc != H2_PAL_OK ? rc : unlock_rc;
}

bool h2_esp_zero_esp_board_modem_call_audio_active(void) {
  return h2_atomic_load_explicit(&s_modem_call_audio_active, H2_ATOMIC_ACQUIRE);
}

static int zero_esp_modem_apply_audio_pa(void) {
  for (;;) {
    const unsigned int generation = h2_atomic_load_explicit(
        &s_call_audio_route_generation, H2_ATOMIC_ACQUIRE);
    const bool enabled =
        h2_esp_zero_esp_board_modem_call_audio_active() ||
        h2_atomic_load_explicit(&s_codec_pa_requested, H2_ATOMIC_ACQUIRE);
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << H2_ZERO_ESP_AUDIO_PA_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err == ESP_OK) {
      err = gpio_set_level((gpio_num_t)H2_ZERO_ESP_AUDIO_PA_GPIO,
                           enabled ? 1 : 0);
    }
    if (err != ESP_OK) {
      return H2_AUDIO_ERR_IO;
    }
    if (generation == h2_atomic_load_explicit(&s_call_audio_route_generation,
                                              H2_ATOMIC_ACQUIRE)) {
      return H2_AUDIO_OK;
    }
  }
}

int h2_esp_zero_esp_board_codec_pa_set(void* user, int enabled) {
  (void)user;
  h2_atomic_store_explicit(&s_codec_pa_requested, enabled != 0,
                           H2_ATOMIC_RELEASE);
  h2_atomic_fetch_add_explicit(&s_call_audio_route_generation, 1u,
                               H2_ATOMIC_ACQ_REL);
  return zero_esp_modem_apply_audio_pa();
}

static void zero_esp_modem_set_call_audio_route(bool active) {
  h2_atomic_store_explicit(&s_modem_call_audio_active, active,
                           H2_ATOMIC_RELEASE);
  h2_atomic_fetch_add_explicit(&s_call_audio_route_generation, 1u,
                               H2_ATOMIC_ACQ_REL);
  const gpio_config_t config = {
      .pin_bit_mask = 1ULL << H2_ZERO_ESP_AUDIO_MODEM_ROUTE_GPIO,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&config);
  if (err == ESP_OK) {
    err = gpio_set_level((gpio_num_t)H2_ZERO_ESP_AUDIO_MODEM_ROUTE_GPIO,
                         active ? H2_ZERO_ESP_AUDIO_MODEM_ROUTE_CALL_LEVEL
                                : H2_ZERO_ESP_AUDIO_MODEM_ROUTE_CODEC_LEVEL);
  }
  if (err == ESP_OK) {
    err = zero_esp_modem_apply_audio_pa() == H2_AUDIO_OK ? ESP_OK : ESP_FAIL;
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "call audio route=%d pa=%d failed: %s", active ? 1 : 0,
             active ? 1 : 0, esp_err_to_name(err));
  }
}

static void zero_esp_modem_call_monitor_observe(
    const h2_pal_modem_call_status_t* status, bool active) {
  (void)active;
  taskENTER_CRITICAL(&s_call_monitor_lock);
  s_call_monitor_status = *status;
  taskEXIT_CRITICAL(&s_call_monitor_lock);
}

static int zero_esp_modem_system_event_post(void* user,
                                            const h2_pal_system_event_t* event,
                                            uint32_t timeout_ms) {
  (void)user;
  if (event != NULL && event->payload != NULL &&
      event->payload_size >= sizeof(h2_pal_modem_call_event_t)) {
    const h2_pal_modem_call_event_t* call = event->payload;
    taskENTER_CRITICAL(&s_call_monitor_lock);
    const bool duplicate =
        event->type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING &&
        s_incoming_posted.call_id == call->call.call_id &&
        (call->call.number[0] == '\0' ||
         strcmp(s_incoming_posted.number, call->call.number) == 0);
    taskEXIT_CRITICAL(&s_call_monitor_lock);
    if (duplicate) return H2_PAL_OK;

    if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED &&
        call->call.state != H2_PAL_MODEM_CALL_STATE_IDLE &&
        call->call.state != H2_PAL_MODEM_CALL_STATE_ENDED &&
        call->call.state != H2_PAL_MODEM_CALL_STATE_INCOMING) {
      zero_esp_modem_set_call_audio_route(true);
      zero_esp_modem_call_monitor_observe(&call->call, true);
    } else if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED ||
               (event->type ==
                    H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED &&
                call->call.state == H2_PAL_MODEM_CALL_STATE_ENDED)) {
      zero_esp_modem_set_call_audio_route(false);
      zero_esp_modem_call_monitor_observe(&call->call, false);
    }
  }
  const int posted = h2_pal_system_event_post(h2_esp_board_system_event_api(),
                                              event, timeout_ms);
  if (event != NULL &&
      event->type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING)
    ESP_LOGW(TAG, "call URC parsed runtime_post_rc=%d", posted);
  if (posted == H2_PAL_OK && event != NULL && event->payload != NULL &&
      event->payload_size >= sizeof(h2_pal_modem_call_event_t)) {
    const h2_pal_modem_call_event_t* call = event->payload;
    taskENTER_CRITICAL(&s_call_monitor_lock);
    if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING)
      s_incoming_posted = call->call;
    else if (event->type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED &&
             s_incoming_posted.call_id == call->call.call_id)
      memset(&s_incoming_posted, 0, sizeof(s_incoming_posted));
    taskEXIT_CRITICAL(&s_call_monitor_lock);
  }
  return posted;
}

static h2_pal_result_t modem_get_call_status(void* user,
                                             h2_pal_modem_call_status_t* out) {
  h2_pal_modem_call_status_t previous;
  taskENTER_CRITICAL(&s_call_monitor_lock);
  previous = s_call_monitor_status;
  if (s_incoming_posted.state == H2_PAL_MODEM_CALL_STATE_INCOMING)
    previous = s_incoming_posted;
  taskEXIT_CRITICAL(&s_call_monitor_lock);
  const h2_pal_result_t rc = s_provider_get_call_status(user, out);
  if (rc == H2_PAL_OK && out->state == H2_PAL_MODEM_CALL_STATE_IDLE &&
      previous.state != H2_PAL_MODEM_CALL_STATE_IDLE &&
      previous.state != H2_PAL_MODEM_CALL_STATE_ENDED) {
    *out = previous;
    out->state = H2_PAL_MODEM_CALL_STATE_ENDED;
    const h2_pal_modem_call_event_t payload = {.call = *out};
    const h2_pal_system_event_t event = {
        .type = H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED,
        .payload = &payload,
        .payload_size = sizeof(payload),
    };
    ESP_LOGW(TAG, "H2_MODEM_CALL poll_end call_id=%ld", (long)out->call_id);
    (void)zero_esp_modem_system_event_post(NULL, &event, 0u);
  }
  return rc;
}

static const h2_pal_system_event_vtable_t s_modem_system_event_vtable = {
    .post = zero_esp_modem_system_event_post,
};

static const h2_pal_system_event_api_t s_modem_system_events = {
    .user = NULL,
    .vtable = &s_modem_system_event_vtable,
};

/*
 * AT RX tap.  esp_modem 1.4.3 passes its URC hook the cumulative DTE buffer
 * in UART command mode (reset only when a command ends), so the hook cannot
 * tell new bytes from replayed ones.  In UART mode the physical bytes come
 * from uart_read_bytes instead (linked with --wrap, see CMakeLists.txt).  In
 * CMUX mode those bytes are frames; the hook then receives each AT-channel
 * payload exactly once and becomes the source.  Both producers run on the
 * esp_modem UART task, so s_rx_receiver has a single writer.
 */
static void zero_esp_modem_rx_feed(zero_esp_modem_rx_source_t source,
                                   const uint8_t* data, size_t len) {
  if (!h2_atomic_load(&s_power_requested) || data == NULL || len == 0u) {
    return;
  }
  char command[H2_ZERO_ESP_MODEM_RX_COMMAND_MAX];
  taskENTER_CRITICAL(&s_rx_lock);
  const bool active = s_rx_source == source;
  const uint32_t epoch = s_rx_epoch;
  memcpy(command, s_rx_command, sizeof(command));
  taskEXIT_CRITICAL(&s_rx_lock);
  if (!active) {
    return;
  }
  if (epoch != s_rx_receiver_epoch) {
    memset(&s_rx_receiver, 0, sizeof(s_rx_receiver));
    s_rx_receiver_epoch = epoch;
  }
  const h2_pal_result_t rc =
      h2_quectel_rx_feed(&s_modem, &s_rx_receiver, s_rx_receiver.next_offset,
                         data, len, command[0] != '\0' ? command : NULL);
  if (rc == H2_PAL_ERR_FULL) {
    const TickType_t now = xTaskGetTickCount();
    if (now - s_rx_full_log_ticks >= pdMS_TO_TICKS(5000u)) {
      s_rx_full_log_ticks = now;
      h2_modem_urc_stats_t stats = {0};
      (void)h2_modem_urc_get_stats(&s_modem.urc_worker, &stats);
      ESP_LOGW(TAG,
               "modem URC queue full accepted=%" PRIu32 " handled=%" PRIu32
               " full=%" PRIu32,
               stats.accepted, stats.handled, stats.full);
    }
  }
}

/* Switching source or command channel restarts line framing: a retained
 * tail belongs to the previous stream. */
static void zero_esp_modem_rx_set_source(zero_esp_modem_rx_source_t source) {
  taskENTER_CRITICAL(&s_rx_lock);
  s_rx_source = source;
  s_rx_epoch++;
  s_rx_command[0] = '\0';
  taskEXIT_CRITICAL(&s_rx_lock);
}

static void zero_esp_modem_rx_restart(void) {
  taskENTER_CRITICAL(&s_rx_lock);
  s_rx_epoch++;
  taskEXIT_CRITICAL(&s_rx_lock);
}

/* Solicited replies share prefixes with URCs (+CPIN, +CSQ, ...); the
 * classifier needs the command that owns the bytes being received. */
static void zero_esp_modem_rx_set_command(const char* cmd) {
  taskENTER_CRITICAL(&s_rx_lock);
  if (cmd == NULL) {
    s_rx_command[0] = '\0';
  } else {
    strlcpy(s_rx_command, cmd, sizeof(s_rx_command));
  }
  taskEXIT_CRITICAL(&s_rx_lock);
}

static h2_pal_result_t modem_at_prepare(void) {
  if (!h2_zero_esp_modem_sleep_admit(&s_sleep_state)) return H2_PAL_ERR_CLOSED;
  /* QSCLK is enabled only during deep-sleep prepare. Once DTR permits
   * sleep, skip ordinary commands; only transport open owns waking. */
  if (gpio_get_level(H2_ZERO_ESP_MODEM_DTR_INVERTED_GPIO) == 0) {
    ESP_LOGW(TAG, "H2_MODEM_AT skipped reason=modem_sleeping");
    return H2_PAL_ERR_UNAVAILABLE;
  }
  return H2_PAL_OK;
}

/* Internal ESP adapter preserves PAL gate failures through map_esp_error.
 * ESP errors are nonnegative except ESP_FAIL (-1); reserve a private range. */
#define MODEM_AT_GATE_ERROR_BASE 0x70000
static esp_err_t modem_at_gate_error(h2_pal_result_t rc) {
  return MODEM_AT_GATE_ERROR_BASE - (int)rc;
}

/* Quectel modules emit "RDY" and later "APP RDY" after a rail power-on; the
 * provider treats both as a module reset, so configuration must not start
 * until "APP RDY" has passed. Matched on the raw UART stream. */
H2_ATOMIC_DEFINE_STATIC(bool, s_app_rdy_seen, false);
H2_ATOMIC_DEFINE_STATIC(bool, s_rail_powered_this_open, false);
static size_t s_app_rdy_match;
static void app_rdy_scan(const uint8_t* buf, size_t len) {
  static const char pattern[] = "APP RDY";
  for (size_t i = 0; i < len; ++i) {
    const char c = (char)buf[i];
    if (c == pattern[s_app_rdy_match]) {
      if (++s_app_rdy_match == sizeof(pattern) - 1u) {
        h2_atomic_store(&s_app_rdy_seen, true);
        s_app_rdy_match = 0;
      }
    } else {
      s_app_rdy_match = c == pattern[0] ? 1u : 0u;
    }
  }
}
static esp_err_t zero_esp_modem_at_raw(const char* cmd, char* response,
                                       uint32_t timeout_ms) {
  if (!h2_atomic_load(&s_power_requested) || s_dce == NULL)
    return ESP_ERR_INVALID_STATE;
  if (h2_atomic_ptr_load(&s_open_task, H2_ATOMIC_SEQ_CST) ==
      xTaskGetCurrentTaskHandle()) {
    const uint32_t elapsed =
        (xTaskGetTickCount() - s_open_started) * portTICK_PERIOD_MS;
    if (elapsed >= s_open_timeout_ms) return ESP_ERR_TIMEOUT;
    const uint32_t remaining = s_open_timeout_ms - elapsed;
    if (timeout_ms > remaining) timeout_ms = remaining;
  }
  const esp_err_t baud_rc =
      uart_set_baudrate(H2_ZERO_ESP_MODEM_UART, s_modem_baud);
  if (baud_rc != ESP_OK) return baud_rc;
  zero_esp_modem_rx_set_command(cmd);
  h2_atomic_ptr_store(&s_sdk_wait_task, xTaskGetCurrentTaskHandle(),
                      H2_ATOMIC_SEQ_CST);
  const esp_err_t err = esp_modem_at(s_dce, cmd, response, (int)timeout_ms);
  h2_atomic_ptr_store(&s_sdk_wait_task, NULL, H2_ATOMIC_SEQ_CST);
  zero_esp_modem_rx_set_command(NULL);
  if (err == ESP_ERR_TIMEOUT) {
    ESP_LOGW(TAG, "modem AT timeout baud=%d cmux=%d", s_modem_baud,
             s_cmux_enabled);
  }
  return h2_atomic_load(&s_power_requested) ? err : ESP_ERR_INVALID_STATE;
}

/* All ordinary AT, including board-local commands, enters through this gate.
 * Only the wake handshake itself uses raw AT to avoid recursive waking. */
static esp_err_t zero_esp_modem_at(const char* cmd, char* response,
                                   uint32_t timeout_ms) {
  response[0] = '\0';
  if (!h2_atomic_load(&s_power_requested) || s_dce == NULL)
    return ESP_ERR_INVALID_STATE;
  const h2_pal_result_t rc = modem_at_prepare();
  if (rc != H2_PAL_OK) return modem_at_gate_error(rc);
  return zero_esp_modem_at_raw(cmd, response, timeout_ms);
}

static int modem_audio_at(void* user, const char* cmd, char* response,
                          unsigned size) {
  (void)user;
  char buffer[CONFIG_ESP_MODEM_C_API_STR_MAX] = {0};
  const esp_err_t rc = zero_esp_modem_at(cmd, buffer, 1000u);
  buffer[sizeof(buffer) - 1u] = '\0';
  if (size > 0u) snprintf(response, size, "%s", buffer);
  return (int)rc;
}

static void modem_audio_log(void* user, const char* line) {
  (void)user;
  ESP_LOGW(TAG, "%s", line);
}

static const h2_zero_esp_modem_audio_profile_t s_modem_audio_profile = {
    .mode = 2,
    .sidetone = 0,
    .mic_txgain = -1,
    .mic_txdgain = -1,
    .eec = NULL,
    .eec_count = 0,
};

EventBits_t __real_xEventGroupWaitBits(EventGroupHandle_t group,
                                       EventBits_t bits, BaseType_t clear,
                                       BaseType_t all, TickType_t ticks);
EventBits_t __wrap_xEventGroupWaitBits(EventGroupHandle_t group,
                                       EventBits_t bits, BaseType_t clear,
                                       BaseType_t all, TickType_t ticks);
EventBits_t __wrap_xEventGroupWaitBits(EventGroupHandle_t group,
                                       EventBits_t bits, BaseType_t clear,
                                       BaseType_t all, TickType_t ticks) {
  if (h2_atomic_ptr_load(&s_sdk_wait_task, H2_ATOMIC_SEQ_CST) !=
      xTaskGetCurrentTaskHandle())
    return __real_xEventGroupWaitBits(group, bits, clear, all, ticks);
  const TickType_t start = xTaskGetTickCount();
  if (ticks > pdMS_TO_TICKS(15000u)) ticks = pdMS_TO_TICKS(15000u);
  for (;;) {
    if (!h2_atomic_load(&s_power_requested)) return 0;
    TickType_t remaining = xTaskGetTickCount() - start;
    remaining = remaining >= ticks ? 0 : ticks - remaining;
    const TickType_t slice =
        remaining > pdMS_TO_TICKS(20u) ? pdMS_TO_TICKS(20u) : remaining;
    const EventBits_t result =
        __real_xEventGroupWaitBits(group, bits, clear, all, slice);
    if ((all ? (result & bits) == bits : (result & bits) != 0u) ||
        xTaskGetTickCount() - start >= ticks)
      return result;
  }
}

static esp_err_t modem_set_mode(esp_modem_dce_t* dce,
                                esp_modem_dce_mode_t mode) {
  if (!h2_atomic_load(&s_power_requested)) return ESP_ERR_INVALID_STATE;
  const h2_pal_result_t wake_rc = modem_at_prepare();
  if (wake_rc != H2_PAL_OK) return modem_at_gate_error(wake_rc);
  h2_atomic_ptr_store(&s_sdk_wait_task, xTaskGetCurrentTaskHandle(),
                      H2_ATOMIC_SEQ_CST);
  const esp_err_t rc = esp_modem_set_mode(dce, mode);
  h2_atomic_ptr_store(&s_sdk_wait_task, NULL, H2_ATOMIC_SEQ_CST);
  return h2_atomic_load(&s_power_requested) ? rc : ESP_ERR_INVALID_STATE;
}

/* UART writes have no SDK deadline. Use a bounded FIFO writer and retain
 * frame serialization across PPP and command producers. */
H2_ATOMIC_DEFINE_STATIC(flag, s_uart_tx_busy, false);
int __real_uart_write_bytes(uart_port_t port, const void* data, size_t size);
int __wrap_uart_write_bytes(uart_port_t port, const void* data, size_t size);
int __wrap_uart_write_bytes(uart_port_t port, const void* data, size_t size) {
  if (port != H2_ZERO_ESP_MODEM_UART)
    return __real_uart_write_bytes(port, data, size);
  const TickType_t start = xTaskGetTickCount();
  while (h2_atomic_flag_test_and_set(&s_uart_tx_busy, H2_ATOMIC_SEQ_CST)) {
    if (!h2_atomic_load(&s_power_requested) ||
        xTaskGetTickCount() - start >= pdMS_TO_TICKS(1000u))
      return -1;
    vTaskDelay(1);
  }
  size_t sent = 0u;
  while (sent < size && h2_atomic_load(&s_power_requested) &&
         xTaskGetTickCount() - start < pdMS_TO_TICKS(1000u)) {
    const size_t remaining = size - sent;
    const int n = uart_tx_chars(port, (const char*)data + sent,
                                remaining > 128u ? 128u : (uint32_t)remaining);
    if (n < 0) break;
    sent += (size_t)n;
    if (n == 0) vTaskDelay(1);
  }
  h2_atomic_flag_clear(&s_uart_tx_busy, H2_ATOMIC_SEQ_CST);
  return sent == size ? (int)sent : -1;
}

int __real_uart_read_bytes(uart_port_t uart_num, void* buf, uint32_t length,
                           uint32_t ticks_to_wait);
int __wrap_uart_read_bytes(uart_port_t uart_num, void* buf, uint32_t length,
                           uint32_t ticks_to_wait);

int __wrap_uart_read_bytes(uart_port_t uart_num, void* buf, uint32_t length,
                           uint32_t ticks_to_wait) {
  /* Baud changes/flush may consume the buffered-length snapshot taken by
   * esp_modem while its callback line_lock is held. Bound the stale read. */
  if (uart_num == H2_ZERO_ESP_MODEM_UART && ticks_to_wait > pdMS_TO_TICKS(20u))
    ticks_to_wait = pdMS_TO_TICKS(20u);
  const int read = __real_uart_read_bytes(uart_num, buf, length, ticks_to_wait);
  if (uart_num == H2_ZERO_ESP_MODEM_UART && read > 0) {
    app_rdy_scan((const uint8_t*)buf, (size_t)read);
    zero_esp_modem_rx_feed(ZERO_ESP_MODEM_RX_UART, (const uint8_t*)buf,
                           (size_t)read);
  }
  return read;
}

#ifdef CONFIG_ESP_MODEM_URC_HANDLER
static esp_err_t zero_esp_modem_urc_line(uint8_t* data, size_t len) {
  zero_esp_modem_rx_feed(ZERO_ESP_MODEM_RX_CMUX, data, len);
  return ESP_OK;
}
#endif

static h2_pal_result_t map_esp_error(esp_err_t err) {
  if (err >= MODEM_AT_GATE_ERROR_BASE) {
    return (h2_pal_result_t)(MODEM_AT_GATE_ERROR_BASE - err);
  }
  switch (err) {
    case ESP_OK:
      return H2_PAL_OK;
    case ESP_ERR_INVALID_STATE:
      return h2_atomic_load(&s_power_requested) ? H2_PAL_ERR_INVALID_STATE
                                                : H2_PAL_ERR_CLOSED;
    case ESP_ERR_INVALID_ARG:
      return H2_PAL_ERR_INVALID_ARG;
    case ESP_ERR_NO_MEM:
      return H2_PAL_ERR_NO_MEMORY;
    case ESP_ERR_TIMEOUT:
      return H2_PAL_ERR_TIMEOUT;
    case ESP_ERR_NOT_SUPPORTED:
      return H2_PAL_ERR_UNSUPPORTED;
    default:
      return H2_PAL_ERR_IO;
  }
}

static uint32_t elapsed_ms_since(TickType_t start_ticks) {
  TickType_t elapsed_ticks = xTaskGetTickCount() - start_ticks;
  uint64_t elapsed_ms = pdTICKS_TO_MS(elapsed_ticks);
  return elapsed_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_ms;
}

static uint32_t remaining_timeout_ms(uint32_t timeout_ms,
                                     TickType_t start_ticks) {
  const uint32_t effective_timeout_ms = timeout_ms == 0u ? 45000u : timeout_ms;
  uint32_t elapsed_ms = elapsed_ms_since(start_ticks);
  return elapsed_ms >= effective_timeout_ms ? 0u
                                            : effective_timeout_ms - elapsed_ms;
}

static void zero_esp_modem_report_netif_change(void) {
  (void)h2_esp_platform_netif_reconcile_default();
}

static bool modem_data_is_open(void) {
  h2_pal_modem_data_status_t status = {0};
  return h2_pal_modem_get_data_status(&s_modem.platform, &status) ==
             H2_PAL_OK &&
         status.state == H2_PAL_MODEM_DATA_OPEN && status.ip4_valid != 0u;
}

static h2_pal_result_t modem_data_state_set(h2_pal_modem_data_state_t state,
                                            h2_pal_result_t error) {
  const h2_pal_result_t rc =
      h2_pal_mutex_lock(s_modem.config.sync_api, s_modem.lock);
  if (rc != H2_PAL_OK) return rc;
  if (state == H2_PAL_MODEM_DATA_OPENING)
    memset(&s_modem.data_status, 0, sizeof(s_modem.data_status));
  s_modem.data_status.state = state;
  s_modem.data_status.last_error = error;
  if (state == H2_PAL_MODEM_DATA_CLOSED) s_modem.data_status.ip4_valid = 0u;
  return h2_pal_mutex_unlock(s_modem.config.sync_api, s_modem.lock);
}

static void zero_esp_modem_mark_data_closed(h2_pal_result_t result,
                                            int post_event) {
  if (h2_pal_mutex_lock(s_modem.config.sync_api, s_modem.lock) != H2_PAL_OK)
    return;
  s_modem.data_status.state = H2_PAL_MODEM_DATA_CLOSED;
  s_modem.data_status.ip4_valid = 0u;
  s_modem.data_status.last_error = result;
  if (post_event) {
    zero_esp_modem_post_event(H2_PAL_SYSTEM_EVENT_TYPE_MODEM_DATA_CLOSED,
                              &s_modem.data_status,
                              sizeof(s_modem.data_status));
  }
  (void)h2_pal_mutex_unlock(s_modem.config.sync_api, s_modem.lock);
}

static int modem_data_swap(void) {
  return esp_modem_set_mode(s_dce, ESP_MODEM_MODE_CMUX_MANUAL_SWAP);
}

static int modem_data_enter(void) {
  return esp_modem_set_mode(s_dce, ESP_MODEM_MODE_CMUX_MANUAL_DATA);
}

/* SDK DATA dials on primary and then swaps terminals. Select DLCI 2 first
 * so AT always returns to DLCI 1 after each PPP dial attempt. */
static esp_err_t modem_enter_data_mode(void) {
  if (!h2_atomic_load(&s_power_requested)) return ESP_ERR_INVALID_STATE;
  const h2_pal_result_t wake_rc = modem_at_prepare();
  if (wake_rc != H2_PAL_OK) return modem_at_gate_error(wake_rc);
  const h2_zero_esp_modem_data_ops_t ops = {modem_data_swap, modem_data_enter};
  h2_atomic_ptr_store(&s_sdk_wait_task, xTaskGetCurrentTaskHandle(),
                      H2_ATOMIC_SEQ_CST);
  const esp_err_t err = h2_zero_esp_modem_data_start(&ops);
  h2_atomic_ptr_store(&s_sdk_wait_task, NULL, H2_ATOMIC_SEQ_CST);
  if (err == ESP_OK) s_ppp_data_mode = true;
  return h2_atomic_load(&s_power_requested) ? err : ESP_ERR_INVALID_STATE;
}

static esp_err_t modem_leave_data_mode(void) {
  if (!s_ppp_data_mode) return ESP_OK;
  const esp_err_t err =
      modem_set_mode(s_dce, ESP_MODEM_MODE_CMUX_MANUAL_COMMAND);
  if (err == ESP_OK) s_ppp_data_mode = false;
  return err;
}

static void zero_esp_modem_cleanup_ppp_session(h2_pal_result_t result) {
  if (s_dce != NULL && s_cmux_enabled) {
    (void)modem_leave_data_mode();
    zero_esp_modem_rx_restart();
  }
  if (s_ppp_events != NULL) {
    xEventGroupClearBits(s_ppp_events, H2_ZERO_ESP_MODEM_PPP_GOT_IP_BIT);
  }
  zero_esp_modem_report_netif_change();
  if (s_dce != NULL) {
    (void)h2_quectel_modem_drop_ppp(&s_modem);
  }
  zero_esp_modem_mark_data_closed(result, 1);
}

bool h2_esp_board_modem_is_powered(void) {
  portENTER_CRITICAL(&s_power_gpio_lock);
  bool powered = h2_atomic_load(&s_modem_powered);
  if (!h2_atomic_load(&s_modem_accessed)) {
    const esp_reset_reason_t reset = esp_reset_reason();
    if (!h2_zero_esp_modem_power_restore(
            s_modem_power_marker,
            reset == ESP_RST_DEEPSLEEP || reset == ESP_RST_SW, &powered))
      powered = rtc_gpio_get_level(H2_ZERO_ESP_MODEM_ENABLE_GPIO) != 0u;
  }
  portEXIT_CRITICAL(&s_power_gpio_lock);
  return powered;
}

void h2_esp_board_modem_record_power(bool powered) {
  portENTER_CRITICAL(&s_power_gpio_lock);
  h2_atomic_store(&s_modem_powered, powered);
  h2_zero_esp_modem_power_record(&s_modem_power_marker, powered);
  portEXIT_CRITICAL(&s_power_gpio_lock);
}

bool h2_esp_board_modem_ri_pending(void) {
  const bool allowed =
      h2_esp_board_modem_is_powered() &&
      h2_atomic_load(&s_sleep_state) == H2_ZERO_ESP_MODEM_SLEEP_PENDING;
  if (allowed && !s_standby_ri_input_ready) {
    const esp_err_t rc = h2_board_modem_ri_restore_input(
        h2_atomic_load(&s_ri_handler_installed));
    if (rc != ESP_OK) {
      if (!s_standby_ri_restore_error_logged)
        ESP_LOGE(TAG, "H2_MODEM_RI stage=standby_input rc=%d", (int)rc);
      s_standby_ri_restore_error_logged = true;
      return false;
    }
    s_standby_ri_input_ready = true;
  }
  const bool pending = modem_ri_take();
  if (!allowed) {
    s_standby_ri_was_low = false;
    return false;
  }
  return h2_board_modem_ri_level_take(pending, gpio_get_level(GPIO_NUM_0) == 0,
                                      &s_standby_ri_was_low);
}

static h2_pal_result_t modem_enable_set_level_gpio(int level) {
  if (!h2_zero_esp_modem_sleep_admit(&s_sleep_state)) return H2_PAL_ERR_CLOSED;
  if (!level) h2_atomic_store(&s_open_ready, false);
  if (level && !h2_atomic_load(&s_power_requested)) return H2_PAL_ERR_CLOSED;
  esp_err_t err =
      h2_zero_esp_modem_output_config(H2_ZERO_ESP_MODEM_ENABLE_GPIO, false);
  if (err == ESP_OK && !level && s_dce == NULL)
    err = h2_zero_esp_modem_tx_claim_gpio(H2_ZERO_ESP_MODEM_TX_GPIO, false);
  if (err == ESP_OK) err = gpio_set_level(H2_ZERO_ESP_MODEM_ENABLE_GPIO, level);
  /* Release RTC retention only after the digital output is ready. */
  if (err == ESP_OK) err = rtc_gpio_hold_dis(H2_ZERO_ESP_MODEM_ENABLE_GPIO);
  if (err == ESP_OK) {
    h2_esp_board_modem_record_power(level != 0);
    if (level != 0) {
      h2_atomic_store(&s_app_rdy_seen, false);
      s_app_rdy_match = 0;
      h2_atomic_store(&s_rail_powered_this_open,
                      s_modem_baud != H2_ZERO_ESP_MODEM_CMUX_BAUD);
    }
    /* Removing module power restores the boot rate and ends CMUX. */
    if (!level) {
      s_modem_baud = H2_ZERO_ESP_MODEM_BAUD;
      s_cmux_enabled = false;
    }
  }
  return map_esp_error(err);
}

static h2_pal_result_t modem_enable_set_level(int level) {
  portENTER_CRITICAL(&s_power_gpio_lock);
  const h2_pal_result_t rc = modem_enable_set_level_gpio(level);
  portEXIT_CRITICAL(&s_power_gpio_lock);
  ESP_LOGW(TAG, "H2_MODEM_POWER level=%d rc=%d", level, rc);
  ESP_LOGW(TAG, "H2_H106_CELLULAR power_result on=%d stage=power rc=%d",
           level != 0, rc);
  return rc;
}

static void zero_esp_modem_post_event(h2_pal_system_event_type_t type,
                                      const void* payload,
                                      size_t payload_size) {
  h2_pal_system_event_t event;
  memset(&event, 0, sizeof(event));
  event.type = type;
  event.payload = payload;
  event.payload_size = payload_size;
  (void)h2_pal_system_event_post(h2_esp_board_system_event_api(), &event, 0u);
}

/* Provider calls this under its state lock, never issue AT here. SDK stop
 * clears the PPP netif; reject delayed GOT_IP until a new data open starts. */
H2_ATOMIC_DEFINE_STATIC(bool, s_sim_data_invalid, false);
/* Retain the board preparation entry for callers. File-static atomic
 * backing is ready before any modem callback and needs no runtime init. */
int h2_esp_zero_esp_board_modem_atomics_prepare(void) { return H2_PAL_OK; }

static void modem_sim_invalidate_data(void* user) {
  (void)user;
  h2_atomic_store(&s_sim_data_invalid, true);
  if (s_ppp_events != NULL) {
    xEventGroupClearBits(s_ppp_events, H2_ZERO_ESP_MODEM_PPP_GOT_IP_BIT);
    xEventGroupSetBits(s_ppp_events, H2_ZERO_ESP_MODEM_PPP_ERROR_BIT);
  }
  if (s_ppp_netif != NULL) esp_netif_action_stop(s_ppp_netif, NULL, 0, NULL);
}

static void zero_esp_modem_ppp_ip_handler_locked(void* arg,
                                                 esp_event_base_t base,
                                                 int32_t event_id,
                                                 void* event_data) {
  (void)arg;
  (void)base;
  if (event_id == IP_EVENT_PPP_GOT_IP) {
    if (h2_atomic_load(&s_sim_data_invalid)) return;
    esp_netif_action_connected(s_ppp_netif, base, event_id, event_data);
    const ip_event_got_ip_t* got_ip = (const ip_event_got_ip_t*)event_data;
    s_modem.data_status.state = H2_PAL_MODEM_DATA_OPEN;
    s_modem.data_status.ip4 = got_ip != NULL ? got_ip->ip_info.ip.addr : 0u;
    s_modem.data_status.ip4_valid =
        got_ip != NULL && got_ip->ip_info.ip.addr != 0u ? 1u : 0u;
    s_modem.data_status.last_error = H2_PAL_OK;

    esp_netif_dns_info_t dns;
    if (s_ppp_netif != NULL &&
        esp_netif_get_dns_info(s_ppp_netif, ESP_NETIF_DNS_MAIN, &dns) ==
            ESP_OK) {
      s_modem.data_status.dns1_ip4 = dns.ip.u_addr.ip4.addr;
    }
    if (s_ppp_netif != NULL &&
        esp_netif_get_dns_info(s_ppp_netif, ESP_NETIF_DNS_BACKUP, &dns) ==
            ESP_OK) {
      s_modem.data_status.dns2_ip4 = dns.ip.u_addr.ip4.addr;
    }
    if (s_ppp_events != NULL) {
      xEventGroupSetBits(s_ppp_events, H2_ZERO_ESP_MODEM_PPP_GOT_IP_BIT);
    }
    zero_esp_modem_report_netif_change();
    zero_esp_modem_post_event(H2_PAL_SYSTEM_EVENT_TYPE_MODEM_DATA_OPENED,
                              &s_modem.data_status,
                              sizeof(s_modem.data_status));
  } else if (event_id == IP_EVENT_PPP_LOST_IP) {
    if (s_ppp_events != NULL) {
      xEventGroupClearBits(s_ppp_events, H2_ZERO_ESP_MODEM_PPP_GOT_IP_BIT);
      xEventGroupSetBits(s_ppp_events, H2_ZERO_ESP_MODEM_PPP_STOPPED_BIT);
    }
    zero_esp_modem_report_netif_change();
    zero_esp_modem_mark_data_closed(
        h2_atomic_load(&s_ppp_close_requested) ? H2_PAL_OK : H2_PAL_ERR_IO, 1);
  }
}

static void zero_esp_modem_ppp_ip_handler(void* arg, esp_event_base_t base,
                                          int32_t event_id, void* event_data) {
  if (h2_pal_mutex_lock(s_modem.config.sync_api, s_modem.lock) != H2_PAL_OK)
    return;
  zero_esp_modem_ppp_ip_handler_locked(arg, base, event_id, event_data);
  (void)h2_pal_mutex_unlock(s_modem.config.sync_api, s_modem.lock);
}

static void zero_esp_modem_ppp_status_handler_locked(void* arg,
                                                     esp_event_base_t base,
                                                     int32_t event_id,
                                                     void* event_data) {
  (void)arg;
  (void)base;
  (void)event_data;
  ESP_LOGI(TAG, "PPP status event id=%" PRId32, event_id);
  if (h2_atomic_load(&s_ppp_close_requested) &&
      (event_id == NETIF_PPP_ERRORUSER ||
       event_id == NETIF_PPP_PHASE_DISCONNECT ||
       event_id == NETIF_PPP_PHASE_DEAD)) {
    if (s_ppp_events != NULL) {
      xEventGroupClearBits(s_ppp_events, H2_ZERO_ESP_MODEM_PPP_GOT_IP_BIT |
                                             H2_ZERO_ESP_MODEM_PPP_ERROR_BIT);
      xEventGroupSetBits(s_ppp_events, H2_ZERO_ESP_MODEM_PPP_STOPPED_BIT);
    }
    zero_esp_modem_report_netif_change();
    return;
  }
  if (event_id == NETIF_PPP_ERRORCONNECT ||
      event_id == NETIF_PPP_ERRORAUTHFAIL ||
      event_id == NETIF_PPP_ERRORPEERDEAD ||
      event_id == NETIF_PPP_CONNECT_FAILED ||
      event_id == NETIF_PPP_PHASE_DEAD ||
      event_id == NETIF_PPP_PHASE_DISCONNECT) {
    if (s_ppp_events != NULL) {
      xEventGroupClearBits(s_ppp_events, H2_ZERO_ESP_MODEM_PPP_GOT_IP_BIT);
      xEventGroupSetBits(s_ppp_events, H2_ZERO_ESP_MODEM_PPP_STOPPED_BIT |
                                           H2_ZERO_ESP_MODEM_PPP_ERROR_BIT);
    }
    zero_esp_modem_report_netif_change();
    zero_esp_modem_mark_data_closed(H2_PAL_ERR_IO, 1);
  }
}

static void zero_esp_modem_ppp_status_handler(void* arg, esp_event_base_t base,
                                              int32_t event_id,
                                              void* event_data) {
  if (h2_pal_mutex_lock(s_modem.config.sync_api, s_modem.lock) != H2_PAL_OK)
    return;
  zero_esp_modem_ppp_status_handler_locked(arg, base, event_id, event_data);
  (void)h2_pal_mutex_unlock(s_modem.config.sync_api, s_modem.lock);
}

static int modem_dtr_drive_gpio(int level) {
  if (!h2_zero_esp_modem_sleep_admit(&s_sleep_state)) return H2_PAL_ERR_CLOSED;
  /* GPIO6 is both the peripheral-power enable and the inverted DTR, and deep
   * sleep latches it low with an RTC hold plus the digital deep-sleep hold.
   * Waking therefore goes through the board's peripheral-power entry, which
   * releases both holds before driving the pad; a bare gpio_set_level would
   * return OK while the pad stays latched and the module stays asleep. */
  const h2_pal_result_t rc =
      level ? (h2_pal_result_t)h2_esp_zero_esp_board_enable_peripheral_power()
            : map_esp_error(
                  gpio_set_level(H2_ZERO_ESP_MODEM_DTR_INVERTED_GPIO, 0));
  return rc;
}

static int modem_dtr_drive(int level) {
  portENTER_CRITICAL(&s_power_gpio_lock);
  const int rc = modem_dtr_drive_gpio(level);
  portEXIT_CRITICAL(&s_power_gpio_lock);
  return rc;
}

static int modem_dtr_drain(void) {
  return map_esp_error(
      uart_wait_tx_done(H2_ZERO_ESP_MODEM_UART, pdMS_TO_TICKS(1000u)));
}

static int modem_dtr_ready(void) {
  if (s_dce == NULL) return H2_PAL_OK; /* Init verifies readiness next. */
  char response[H2_ZERO_ESP_MODEM_AT_RESPONSE_SIZE] = {0};
  return map_esp_error(zero_esp_modem_at_raw("AT", response, 1000u));
}

static void modem_dtr_wait(unsigned ms) {
  while (ms != 0u && h2_atomic_load(&s_power_requested)) {
    if (h2_atomic_ptr_load(&s_open_task, H2_ATOMIC_SEQ_CST) ==
            xTaskGetCurrentTaskHandle() &&
        (uint32_t)((xTaskGetTickCount() - s_open_started) *
                   portTICK_PERIOD_MS) >= s_open_timeout_ms)
      return;
    const unsigned slice = ms > 20u ? 20u : ms;
    vTaskDelay(pdMS_TO_TICKS(slice));
    ms -= slice;
  }
}

static h2_pal_result_t zero_esp_modem_sleep_gate(void* user, int allow_sleep) {
  (void)user;
  if (allow_sleep) return H2_PAL_OK;
  const h2_zero_esp_modem_dtr_ops_t ops = {
      modem_dtr_drive,
      modem_dtr_drain,
      modem_dtr_ready,
      modem_dtr_wait,
  };
  const h2_pal_result_t rc = h2_zero_esp_modem_dtr_gate(&ops, allow_sleep);
  return rc;
}

static h2_pal_result_t modem_create_transport(void) {
  esp_modem_dte_config_t dte_config = {
      .dte_buffer_size = 2u * 1024u,
      .task_stack_size = 6u * 1024u,
      .task_priority = 21u,
      .uart_config =
          {
              .port_num = H2_ZERO_ESP_MODEM_UART,
              .data_bits = UART_DATA_8_BITS,
              .stop_bits = UART_STOP_BITS_1,
              .parity = UART_PARITY_DISABLE,
              .flow_control = ESP_MODEM_FLOW_CONTROL_NONE,
              .source_clk = ESP_MODEM_DEFAULT_UART_CLK,
              .baud_rate = s_modem_baud,
              .tx_io_num = H2_ZERO_ESP_MODEM_TX_GPIO,
              .rx_io_num = H2_ZERO_ESP_MODEM_RX_GPIO,
              .rts_io_num = -1,
              .cts_io_num = -1,
              .rx_buffer_size = 8u * 1024u,
              .tx_buffer_size = 0u,
              .event_queue_size = 30,
          },
  };
  esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG("internet");
  s_dce = esp_modem_new(&dte_config, &dce_config, s_ppp_netif);
  if (s_dce == NULL) {
    ESP_LOGE(TAG, "esp_modem_new failed");
    return H2_PAL_ERR_NO_MEMORY;
  }
  /* The board validates SIM generation before publishing a link-up. The
   * SDK's unconditional second handler would resurrect a stale PPP address. */
  esp_err_t err = esp_event_handler_unregister(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                                               esp_netif_action_connected);
  if (err != ESP_OK) {
    return map_esp_error(err);
  }
  s_cmux_enabled = false;
  zero_esp_modem_rx_set_source(ZERO_ESP_MODEM_RX_UART);

#ifdef CONFIG_ESP_MODEM_URC_HANDLER
  err = esp_modem_set_urc(s_dce, zero_esp_modem_urc_line);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "set URC handler failed: %s", esp_err_to_name(err));
  }
#endif
  return H2_PAL_OK;
}

static h2_pal_result_t zero_esp_modem_init(void* user) {
  (void)user;
  if (!h2_atomic_load(&s_power_requested)) return H2_PAL_ERR_CLOSED;
  if (s_dce != NULL) {
    return H2_PAL_OK;
  }

  if (h2_atomic_ptr_load(&s_open_task, H2_ATOMIC_SEQ_CST) !=
      xTaskGetCurrentTaskHandle())
    return H2_PAL_ERR_CLOSED;
  ESP_LOGW(TAG, "H2_H106_CELLULAR stage=transport_init task=%s",
           pcTaskGetName(NULL));
  h2_pal_result_t rc;

  rc = zero_esp_modem_sleep_gate(NULL, 0);
  if (rc != H2_PAL_OK) return rc;
  rc = modem_enable_set_level(1);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  rc = map_esp_error(
      h2_zero_esp_modem_tx_claim_gpio(H2_ZERO_ESP_MODEM_TX_GPIO, true));
  if (rc != H2_PAL_OK) return rc;

  rc = h2_esp_platform_netif_init_once();
  if (rc != H2_PAL_OK) return rc;

  s_ppp_events = xEventGroupCreate();
  if (s_ppp_events == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }

  esp_err_t err;
  esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_PPP();
  s_ppp_netif = esp_netif_new(&netif_config);
  if (s_ppp_netif == NULL) {
    vEventGroupDelete(s_ppp_events);
    s_ppp_events = NULL;
    return H2_PAL_ERR_NO_MEMORY;
  }
  h2_esp_platform_netif_register(s_ppp_netif, H2_PAL_NETIF_KIND_MODEM_DATA);

  esp_netif_ppp_config_t ppp_config = {
      .ppp_phase_event_enabled = true,
      .ppp_error_event_enabled = true,
  };
  err = esp_netif_ppp_set_params(s_ppp_netif, &ppp_config);
  if (err == ESP_OK) {
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                                              zero_esp_modem_ppp_ip_handler,
                                              NULL, &s_ip_got_instance);
  }
  if (err == ESP_OK) {
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_PPP_LOST_IP,
                                              zero_esp_modem_ppp_ip_handler,
                                              NULL, &s_ip_lost_instance);
  }
  if (err == ESP_OK) {
    err = esp_event_handler_instance_register(
        NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, zero_esp_modem_ppp_status_handler,
        NULL, &s_ppp_status_instance);
  }
  if (err != ESP_OK) {
    zero_esp_modem_deinit(NULL);
    return map_esp_error(err);
  }

  rc = modem_create_transport();
  if (rc != H2_PAL_OK) {
    zero_esp_modem_deinit(NULL);
    return rc;
  }
  err = modem_ri_start();
  if (err != ESP_OK) {
    zero_esp_modem_deinit(NULL);
    return map_esp_error(err);
  }

  ESP_LOGI(TAG, "esp_modem ready tx=%d rx=%d baud=%d",
           H2_ZERO_ESP_MODEM_TX_GPIO, H2_ZERO_ESP_MODEM_RX_GPIO, s_modem_baud);
  return H2_PAL_OK;
}

static h2_pal_result_t zero_esp_modem_deinit(void* user) {
  (void)user;
  return modem_destroy_transport(15000u);
}

static h2_pal_result_t modem_destroy_transport(uint32_t timeout_ms) {
  /* Drain provider callbacks under its state lock before SDK joins. Never
   * hold that lock while unregistering handlers or destroying the UART. */
  const h2_pal_result_t admission =
      h2_quectel_modem_transport_closing(&s_modem);
  if (admission != H2_PAL_OK) return admission;
  h2_atomic_store(&s_cleanup_required, true);
  const h2_pal_result_t power_rc = modem_enable_set_level(0);
  if (power_rc != H2_PAL_OK) return power_rc;
  if (s_ppp_netif != NULL) {
    /* ESP-IDF creates the native PPP PCB inside esp_netif_new(PPP), with
     * phase DEAD, before event registration or esp_modem_new attaches IO.
     * Keep this gate even when later init failed and s_dce is still NULL. */
    /* Modem SDK flags and DISCONNECT events do not prove the PPP PCB is
     * dead. Keep DCE, netif and handlers alive until lwIP confirms it. */
    const h2_pal_result_t rc =
        h2_esp_platform_ppp_quiesce(s_ppp_netif, timeout_ms);
    if (rc != H2_PAL_OK) {
      ESP_LOGW(TAG, "H2_MODEM_PPP quiesce rc=%d resources_retained=1", rc);
      return rc;
    }
  }
  ESP_LOGI(TAG, "H2_MODEM_PPP quiesced before transport destroy");
  h2_atomic_store(&s_ri_paused, true);
  if (s_ri_installed) {
    const esp_err_t err = gpio_isr_handler_remove(GPIO_NUM_0);
    if (err != ESP_OK) return map_esp_error(err);
    s_ri_installed = false;
    h2_atomic_store(&s_ri_handler_installed, false);
  }
#ifdef CONFIG_ESP_MODEM_URC_HANDLER
  if (s_dce != NULL) {
    const esp_err_t err = esp_modem_set_urc(s_dce, NULL);
    if (err != ESP_OK) return map_esp_error(err);
  }
#endif
  if (s_ppp_status_instance != NULL) {
    const esp_err_t err = esp_event_handler_instance_unregister(
        NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, s_ppp_status_instance);
    if (err != ESP_OK) return map_esp_error(err);
    s_ppp_status_instance = NULL;
  }
  if (s_ip_lost_instance != NULL) {
    const esp_err_t err = esp_event_handler_instance_unregister(
        IP_EVENT, IP_EVENT_PPP_LOST_IP, s_ip_lost_instance);
    if (err != ESP_OK) return map_esp_error(err);
    s_ip_lost_instance = NULL;
  }
  if (s_ip_got_instance != NULL) {
    const esp_err_t err = esp_event_handler_instance_unregister(
        IP_EVENT, IP_EVENT_PPP_GOT_IP, s_ip_got_instance);
    if (err != ESP_OK) return map_esp_error(err);
    s_ip_got_instance = NULL;
  }
  /* Stop app-owned callbacks before esp_modem tears down PPP. Destroy can
   * post the final ERRORUSER/DISCONNECT events on the default event loop. */
  zero_esp_modem_rx_set_source(ZERO_ESP_MODEM_RX_OFF);
  if (s_dce != NULL) {
    esp_modem_destroy(s_dce);
    s_dce = NULL;
  }
  if (s_ppp_netif != NULL) {
    h2_esp_platform_netif_unregister(s_ppp_netif);
    esp_netif_destroy(s_ppp_netif);
    s_ppp_netif = NULL;
  }
  if (s_ppp_events != NULL) {
    vEventGroupDelete(s_ppp_events);
    s_ppp_events = NULL;
  }
  s_cmux_enabled = false;
  s_ppp_data_mode = false;
  h2_atomic_store(&s_ppp_close_requested, false);
  s_dce_apn_configured = false;
  taskENTER_CRITICAL(&s_call_monitor_lock);
  memset(&s_call_monitor_status, 0, sizeof(s_call_monitor_status));
  taskEXIT_CRITICAL(&s_call_monitor_lock);
  const h2_pal_result_t tx_rc = map_esp_error(
      h2_zero_esp_modem_tx_claim_gpio(H2_ZERO_ESP_MODEM_TX_GPIO, false));
  if (tx_rc != H2_PAL_OK) return tx_rc;
  h2_atomic_store(&s_cleanup_required, false);
  return H2_PAL_OK;
}

/* Cold power-on can take seconds. Retry only at the known module baud. */
static esp_err_t zero_esp_modem_wait_ready(char* response, size_t response_size,
                                           uint32_t timeout_ms) {
  if (response == NULL || response_size < H2_ZERO_ESP_MODEM_AT_RESPONSE_SIZE)
    return ESP_ERR_INVALID_ARG;
  if (h2_atomic_ptr_load(&s_open_task, H2_ATOMIC_SEQ_CST) ==
      xTaskGetCurrentTaskHandle()) {
    const uint32_t remaining =
        remaining_timeout_ms(s_open_timeout_ms, s_open_started);
    if (timeout_ms > remaining) timeout_ms = remaining;
  }
  const TickType_t start = xTaskGetTickCount();
  for (unsigned attempt = 0; attempt < 10u; ++attempt) {
    if (!h2_atomic_load(&s_power_requested)) return ESP_ERR_INVALID_STATE;
    uint32_t remaining = remaining_timeout_ms(timeout_ms, start);
    if (remaining == 0u) return ESP_ERR_TIMEOUT;
    response[0] = '\0';
    const esp_err_t err =
        zero_esp_modem_at("AT", response, remaining > 600u ? 600u : remaining);
    if (err != ESP_ERR_TIMEOUT) return err;
    remaining = remaining_timeout_ms(timeout_ms, start);
    if (remaining == 0u) return ESP_ERR_TIMEOUT;
    modem_dtr_wait(remaining > 500u ? 500u : remaining);
  }
  return ESP_ERR_TIMEOUT;
}

/* SIM configuration may restart only during the cold, plain-UART prepare. */
static h2_pal_result_t modem_restart_module(void* user) {
  (void)user;
  if (s_cmux_enabled) return H2_PAL_ERR_INVALID_STATE;
  if (!h2_atomic_load(&s_power_requested)) return H2_PAL_ERR_CLOSED;
  h2_pal_result_t rc = modem_enable_set_level(0);
  if (rc != H2_PAL_OK) return rc;
  s_modem.prepared = 0u;
  s_dce_apn_configured = false;
  modem_dtr_wait(1000u);
  rc = modem_enable_set_level(1);
  if (rc != H2_PAL_OK) return rc;
  modem_dtr_wait(15000u);
  char response[H2_ZERO_ESP_MODEM_AT_RESPONSE_SIZE] = {0};
  rc = map_esp_error(
      zero_esp_modem_wait_ready(response, sizeof(response), 5000u));
  ESP_LOGW(TAG, "H2_MODEM_SIM_CONFIG stage=restart rc=%d", rc);
  return rc;
}

static h2_pal_result_t zero_esp_modem_command_locked(void* user,
                                                     const char* cmd,
                                                     char* response,
                                                     size_t response_size,
                                                     uint32_t timeout_ms) {
  (void)user;
  if (cmd == NULL || response == NULL ||
      response_size < H2_ZERO_ESP_MODEM_AT_RESPONSE_SIZE) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!h2_atomic_load(&s_power_requested) || s_dce == NULL) {
    response[0] = '\0';
    return H2_PAL_ERR_CLOSED;
  }
  response[0] = '\0';
  esp_err_t err = zero_esp_modem_at(cmd, response, timeout_ms);
  const h2_pal_result_t result = map_esp_error(err);
  if (result == H2_PAL_OK && strcmp(cmd, "AT+CLIP=1") == 0) {
    /*
     * Quectel modules can emit URCs on a USB interface even when AT
     * commands use UART1.  This runs during the established modem prepare
     * sequence, after the module has accepted its first AT command.
     */
    char urc_port_response[H2_ZERO_ESP_MODEM_AT_RESPONSE_SIZE];
    esp_err_t urc_port_err = ESP_FAIL;
    for (uint32_t attempt = 0u; attempt < H2_ZERO_ESP_MODEM_URC_PORT_ATTEMPTS;
         ++attempt) {
      memset(urc_port_response, 0, sizeof(urc_port_response));
      urc_port_err = zero_esp_modem_at("AT+QURCCFG=\"urcport\",\"uart1\"",
                                       urc_port_response,
                                       H2_ZERO_ESP_MODEM_URC_PORT_TIMEOUT_MS);
      if (urc_port_err == ESP_OK) {
        break;
      }
      if (attempt + 1u < H2_ZERO_ESP_MODEM_URC_PORT_ATTEMPTS) {
        vTaskDelay(pdMS_TO_TICKS(H2_ZERO_ESP_MODEM_URC_PORT_RETRY_DELAY_MS));
      }
    }
    if (urc_port_err != ESP_OK) {
      ESP_LOGE(TAG, "URC port UART1 setup failed: %s response=%s",
               esp_err_to_name(urc_port_err), urc_port_response);
      return map_esp_error(urc_port_err);
    }
    char sms_response[H2_ZERO_ESP_MODEM_AT_RESPONSE_SIZE] = {0};
    esp_err_t sms_rc =
        zero_esp_modem_at("AT+CNMI=0,0,0,0,0", sms_response, 2000);
    if (sms_rc != ESP_OK) return map_esp_error(sms_rc);
    const h2_zero_esp_modem_audio_ops_t audio_ops = {modem_audio_at,
                                                     modem_audio_log, NULL};
    const unsigned audio_failures =
        h2_zero_esp_modem_audio_apply(&audio_ops, &s_modem_audio_profile);
    if (audio_failures != 0u)
      ESP_LOGW(TAG, "H2_MODEM_AUDIO failures=%u", audio_failures);
    return h2_esp_board_modem_prepare_ri_wake();
  }
  return result;
}

static h2_pal_result_t zero_esp_modem_command(void* user, const char* cmd,
                                              char* response,
                                              size_t response_size,
                                              uint32_t timeout_ms) {
  if (cmd == NULL || response == NULL ||
      response_size < H2_ZERO_ESP_MODEM_AT_RESPONSE_SIZE)
    return H2_PAL_ERR_INVALID_ARG;
  response[0] = '\0';
  const TickType_t start = xTaskGetTickCount();
  h2_pal_result_t rc = modem_operation_lock(timeout_ms);
  if (rc != H2_PAL_OK) return rc;
  const uint32_t remaining = remaining_timeout_ms(timeout_ms, start);
  rc = remaining == 0u ? H2_PAL_ERR_TIMEOUT
                       : zero_esp_modem_command_locked(
                             user, cmd, response, response_size, remaining);
  const h2_pal_result_t unlock_rc = modem_operation_unlock();
  return rc != H2_PAL_OK ? rc : unlock_rc;
}

h2_pal_result_t h2_esp_board_modem_prepare_ri_wake(void) {
  char response[H2_ZERO_ESP_MODEM_AT_RESPONSE_SIZE] = {0};
  const char* commands[] = {
      "AT+QCFG=\"urc/ri/ring\",\"pulse\",2000,1",
      "AT+QCFG=\"risignaltype\",\"physical\"",
      "AT+QCFG=\"urc/ri/other\",\"off\"",
  };
  for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
    const esp_err_t err = zero_esp_modem_at(commands[i], response, 1000u);
    if (err != ESP_OK) return map_esp_error(err);
  }
  return H2_PAL_OK;
}

void h2_esp_board_modem_poll(void) {
  if (modem_operation_allowed(NULL) != H2_PAL_OK ||
      h2_atomic_load(&s_ri_paused))
    return;
  if (modem_operation_lock(0u) != H2_PAL_OK) return;
  if (modem_operation_allowed(NULL) == H2_PAL_OK &&
      !h2_atomic_load(&s_ri_paused) && s_dce != NULL && s_modem.prepared &&
      modem_ri_take()) {
    ESP_LOGD(TAG, "H2_MODEM_RI edge gpio=0 level=%d baud=%d cmux=%d",
             gpio_get_level(GPIO_NUM_0), s_modem_baud, s_cmux_enabled);
    h2_pal_result_t rc = modem_dtr_drive(1);
    if (rc == H2_PAL_OK) {
      modem_dtr_wait(500u);
      rc = modem_dtr_ready();
    }
    ESP_LOGW(TAG, "H2_MODEM_RI wake_ready rc=%d dtr_gpio=%d", rc,
             gpio_get_level(H2_ZERO_ESP_MODEM_DTR_INVERTED_GPIO));
    /* URCs remain authoritative; do not self-requeue AT contention. */
  }
  (void)modem_operation_unlock();
}

static int sleep_close_ppp(void) {
  const TickType_t start = xTaskGetTickCount();
  /* Bound the provider AT and board event waits. SDK exit_data has its
   * own waits; enter it only once while PPP is active. */
  const uint32_t saved_timeout = s_modem.config.command_timeout_ms;
  s_modem.config.command_timeout_ms = 500u;
  const h2_pal_result_t rc = zero_esp_modem_data_close(&s_modem.platform, 500u);
  s_modem.config.command_timeout_ms = saved_timeout;
  ESP_LOGW(TAG, "H2_BOARD_DEEP_SLEEP stage=close_ppp rc=%d elapsed_ms=%lu", rc,
           (unsigned long)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS));
  return rc == H2_PAL_ERR_TIMEOUT ? modem_dtr_ready() : rc;
}

static int sleep_exit_cmux(void) {
  const TickType_t start = xTaskGetTickCount();
  ESP_LOGW(TAG,
           "H2_BOARD_DEEP_SLEEP stage=exit_cmux_begin elapsed_ms=0 cmux=%d",
           s_cmux_enabled);
  /* data_close already leaves the data DLCI in command mode. Repeating
   * MANUAL_COMMAND runs SDK exit_data again (30 s PPP + 2 s text wait). */
  esp_err_t err = s_cmux_enabled
                      ? modem_set_mode(s_dce, ESP_MODEM_MODE_CMUX_MANUAL_EXIT)
                      : ESP_OK;
  if (err == ESP_OK) {
    s_cmux_enabled = false;
    s_ppp_data_mode = false;
    zero_esp_modem_rx_set_source(ZERO_ESP_MODEM_RX_UART);
  }
  ESP_LOGW(TAG, "H2_BOARD_DEEP_SLEEP stage=exit_cmux rc=%d elapsed_ms=%lu",
           (int)err,
           (unsigned long)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS));
  return map_esp_error(err);
}

static int sleep_quiet_urcs(const char* command) {
  const TickType_t start = xTaskGetTickCount();
  char response[H2_ZERO_ESP_MODEM_AT_RESPONSE_SIZE] = {0};
  const esp_err_t err = zero_esp_modem_at(command, response, 500u);
  ESP_LOGW(
      TAG,
      "H2_BOARD_DEEP_SLEEP stage=quiet_urcs rc=%d elapsed_ms=%lu command=%s",
      (int)err,
      (unsigned long)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS),
      command);
  return map_esp_error(err);
}

static int sleep_ri_wake(void) {
  const TickType_t start = xTaskGetTickCount();
  char response[H2_ZERO_ESP_MODEM_AT_RESPONSE_SIZE] = {0};
  esp_err_t err =
      zero_esp_modem_at("AT+QURCCFG=\"urcport\",\"uart1\"", response, 500u);
  const esp_err_t ring_rc = zero_esp_modem_at(
      "AT+QCFG=\"urc/ri/ring\",\"pulse\",2000,1", response, 500u);
  if (err == ESP_OK) err = ring_rc;
  const esp_err_t physical_rc = zero_esp_modem_at(
      "AT+QCFG=\"risignaltype\",\"physical\"", response, 500u);
  if (err == ESP_OK) err = physical_rc;
  ESP_LOGW(TAG, "H2_BOARD_DEEP_SLEEP stage=ri rc=%d elapsed_ms=%lu", (int)err,
           (unsigned long)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS));
  return map_esp_error(err);
}

static int sleep_qsclk(const char* command) {
  const TickType_t start = xTaskGetTickCount();
  char response[H2_ZERO_ESP_MODEM_AT_RESPONSE_SIZE] = {0};
  const esp_err_t err = zero_esp_modem_at(command, response, 1000u);
  ESP_LOGW(TAG, "H2_BOARD_DEEP_SLEEP stage=qsclk rc=%d elapsed_ms=%lu",
           (int)err,
           (unsigned long)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS));
  if (err != ESP_OK)
    ESP_LOGW(
        TAG,
        "Modem sleep request failed; retention requires successful prepare");
  return map_esp_error(err);
}

#ifndef H2_BOARD_SLEEP_AT_PROBE
#define H2_BOARD_SLEEP_AT_PROBE 0
#endif

#if H2_BOARD_SLEEP_AT_PROBE
static void sleep_at_probe(void) {
  char response[H2_ZERO_ESP_MODEM_AT_RESPONSE_SIZE] = {0};
  esp_err_t err = zero_esp_modem_at("AT", response, 500u);
  ESP_LOGW(TAG, "H2_SLEEP_AT_PROBE step=baseline err=%d", (int)err);
  if (err != ESP_OK) return;
  const int drive_rc = modem_dtr_drive(0);
  ESP_LOGW(TAG, "H2_SLEEP_AT_PROBE step=dtr_high err=%d", drive_rc);
  if (drive_rc != H2_PAL_OK) return;
  const TickType_t start = xTaskGetTickCount();
  const unsigned offsets[] = {2000u, 5000u};
  for (unsigned i = 0; i < 2u; ++i) {
    const TickType_t elapsed = xTaskGetTickCount() - start;
    const TickType_t deadline = pdMS_TO_TICKS(offsets[i]);
    if (elapsed < deadline) vTaskDelay(deadline - elapsed);
    err = zero_esp_modem_at("AT", response, 500u);
    ESP_LOGW(
        TAG, "H2_SLEEP_AT_PROBE step=after%us err=%d elapsed_ms=%lu",
        offsets[i] / 1000u, (int)err,
        (unsigned long)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS));
  }
}
#endif

static int sleep_drain_tx(void) {
  const TickType_t start = xTaskGetTickCount();
  const int rc = map_esp_error(
      uart_wait_tx_done(H2_ZERO_ESP_MODEM_UART, pdMS_TO_TICKS(500u)));
  ESP_LOGW(TAG, "H2_BOARD_DEEP_SLEEP stage=drain rc=%d elapsed_ms=%lu", rc,
           (unsigned long)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS));
#if H2_BOARD_SLEEP_AT_PROBE
  if (rc == H2_PAL_OK) sleep_at_probe();
#endif
  return rc;
}

static h2_pal_result_t modem_prepare_sleep_locked(void) {
  const TickType_t start = xTaskGetTickCount();
  if (s_dce == NULL) {
    ESP_LOGW(TAG,
             "H2_BOARD_DEEP_SLEEP stage=modem_skip rc=%d elapsed_ms=0 "
             "transport=closed",
             H2_PAL_ERR_CLOSED);
    return H2_PAL_ERR_CLOSED;
  }
  h2_pal_result_t wake_rc = modem_dtr_drive(1);
  if (wake_rc == H2_PAL_OK) modem_dtr_wait(500u);
  ESP_LOGW(TAG, "H2_BOARD_DEEP_SLEEP stage=wake rc=%d elapsed_ms=%lu", wake_rc,
           (unsigned long)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS));
  if (wake_rc != H2_PAL_OK) return wake_rc;
  const h2_zero_esp_modem_sleep_ops_t ops = {
      .close_ppp = sleep_close_ppp,
      .exit_cmux = sleep_exit_cmux,
      .ri_wake = sleep_ri_wake,
      .quiet_urcs = sleep_quiet_urcs,
      .qsclk = sleep_qsclk,
      .drain_tx = sleep_drain_tx,
  };
  const h2_pal_result_t rc = h2_zero_esp_modem_sleep_prepare(&ops);
  ESP_LOGW(TAG,
           "H2_BOARD_DEEP_SLEEP stage=modem_done rc=%d elapsed_ms=%lu baud=%d "
           "cmux=%d",
           rc,
           (unsigned long)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS),
           s_modem_baud, s_cmux_enabled);
  return rc;
}

/* Terminal-only admission: no AT, worker lock, allocation or logging. */
bool h2_esp_board_modem_sleep_commit(void) {
  portENTER_CRITICAL(&s_power_gpio_lock);
  const bool ready = h2_zero_esp_modem_sleep_commit_session(
      &s_sleep_state, h2_atomic_load(&s_modem_accessed));
  const bool powered = h2_esp_board_modem_is_powered();
  h2_atomic_store(&s_power_requested, false);
  portEXIT_CRITICAL(&s_power_gpio_lock);
  return ready && powered;
}

h2_pal_result_t h2_esp_board_modem_prepare_sleep(bool supervised) {
  if (!h2_atomic_load(&s_modem_accessed)) return H2_PAL_OK;
  if (!h2_zero_esp_modem_sleep_admit(&s_sleep_state)) return H2_PAL_ERR_CLOSED;
  const TickType_t start = xTaskGetTickCount();
  h2_atomic_store(&s_ri_paused, true);
  ESP_LOGW(TAG,
           "H2_BOARD_DEEP_SLEEP stage=modem_begin rc=0 elapsed_ms=0 baud=%d",
           s_modem_baud);
  if (!s_modem_initialized) return H2_PAL_ERR_CLOSED;
  h2_pal_result_t rc = modem_operation_lock(15000u);
  ESP_LOGW(TAG, "H2_BOARD_DEEP_SLEEP stage=operation_lock rc=%d elapsed_ms=%lu",
           rc,
           (unsigned long)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS));
  if (rc != H2_PAL_OK) return rc;
  rc = modem_prepare_sleep_locked();
  /* Seal against GPIO/power admission before releasing transaction ownership.
   */
  if (rc == H2_PAL_OK) {
    portENTER_CRITICAL(&s_power_gpio_lock);
    if (!h2_zero_esp_modem_sleep_complete(&s_sleep_state, supervised))
      rc = H2_PAL_ERR_CLOSED;
    portEXIT_CRITICAL(&s_power_gpio_lock);
  }
  const h2_pal_result_t unlock_rc = modem_operation_unlock();
  if (unlock_rc != H2_PAL_OK)
    (void)h2_zero_esp_modem_sleep_invalidate(&s_sleep_state);
  return rc != H2_PAL_OK ? rc : unlock_rc;
}

static h2_pal_result_t zero_esp_modem_apply_apn(const char* apn) {
  if (apn == NULL || apn[0] == '\0') {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_modem_apn_config_t config;
  memset(&config, 0, sizeof(config));
  strncpy(config.apn, apn, sizeof(config.apn) - 1u);
  h2_pal_result_t rc = zero_esp_modem_set_apn(&s_modem.platform, &config);
  if (rc == H2_PAL_OK) {
    s_apn_auto_selected = true;
  }
  return rc;
}

static h2_pal_result_t zero_esp_modem_configure_apn_from_imsi(void) {
  if (s_modem.last_apn[0] != '\0' && !s_apn_auto_selected) {
    if (s_dce_apn_configured) {
      return H2_PAL_OK;
    }
    h2_pal_modem_apn_config_t config;
    memset(&config, 0, sizeof(config));
    strncpy(config.apn, s_modem.last_apn, sizeof(config.apn) - 1u);
    strncpy(config.username, s_modem.last_username,
            sizeof(config.username) - 1u);
    strncpy(config.password, s_modem.last_password,
            sizeof(config.password) - 1u);
    return zero_esp_modem_set_apn(&s_modem.platform, &config);
  }

  h2_pal_modem_identity_t identity;
  memset(&identity, 0, sizeof(identity));
  h2_pal_result_t rc = h2_pal_modem_get_identity(&s_modem.platform, &identity);
  const char* apn =
      rc == H2_PAL_OK ? h2_apn_from_imsi(identity.imsi) : "internet";
  if (s_modem.last_apn[0] != '\0' && strcmp(s_modem.last_apn, apn) == 0 &&
      s_dce_apn_configured) {
    s_apn_auto_selected = true;
    return H2_PAL_OK;
  }

  ESP_LOGI(TAG, "using modem APN %s", apn);
  return zero_esp_modem_apply_apn(apn);
}

static h2_pal_result_t zero_esp_modem_wait_registered(uint32_t timeout_ms) {
  const uint32_t interval_ms = 2000u;
  const TickType_t start_ticks = xTaskGetTickCount();

  while (true) {
    if (!h2_atomic_load(&s_power_requested)) return H2_PAL_ERR_CLOSED;
    uint32_t remaining_ms = remaining_timeout_ms(
        timeout_ms == 0u ? 30000u : timeout_ms, start_ticks);
    if (remaining_ms == 0u) {
      break;
    }
    uint32_t old_command_timeout_ms = s_modem.config.command_timeout_ms;
    uint32_t command_timeout_ms = remaining_ms / 4u;
    if (command_timeout_ms < 250u) {
      command_timeout_ms = remaining_ms < 250u ? remaining_ms : 250u;
    }
    if (command_timeout_ms > old_command_timeout_ms) {
      command_timeout_ms = old_command_timeout_ms;
    }
    s_modem.config.command_timeout_ms = command_timeout_ms;
    h2_pal_modem_status_t status;
    memset(&status, 0, sizeof(status));
    h2_pal_result_t rc = h2_pal_modem_get_status(&s_modem.platform, &status);
    s_modem.config.command_timeout_ms = old_command_timeout_ms;
    if (rc == H2_PAL_OK &&
        (status.registration == H2_PAL_MODEM_REGISTRATION_HOME ||
         status.registration == H2_PAL_MODEM_REGISTRATION_ROAMING)) {
      ESP_LOGI(TAG, "network registered packet=%d", status.packet);
      return H2_PAL_OK;
    }
    if (rc != H2_PAL_OK) {
      ESP_LOGW(TAG, "network registration poll failed rc=%d", rc);
    }
    remaining_ms = remaining_timeout_ms(timeout_ms == 0u ? 30000u : timeout_ms,
                                        start_ticks);
    if (remaining_ms == 0u) {
      break;
    }
    uint32_t delay_ms = remaining_ms > interval_ms ? interval_ms : remaining_ms;
    modem_dtr_wait(delay_ms);
  }
  ESP_LOGW(TAG, "network registration timeout");
  return H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t zero_esp_modem_set_apn(
    h2_pal_modem_t* platform, const h2_pal_modem_apn_config_t* config) {
  h2_pal_result_t rc = h2_quectel_modem_set_apn(platform, config);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if (s_dce != NULL) {
    esp_err_t err = esp_modem_set_apn(s_dce, config->apn);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "set esp_modem APN failed: %s", esp_err_to_name(err));
      s_dce_apn_configured = false;
      return map_esp_error(err);
    }
    esp_netif_auth_type_t auth_type = NETIF_PPP_AUTHTYPE_NONE;
    const char* username = "";
    const char* password = "";
    if (config->username[0] != '\0' || config->password[0] != '\0') {
      auth_type = (esp_netif_auth_type_t)(NETIF_PPP_AUTHTYPE_PAP |
                                          NETIF_PPP_AUTHTYPE_CHAP);
      username = config->username;
      password = config->password;
    }
    err = esp_netif_ppp_set_auth(s_ppp_netif, auth_type, username, password);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "set PPP auth failed: %s", esp_err_to_name(err));
      s_dce_apn_configured = false;
      return map_esp_error(err);
    }
    s_dce_apn_configured = true;
  } else {
    s_dce_apn_configured = false;
  }
  s_apn_auto_selected = false;
  return H2_PAL_OK;
}

typedef struct {
  char* response;
  uint32_t timeout_ms;
  TickType_t start;
  bool aborted;
} modem_handshake_context_t;

static int modem_handshake_power(void* user, int level) {
  modem_handshake_context_t* ctx = user;
  if (level && remaining_timeout_ms(ctx->timeout_ms, ctx->start) == 0u)
    return H2_PAL_ERR_TIMEOUT;
  return modem_enable_set_level(level);
}

static int modem_handshake_cold_ready(void* user) {
  modem_handshake_context_t* ctx = user;
  s_modem_baud = H2_ZERO_ESP_MODEM_BAUD;
  uart_flush_input(H2_ZERO_ESP_MODEM_UART);
  const uint32_t remaining = remaining_timeout_ms(ctx->timeout_ms, ctx->start);
  return remaining == 0u ? H2_PAL_ERR_TIMEOUT
                         : map_esp_error(zero_esp_modem_wait_ready(
                               ctx->response,
                               H2_ZERO_ESP_MODEM_AT_RESPONSE_SIZE, remaining));
}

static void modem_handshake_cycle_report(void* user, int rc) {
  (void)user;
  ESP_LOGW(TAG, "H2_H106_CELLULAR stage=handshake_power_cycle rc=%d baud=%d",
           rc, s_modem_baud);
}

static int modem_handshake_power_cycle(void* user) {
  const h2_modem_power_cycle_ops_t ops = {modem_handshake_power, modem_dtr_wait,
                                          modem_handshake_cold_ready,
                                          modem_handshake_cycle_report};
  return h2_modem_handshake_power_cycle(&ops, user);
}

static int modem_handshake_probe_high(void* user, unsigned attempts) {
  modem_handshake_context_t* ctx = user;
  if (attempts == 3u) {
    s_modem_baud = H2_ZERO_ESP_MODEM_CMUX_BAUD;
    uart_flush_input(H2_ZERO_ESP_MODEM_UART);
  }
  h2_pal_result_t rc = H2_PAL_ERR_TIMEOUT;
  for (unsigned attempt = 0; attempt < attempts && rc != H2_PAL_OK; ++attempt) {
    const uint32_t remaining =
        remaining_timeout_ms(ctx->timeout_ms, ctx->start);
    if (remaining == 0u) {
      ctx->aborted = true;
      return H2_PAL_ERR_TIMEOUT;
    }
    rc = map_esp_error(zero_esp_modem_at(
        "AT", ctx->response, remaining > 1000u ? 1000u : remaining));
    if (rc != H2_PAL_OK) uart_flush_input(H2_ZERO_ESP_MODEM_UART);
  }
  return rc;
}

static void modem_handshake_send_cld(void* user) {
  (void)user;
  /* A reset can leave the powered module inside CMUX. */
  static const uint8_t cld[] = {0xF9, 0x03, 0xEF, 0x05, 0xC3, 0x01, 0xF2, 0xF9};
  if (uart_set_baudrate(H2_ZERO_ESP_MODEM_UART, s_modem_baud) == ESP_OK) {
    (void)uart_write_bytes(H2_ZERO_ESP_MODEM_UART, cld, sizeof(cld));
    (void)uart_wait_tx_done(H2_ZERO_ESP_MODEM_UART, pdMS_TO_TICKS(100u));
  }
  modem_dtr_wait(300u);
  uart_flush_input(H2_ZERO_ESP_MODEM_UART);
}

static int modem_handshake_probe_low(void* user) {
  modem_handshake_context_t* ctx = user;
  s_modem_baud = H2_ZERO_ESP_MODEM_BAUD;
  uart_flush_input(H2_ZERO_ESP_MODEM_UART);
  const uint32_t remaining = remaining_timeout_ms(ctx->timeout_ms, ctx->start);
  if (remaining == 0u) {
    ctx->aborted = true;
    return H2_PAL_ERR_TIMEOUT;
  }
  return map_esp_error(zero_esp_modem_wait_ready(
      ctx->response, H2_ZERO_ESP_MODEM_AT_RESPONSE_SIZE,
      remaining > 4500u ? 4500u : remaining));
}

static bool modem_handshake_aborted(void* user) {
  return ((modem_handshake_context_t*)user)->aborted;
}

static void modem_handshake_report(void* user, bool cld, int rc) {
  (void)user;
  if (cld)
    ESP_LOGW(TAG, "H2_H106_CELLULAR stage=handshake_cld rc=%d", rc);
  else
    ESP_LOGW(TAG, "H2_H106_CELLULAR stage=handshake_result rc=%d baud=%d", rc,
             s_modem_baud);
}

static void modem_handshake_fresh(void* user, bool cold, bool recovery) {
  modem_handshake_context_t* ctx = user;
  if (!recovery) {
    h2_atomic_store(&s_rail_powered_this_open, cold);
    cold = h2_atomic_exchange(&s_rail_powered_this_open, false);
  }
  if (!cold) return;
  const TickType_t rdy_start = xTaskGetTickCount();
  while (!h2_atomic_load(&s_app_rdy_seen) &&
         (xTaskGetTickCount() - rdy_start) * portTICK_PERIOD_MS < 10000u &&
         remaining_timeout_ms(ctx->timeout_ms, ctx->start) > 500u) {
    vTaskDelay(pdMS_TO_TICKS(100u));
  }
  if (!recovery) {
    ESP_LOGW(TAG, "H2_H106_CELLULAR stage=app_rdy seen=%d elapsed_ms=%lu",
             h2_atomic_load(&s_app_rdy_seen) ? 1 : 0,
             (unsigned long)((xTaskGetTickCount() - rdy_start) *
                             portTICK_PERIOD_MS));
  }
  vTaskDelay(pdMS_TO_TICKS(300u));
}

static int modem_handshake_qsclk(void* user) {
  modem_handshake_context_t* ctx = user;
  const uint32_t remaining = remaining_timeout_ms(ctx->timeout_ms, ctx->start);
  if (remaining == 0u) {
    ctx->aborted = true;
    return H2_PAL_ERR_TIMEOUT;
  }
  return map_esp_error(zero_esp_modem_at(
      "AT+QSCLK=0", ctx->response, remaining > 1000u ? 1000u : remaining));
}

static h2_pal_result_t zero_esp_modem_enable_cmux(uint32_t timeout_ms) {
  const TickType_t start = xTaskGetTickCount();
  h2_pal_result_t rc = zero_esp_modem_init(NULL);
  if (rc != H2_PAL_OK) return rc;
  if (s_cmux_enabled)
    return remaining_timeout_ms(timeout_ms, start) == 0u ? H2_PAL_ERR_TIMEOUT
                                                         : H2_PAL_OK;
  char response[H2_ZERO_ESP_MODEM_AT_RESPONSE_SIZE] = {0};
  uint32_t remaining = remaining_timeout_ms(timeout_ms, start);
  if (remaining == 0u) return H2_PAL_ERR_TIMEOUT;
  ESP_LOGW(TAG, "H2_H106_CELLULAR stage=handshake");
  modem_handshake_context_t ctx = {response, timeout_ms, start, false};
  const h2_modem_handshake_ops_t ops = {
      modem_handshake_probe_high, modem_handshake_send_cld,
      modem_handshake_probe_low,  modem_handshake_aborted,
      modem_handshake_report,     modem_handshake_power_cycle,
      modem_handshake_fresh,      modem_handshake_qsclk};
  rc = h2_modem_handshake_open(&ops, &ctx, H2_PAL_ERR_TIMEOUT);
  if (rc != H2_PAL_OK) return rc;
  remaining = remaining_timeout_ms(timeout_ms, start);
  if (remaining == 0u) return H2_PAL_ERR_TIMEOUT;
  /* Preserve the provider's normal timeout outside this open transaction.
   * The raw AT gate also enforces the shrinking deadline per command. */
  const uint32_t saved_command_timeout_ms = s_modem.config.command_timeout_ms;
  if (saved_command_timeout_ms == 0u || remaining < saved_command_timeout_ms)
    s_modem.config.command_timeout_ms = remaining;
  rc = h2_quectel_modem_prepare(&s_modem);
  s_modem.config.command_timeout_ms = saved_command_timeout_ms;
  if (rc != H2_PAL_OK) return rc;
  remaining = remaining_timeout_ms(timeout_ms, start);
  if (remaining == 0u) return H2_PAL_ERR_TIMEOUT;
  esp_err_t err = ESP_OK;
  if (s_modem_baud != H2_ZERO_ESP_MODEM_CMUX_BAUD) {
    uint32_t at_timeout_ms = remaining > 3000u ? 3000u : remaining;
    err = zero_esp_modem_at("AT+IPR=921600", response, at_timeout_ms);
  }
  if (err == ESP_OK && s_modem_baud != H2_ZERO_ESP_MODEM_CMUX_BAUD) {
    uart_flush(H2_ZERO_ESP_MODEM_UART);
    uart_flush_input(H2_ZERO_ESP_MODEM_UART);
    err =
        uart_set_baudrate(H2_ZERO_ESP_MODEM_UART, H2_ZERO_ESP_MODEM_CMUX_BAUD);
    if (err != ESP_OK) {
      return map_esp_error(err);
    }
    s_modem_baud = H2_ZERO_ESP_MODEM_CMUX_BAUD;
    remaining = remaining_timeout_ms(timeout_ms, start);
    if (remaining == 0u) {
      return H2_PAL_ERR_TIMEOUT;
    }
    uint32_t delay_ms = remaining > 200u ? 200u : remaining;
    modem_dtr_wait(delay_ms);
    uart_flush(H2_ZERO_ESP_MODEM_UART);
    uart_flush_input(H2_ZERO_ESP_MODEM_UART);
    err = ESP_FAIL;
    for (int attempt = 0; attempt < 5; ++attempt) {
      remaining = remaining_timeout_ms(timeout_ms, start);
      if (remaining == 0u) {
        err = ESP_ERR_TIMEOUT;
        break;
      }
      memset(response, 0, sizeof(response));
      uint32_t at_timeout_ms = remaining > 2000u ? 2000u : remaining;
      err = zero_esp_modem_at("AT", response, at_timeout_ms);
      if (err == ESP_OK) {
        break;
      }
      ESP_LOGW(TAG, "high-speed AT verify retry %d failed: %s", attempt + 1,
               esp_err_to_name(err));
      uart_flush(H2_ZERO_ESP_MODEM_UART);
      uart_flush_input(H2_ZERO_ESP_MODEM_UART);
      remaining = remaining_timeout_ms(timeout_ms, start);
      if (remaining > 0u) {
        delay_ms = remaining > 300u ? 300u : remaining;
        modem_dtr_wait(delay_ms);
      }
    }
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "high-speed AT verify failed after AT+IPR=%d: %s",
               H2_ZERO_ESP_MODEM_CMUX_BAUD, esp_err_to_name(err));
      return map_esp_error(err);
    }
  }

  if (err != ESP_OK) return map_esp_error(err);

  ESP_LOGI(TAG, "entering CMUX at baud=%d", s_modem_baud);
  /* The modem answers AT+CMUX in text, then speaks only frames.  Switch
   * first: at worst the AT+CMUX echo/OK is framed twice, never a frame. */
  zero_esp_modem_rx_set_source(ZERO_ESP_MODEM_RX_CMUX);
  err = modem_set_mode(s_dce, ESP_MODEM_MODE_CMUX_MANUAL);
  if (err != ESP_OK) {
    zero_esp_modem_rx_set_source(ZERO_ESP_MODEM_RX_UART);
    ESP_LOGE(TAG, "enter CMUX failed: %s", esp_err_to_name(err));
    return map_esp_error(err);
  }
  s_cmux_enabled = true;
  remaining = remaining_timeout_ms(timeout_ms, start);
  if (remaining == 0u) {
    return H2_PAL_ERR_TIMEOUT;
  }
  uint32_t delay_ms = remaining > 500u ? 500u : remaining;
  vTaskDelay(pdMS_TO_TICKS(delay_ms));
  ESP_LOGI(TAG, "CMUX manual mode enabled");
  return H2_PAL_OK;
}

static h2_pal_result_t zero_esp_modem_open(h2_pal_modem_t* platform,
                                           uint32_t timeout_ms) {
  (void)platform;
  if (s_dce != NULL && s_cmux_enabled && s_modem.opened && s_modem.prepared)
    return H2_PAL_OK;
  h2_pal_result_t rc = zero_esp_modem_enable_cmux(timeout_ms);
  if (rc == H2_PAL_OK && !h2_atomic_load(&s_power_requested))
    rc = H2_PAL_ERR_CLOSED;
  if (rc == H2_PAL_OK) {
    s_modem.opened = 1u;
    h2_atomic_store(&s_open_ready, true);
  }
  ESP_LOGW(TAG, "H2_H106_CELLULAR power_result on=1 stage=prepare rc=%d", rc);
  return rc;
}

static h2_pal_result_t zero_esp_modem_close(h2_pal_modem_t* platform,
                                            uint32_t timeout_ms) {
  (void)platform;
  const h2_pal_result_t rc =
      modem_destroy_transport(timeout_ms == 0u ? 15000u : timeout_ms);
  if (rc == H2_PAL_OK) {
    const h2_pal_result_t reset_rc =
        h2_quectel_modem_transport_closed(&s_modem);
    if (reset_rc == H2_PAL_OK) h2_atomic_store(&s_cleanup_required, false);
    return reset_rc;
  }
  return rc;
}

static h2_pal_result_t zero_esp_modem_data_open(h2_pal_modem_t* platform,
                                                uint32_t timeout_ms) {
  (void)platform;
  const TickType_t start_ticks = xTaskGetTickCount();
  h2_atomic_store(&s_ppp_close_requested, false);
  if (s_ppp_data_mode &&
      (h2_atomic_load(&s_sim_data_invalid) || !modem_data_is_open())) {
    const esp_err_t err = modem_leave_data_mode();
    if (err != ESP_OK) return map_esp_error(err);
  }
  h2_pal_result_t rc;
  if (s_dce == NULL || !s_cmux_enabled) return H2_PAL_ERR_INVALID_STATE;
  if (s_ppp_events == NULL) {
    return H2_PAL_ERR_UNAVAILABLE;
  }

  EventBits_t bits = xEventGroupGetBits(s_ppp_events);
  if ((bits & H2_ZERO_ESP_MODEM_PPP_GOT_IP_BIT) != 0u && modem_data_is_open()) {
    return H2_PAL_OK;
  }

  xEventGroupClearBits(s_ppp_events, H2_ZERO_ESP_MODEM_PPP_GOT_IP_BIT |
                                         H2_ZERO_ESP_MODEM_PPP_STOPPED_BIT |
                                         H2_ZERO_ESP_MODEM_PPP_ERROR_BIT);
  rc = modem_data_state_set(H2_PAL_MODEM_DATA_OPENING, H2_PAL_OK);
  if (rc != H2_PAL_OK) return rc;

  rc = zero_esp_modem_configure_apn_from_imsi();
  if (rc != H2_PAL_OK) {
    (void)modem_data_state_set(H2_PAL_MODEM_DATA_CLOSED, rc);
    return rc;
  }

  uint32_t remaining_ms = remaining_timeout_ms(timeout_ms, start_ticks);
  if (remaining_ms == 0u) {
    (void)modem_data_state_set(H2_PAL_MODEM_DATA_CLOSED, H2_PAL_ERR_TIMEOUT);
    return H2_PAL_ERR_TIMEOUT;
  }

  rc = zero_esp_modem_wait_registered(remaining_ms);
  if (rc != H2_PAL_OK) {
    (void)modem_data_state_set(H2_PAL_MODEM_DATA_CLOSED, rc);
    return rc;
  }

  h2_atomic_store(&s_sim_data_invalid, false);
  esp_err_t err = modem_enter_data_mode();
  /* Reset framing after the temporary command-terminal swap. */
  zero_esp_modem_rx_restart();
  if (err != ESP_OK) {
    const h2_pal_result_t error = map_esp_error(err);
    zero_esp_modem_cleanup_ppp_session(error);
    return error;
  }

  remaining_ms = remaining_timeout_ms(timeout_ms, start_ticks);
  if (remaining_ms == 0u) {
    zero_esp_modem_cleanup_ppp_session(H2_PAL_ERR_TIMEOUT);
    return H2_PAL_ERR_TIMEOUT;
  }

  TickType_t wait_ticks = pdMS_TO_TICKS(remaining_ms);
  h2_atomic_ptr_store(&s_sdk_wait_task, xTaskGetCurrentTaskHandle(),
                      H2_ATOMIC_SEQ_CST);
  bits = xEventGroupWaitBits(s_ppp_events,
                             H2_ZERO_ESP_MODEM_PPP_GOT_IP_BIT |
                                 H2_ZERO_ESP_MODEM_PPP_STOPPED_BIT |
                                 H2_ZERO_ESP_MODEM_PPP_ERROR_BIT,
                             pdFALSE, pdFALSE, wait_ticks);
  h2_atomic_ptr_store(&s_sdk_wait_task, NULL, H2_ATOMIC_SEQ_CST);
  if (!h2_atomic_load(&s_power_requested)) return H2_PAL_ERR_CLOSED;
  if ((bits & H2_ZERO_ESP_MODEM_PPP_GOT_IP_BIT) != 0u && modem_data_is_open()) {
    zero_esp_modem_report_netif_change();
    return H2_PAL_OK;
  }

  h2_pal_result_t failure = (bits & H2_ZERO_ESP_MODEM_PPP_ERROR_BIT) != 0u
                                ? H2_PAL_ERR_IO
                                : H2_PAL_ERR_TIMEOUT;
  zero_esp_modem_cleanup_ppp_session(failure);
  ESP_LOGW(TAG, "PPP open failed bits=0x%" PRIx32 " last_error=%d",
           (uint32_t)bits, failure);
  return failure;
}

static h2_pal_result_t zero_esp_modem_data_close(h2_pal_modem_t* platform,
                                                 uint32_t timeout_ms) {
  (void)platform;
  if (s_dce == NULL) {
    if (s_ppp_events != NULL) {
      xEventGroupClearBits(s_ppp_events, H2_ZERO_ESP_MODEM_PPP_GOT_IP_BIT);
    }
    zero_esp_modem_report_netif_change();
    h2_atomic_store(&s_ppp_close_requested, false);
    zero_esp_modem_mark_data_closed(H2_PAL_OK, 0);
    return H2_PAL_OK;
  }
  h2_pal_modem_data_status_t status = {0};
  const h2_pal_result_t status_rc =
      h2_pal_modem_get_data_status(&s_modem.platform, &status);
  if (status_rc != H2_PAL_OK) return status_rc;
  if (status.state == H2_PAL_MODEM_DATA_CLOSED && !s_ppp_data_mode)
    return H2_PAL_OK;
  if (s_dce != NULL && s_cmux_enabled) {
    h2_atomic_store(&s_ppp_close_requested, true);
    const h2_pal_result_t state_rc =
        modem_data_state_set(H2_PAL_MODEM_DATA_CLOSING, H2_PAL_OK);
    if (state_rc != H2_PAL_OK) return state_rc;
    esp_err_t err = modem_leave_data_mode();
    zero_esp_modem_rx_restart();
    if (err != ESP_OK) {
      h2_pal_result_t result = map_esp_error(err);
      h2_atomic_store(&s_ppp_close_requested, false);
      (void)modem_data_state_set(H2_PAL_MODEM_DATA_CLOSING, result);
      ESP_LOGW(TAG, "PPP close command mode failed: %s", esp_err_to_name(err));
      return result;
    }
  }
  if (s_ppp_events != NULL) {
    xEventGroupClearBits(s_ppp_events, H2_ZERO_ESP_MODEM_PPP_GOT_IP_BIT);
  }
  zero_esp_modem_report_netif_change();
  h2_pal_result_t rc = h2_quectel_modem_drop_ppp(&s_modem);
  if (s_ppp_events != NULL) {
    TickType_t wait_ticks =
        pdMS_TO_TICKS(timeout_ms == 0u ? 2000u : timeout_ms);
    h2_atomic_ptr_store(&s_sdk_wait_task, xTaskGetCurrentTaskHandle(),
                        H2_ATOMIC_SEQ_CST);
    EventBits_t bits =
        xEventGroupWaitBits(s_ppp_events, H2_ZERO_ESP_MODEM_PPP_STOPPED_BIT,
                            pdFALSE, pdFALSE, wait_ticks);
    h2_atomic_ptr_store(&s_sdk_wait_task, NULL, H2_ATOMIC_SEQ_CST);
    if (rc == H2_PAL_OK && (bits & H2_ZERO_ESP_MODEM_PPP_STOPPED_BIT) == 0u) {
      rc = H2_PAL_ERR_TIMEOUT;
    }
  }
  h2_atomic_store(&s_ppp_close_requested, false);
  zero_esp_modem_mark_data_closed(rc, 1);
  return rc;
}

h2_pal_modem_t* h2_esp_board_modem(void) {
  if (!s_modem_initialized) {
    h2_quectel_modem_config_t config = {
        .transport_user = NULL,
        .profile = H2_QUECTEL_MODEM_PROFILE_EC25_UART,
        /* Retain the board's insertion polarity. The public provider owns
         * QSIMDET readback and one configuration restart before CMUX. */
        .sim_hotplug = 1u,
        .sim_insert_level = 1u,
        .invalidate_data = modem_sim_invalidate_data,
        .restart_module = modem_restart_module,
        /* No runtime sleep gate: GPIO6 is the peripheral enable and also
         * drives DTR through an inverter, so letting the modem sleep would
         * cut codec power while awake. Only terminal deep sleep drops it. */
        .init = zero_esp_modem_init,
        .deinit = zero_esp_modem_deinit,
        .command = zero_esp_modem_command,
        .sync_api = h2_esp_board_sync_api(),
        .urc_task_api = h2_esp_board_task_api(),
        .urc_queue_api = h2_esp_board_queue_api(),
        .allocator = h2_esp_board_psram_allocator(),
        .system_events = &s_modem_system_events,
        .capabilities =
            H2_PAL_MODEM_CAPABILITY_DATA | H2_PAL_MODEM_CAPABILITY_CALL,
        .command_timeout_ms = 5000u,
        .io_timeout_ms = 250u,
        .cell_locate_token = H2_ZERO_ESP_MODEM_CELL_LOCATE_TOKEN,
    };
    h2_pal_result_t rc = h2_quectel_modem_init(&s_modem, &config);
    if (rc != H2_PAL_OK) {
      ESP_LOGE(TAG, "modem init failed rc=%d", rc);
      return h2_esp_platform_modem_unsupported();
    }
    static h2_pal_modem_vtable_t vtable;
    vtable = *s_modem.platform.vtable;
    s_provider_get_call_status = vtable.get_call_status;
    vtable.get_call_status = modem_get_call_status;
    vtable.set_apn = zero_esp_modem_set_apn_adapter;
    vtable.open = zero_esp_modem_open_adapter;
    vtable.close = zero_esp_modem_close_adapter;
    vtable.data_open = zero_esp_modem_data_open_adapter;
    vtable.data_close = zero_esp_modem_data_close_adapter;
    s_modem.platform.vtable = &vtable;
    s_modem_initialized = true;
    /* RI must be observable in charging standby before the first open. No
     * synthetic notification: only a real edge/active line admits bootstrap. */
    if (h2_esp_board_modem_is_powered()) {
      const esp_err_t ri_rc = modem_ri_start();
      if (ri_rc != ESP_OK)
        ESP_LOGE(TAG, "standby RI monitor failed rc=%d", (int)ri_rc);
      else if (gpio_get_level(GPIO_NUM_0) == 0)
        modem_ri_notify();
    }
  }
  return h2_quectel_modem_platform(&s_modem);
}
