#include "h2_esp_board_private.h"
#include "h2_esp_board.h"
#include "h2_esp_board_boot_transition.h"
#include "h2_esp_board_sleep_enter.h"
#include "h2_esp_board_sleep_gpio.h"
#include "h2_esp_board_modem_sleep.h"
#include "h2_esp_board_sleep_i2c.h"
#include "h2_esp_tiga_board_internal.h"

#include "h2_esp_board_config.h"

#include "h2_atomic.h"
#include "h2_atomic_static.h"
#include "h2_esp_platform_core.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_private/sleep_uart.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#define H2_TIGA_ESP_DISPLAY_SCLK_GPIO GPIO_NUM_41
#define H2_TIGA_ESP_DISPLAY_MOSI_GPIO GPIO_NUM_42
#define H2_TIGA_ESP_DISPLAY_DC_GPIO GPIO_NUM_39
#define H2_TIGA_ESP_DISPLAY_BL_GPIO GPIO_NUM_46
#define H2_TIGA_ESP_DISPLAY_CS_GPIO GPIO_NUM_40
#define H2_TIGA_ESP_DISPLAY_RST_GPIO GPIO_NUM_45

#define H2_TIGA_ESP_AUDIO_MCLK_GPIO GPIO_NUM_47
#define H2_TIGA_ESP_AUDIO_BCLK_GPIO GPIO_NUM_48
#define H2_TIGA_ESP_AUDIO_WS_GPIO GPIO_NUM_19
#define H2_TIGA_ESP_AUDIO_DOUT_GPIO GPIO_NUM_16

#define H2_TIGA_ESP_MODEM_ENABLE_GPIO GPIO_NUM_12

#define H2_TIGA_ESP_POWER_PREF_NAMESPACE "tiga.power"
#define H2_TIGA_ESP_POWER_BOOT_COUNT_KEY "boot_count"
#define H2_TIGA_ESP_POWER_TRANSITION_KEY "transition"
#define H2_TIGA_ESP_POWER_REASON_KEY "reason"

/* Set by shutdown_commit(physical_off); survives only the ESP deep sleep that
 * follows when external power keeps the MCU alive. */
static RTC_NOINIT_ATTR uint32_t s_physical_off_marker;
/* Report forced modem power removal on the following deep-sleep boot. */
static RTC_NOINIT_ATTR uint32_t s_modem_sleep_fallback_marker;

static int s_power_hold_enabled;
static h2_pal_power_boot_info_t s_boot_info;
/* Deep-sleep wake timer armed by the App; 0 leaves the timer disabled. */
static volatile uint32_t s_deep_sleep_wake_ms;
static h2_pal_result_t s_boot_info_rc = H2_PAL_ERR_INVALID_STATE;
H2_ATOMIC_DEFINE_STATIC(bool, s_boot_info_initialized, false);
static StaticSemaphore_t s_boot_info_mutex_storage;
static SemaphoreHandle_t s_boot_info_mutex;
static portMUX_TYPE s_boot_info_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

static void shutdown_non_wake_peripherals(void);
static const h2_pal_power_api_t* base_power(void);

static SemaphoreHandle_t boot_info_mutex(void) {
  portENTER_CRITICAL(&s_boot_info_mutex_init_lock);
  if (s_boot_info_mutex == NULL) {
    s_boot_info_mutex = xSemaphoreCreateMutexStatic(&s_boot_info_mutex_storage);
  }
  portEXIT_CRITICAL(&s_boot_info_mutex_init_lock);
  if (s_boot_info_mutex == NULL ||
      xSemaphoreTake(s_boot_info_mutex, portMAX_DELAY) != pdTRUE) {
    return NULL;
  }
  (void)xSemaphoreGive(s_boot_info_mutex);
  return s_boot_info_mutex;
}

static h2_pal_result_t close_power_preferences(
    h2_pal_pref_namespace_t* preferences, h2_pal_result_t rc) {
  if (preferences != NULL && preferences->close != NULL) {
    const h2_pal_result_t close_rc =
        (h2_pal_result_t)preferences->close(preferences);
    if (rc == H2_PAL_OK) {
      rc = close_rc;
    }
  }
  return rc;
}

static h2_pal_result_t open_power_preferences(
    h2_pal_pref_namespace_t** out_preferences) {
  if (out_preferences == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_preferences = NULL;
  h2_pal_result_t rc = (h2_pal_result_t)h2_pal_pref_open(
      h2_esp_platform_pref_api(), H2_TIGA_ESP_POWER_PREF_NAMESPACE,
      H2_PAL_PREF_OPEN_READ_WRITE, out_preferences);
  if (rc == H2_PAL_OK && ((*out_preferences)->get_u32 == NULL ||
                          (*out_preferences)->set_u32 == NULL ||
                          (*out_preferences)->remove == NULL ||
                          (*out_preferences)->commit == NULL)) {
    rc = H2_PAL_ERR_UNSUPPORTED;
  }
  return rc;
}

static h2_pal_result_t remove_if_present(h2_pal_pref_namespace_t* preferences,
                                         const char* key) {
  const h2_pal_result_t rc =
      (h2_pal_result_t)preferences->remove(preferences, key);
  return rc == H2_PAL_ERR_NOT_FOUND ? H2_PAL_OK : rc;
}

static h2_pal_result_t save_planned_transition(
    h2_pal_power_previous_transition_t transition, uint32_t reason) {
  h2_pal_pref_namespace_t* preferences = NULL;
  SemaphoreHandle_t mutex = boot_info_mutex();
  if (mutex == NULL || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
    return H2_PAL_ERR_TASK;
  }
  h2_pal_result_t rc = open_power_preferences(&preferences);
  if (rc == H2_PAL_OK) {
    rc = (h2_pal_result_t)preferences->set_u32(
        preferences, H2_TIGA_ESP_POWER_TRANSITION_KEY, (uint32_t)transition);
  }
  if (rc == H2_PAL_OK) {
    rc = (h2_pal_result_t)preferences->set_u32(
        preferences, H2_TIGA_ESP_POWER_REASON_KEY, reason);
  }
  if (rc == H2_PAL_OK) {
    rc = (h2_pal_result_t)preferences->commit(preferences);
  }
  rc = close_power_preferences(preferences, rc);
  (void)xSemaphoreGive(mutex);
  return rc;
}

static h2_pal_result_t discard_planned_transition(void) {
  h2_pal_pref_namespace_t* preferences = NULL;
  SemaphoreHandle_t mutex = boot_info_mutex();
  if (mutex == NULL || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
    return H2_PAL_ERR_TASK;
  }
  h2_pal_result_t rc = open_power_preferences(&preferences);
  if (rc == H2_PAL_OK) {
    rc = remove_if_present(preferences, H2_TIGA_ESP_POWER_TRANSITION_KEY);
  }
  if (rc == H2_PAL_OK) {
    rc = remove_if_present(preferences, H2_TIGA_ESP_POWER_REASON_KEY);
  }
  if (rc == H2_PAL_OK) {
    rc = (h2_pal_result_t)preferences->commit(preferences);
  }
  rc = close_power_preferences(preferences, rc);
  (void)xSemaphoreGive(mutex);
  return rc;
}

static h2_pal_result_t initialize_boot_info(void) {
  h2_pal_pref_namespace_t* preferences = NULL;
  h2_pal_result_t rc = h2_pal_power_get_boot_info(base_power(), &s_boot_info);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if (h2_tiga_modem_sleep_fallback_take(
          &s_modem_sleep_fallback_marker,
          s_boot_info.reset_reason == H2_PAL_POWER_RESET_REASON_DEEP_SLEEP))
    printf(
        "H2_MODEM_SLEEP previous=power_cut reason=prepare_incomplete "
        "restore=user_setting\n");
  rc = open_power_preferences(&preferences);
  uint32_t boot_count = 0u;
  uint32_t transition = (uint32_t)H2_PAL_POWER_PREVIOUS_TRANSITION_UNKNOWN;
  bool transition_found = false;
  uint32_t reason = 0u;
  if (rc == H2_PAL_OK) {
    rc = (h2_pal_result_t)preferences->get_u32(
        preferences, H2_TIGA_ESP_POWER_BOOT_COUNT_KEY, &boot_count);
    if (rc == H2_PAL_ERR_NOT_FOUND) {
      rc = H2_PAL_OK;
      boot_count = 0u;
    }
  }
  if (rc == H2_PAL_OK && boot_count == UINT32_MAX) {
    rc = H2_PAL_ERR_NO_SPACE;
  }
  if (rc == H2_PAL_OK) {
    ++boot_count;
    rc = (h2_pal_result_t)preferences->get_u32(
        preferences, H2_TIGA_ESP_POWER_TRANSITION_KEY, &transition);
    transition_found = rc == H2_PAL_OK;
    if (rc == H2_PAL_ERR_NOT_FOUND) rc = H2_PAL_OK;
  }
  if (rc == H2_PAL_OK) {
    const h2_pal_result_t reason_rc = (h2_pal_result_t)preferences->get_u32(
        preferences, H2_TIGA_ESP_POWER_REASON_KEY, &reason);
    if (reason_rc != H2_PAL_OK && reason_rc != H2_PAL_ERR_NOT_FOUND) {
      rc = reason_rc;
    }
  }
  /* The error-page supervisor commits a physical power-off without
   * shutdown_prepare, so only the RTC marker records that transition. */
  const bool physical_off =
      h2_esp_board_physical_off_marker_take(&s_physical_off_marker);
  if (rc == H2_PAL_OK)
    transition = (uint32_t)h2_esp_board_boot_transition(
        transition_found, transition, s_boot_info.reset_reason, physical_off);
  if (rc == H2_PAL_OK &&
      transition > (uint32_t)H2_PAL_POWER_PREVIOUS_TRANSITION_DEEP_SLEEP) {
    rc = H2_PAL_ERR_FORMAT;
  }
  if (rc == H2_PAL_OK) {
    rc = (h2_pal_result_t)preferences->set_u32(
        preferences, H2_TIGA_ESP_POWER_BOOT_COUNT_KEY, boot_count);
  }
  if (rc == H2_PAL_OK) {
    rc = remove_if_present(preferences, H2_TIGA_ESP_POWER_TRANSITION_KEY);
  }
  if (rc == H2_PAL_OK) {
    rc = remove_if_present(preferences, H2_TIGA_ESP_POWER_REASON_KEY);
  }
  if (rc == H2_PAL_OK) {
    rc = (h2_pal_result_t)preferences->commit(preferences);
  }
  rc = close_power_preferences(preferences, rc);
  if (rc == H2_PAL_OK) {
    s_boot_info.boot_count = boot_count;
    s_boot_info.previous_transition =
        (h2_pal_power_previous_transition_t)transition;
    s_boot_info.transition_reason = reason;
  }
  return rc;
}

static void restore_sleep_gpio(void) {
  static bool restored;
  if (restored) return;
  if (h2_tiga_sleep_gpio_restore() != ESP_OK) abort();
  restored = true;
}

int h2_esp_board_power_button_pressed(void) {
  restore_sleep_gpio();
  static int s_button_configured;
  if (!s_button_configured) {
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << (gpio_num_t)H2_TIGA_ESP_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&config) != ESP_OK) {
      return 0;
    }
    s_button_configured = 1;
  }
  return gpio_get_level((gpio_num_t)H2_TIGA_ESP_BOOT_BUTTON_GPIO) == 0;
}

h2_pal_result_t h2_esp_board_external_power_present(int* out_present) {
  restore_sleep_gpio();
  static int s_charge_detect_configured;
  if (out_present == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!s_charge_detect_configured) {
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << (gpio_num_t)H2_TIGA_ESP_CHARGE_DETECT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&config) != ESP_OK) {
      return H2_PAL_ERR_IO;
    }
    s_charge_detect_configured = 1;
  }
  *out_present =
      gpio_get_level((gpio_num_t)H2_TIGA_ESP_CHARGE_DETECT_GPIO) != 0;
  return H2_PAL_OK;
}

int h2_esp_tiga_board_enable_peripheral_power(void) {
  restore_sleep_gpio();
  const esp_err_t err = h2_tiga_modem_dtr_wake_gpio(
      (gpio_num_t)H2_TIGA_ESP_MODEM_DTR_INVERTED_GPIO);
  return err == ESP_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static const h2_pal_power_api_t* base_power(void) {
  return h2_esp_platform_power_api();
}

static h2_pal_result_t power_get_capabilities(
    void* user, h2_pal_power_capabilities_t* out_capabilities) {
  (void)user;
  h2_pal_result_t rc =
      h2_pal_power_get_capabilities(base_power(), out_capabilities);
  if (rc == H2_PAL_OK) {
    out_capabilities->flags |= H2_PAL_POWER_CAPABILITY_BOOT_SOURCE;
  }
  return rc;
}

static h2_pal_result_t power_get_boot_info(void* user,
                                           h2_pal_power_boot_info_t* out_info) {
  (void)user;
  if (out_info == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  SemaphoreHandle_t mutex = boot_info_mutex();
  if (mutex == NULL || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
    return H2_PAL_ERR_TASK;
  }
  if (!h2_atomic_bool_load(&s_boot_info_initialized, H2_ATOMIC_ACQUIRE)) {
    s_boot_info_rc = initialize_boot_info();
    h2_atomic_bool_store(&s_boot_info_initialized, true, H2_ATOMIC_RELEASE);
  }
  h2_pal_result_t rc = s_boot_info_rc;
  if (rc == H2_PAL_OK) {
    *out_info = s_boot_info;
  }
  (void)xSemaphoreGive(mutex);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  out_info->source = H2_PAL_POWER_BOOT_SOURCE_COLD_BOOT;
  out_info->source_id = 0u;
  if ((esp_sleep_get_wakeup_causes() &
       (1u << (unsigned int)ESP_SLEEP_WAKEUP_EXT1)) != 0u) {
    const uint64_t wake_mask = esp_sleep_get_ext1_wakeup_status();
    out_info->source = H2_PAL_POWER_BOOT_SOURCE_GPIO_IRQ;
    if ((wake_mask & (1ULL << H2_TIGA_ESP_MODEM_RI_GPIO)) != 0u) {
      out_info->source_id = H2_TIGA_ESP_MODEM_RI_ID;
    } else if ((wake_mask & (1ULL << H2_TIGA_ESP_BOOT_BUTTON_GPIO)) != 0u) {
      out_info->source_id = H2_TIGA_ESP_BOOT_BUTTON_ID;
    }
    out_info->previous_transition =
        h2_esp_board_wake_transition(out_info->previous_transition);
  }
  if ((esp_sleep_get_wakeup_causes() &
       (1u << (unsigned int)ESP_SLEEP_WAKEUP_TIMER)) != 0u) {
    out_info->source = H2_PAL_POWER_BOOT_SOURCE_TIMER;
    out_info->source_id = 0u;
    out_info->previous_transition =
        h2_esp_board_wake_transition(out_info->previous_transition);
  }
  if ((esp_sleep_get_wakeup_causes() &
       (1u << (unsigned int)ESP_SLEEP_WAKEUP_EXT0)) != 0u) {
    out_info->source = H2_PAL_POWER_BOOT_SOURCE_GPIO_IRQ;
    out_info->source_id = 0u;
    out_info->previous_transition =
        h2_esp_board_wake_transition(out_info->previous_transition);
  }
  return H2_PAL_OK;
}

static h2_pal_result_t apply_power_hold_level(int requested) {
  if (requested == s_power_hold_enabled) {
    return H2_PAL_OK;
  }
  const gpio_num_t gpio = (gpio_num_t)H2_TIGA_ESP_POWER_HOLD_GPIO;
  esp_err_t err = rtc_gpio_init(gpio);
  if (err == ESP_OK) {
    err = rtc_gpio_set_direction(gpio, RTC_GPIO_MODE_INPUT_OUTPUT);
  }
  if (err == ESP_OK) {
    err = rtc_gpio_set_level(gpio, requested != 0 ? 1u : 0u);
  }
  /* Set the level before leaving any hold: a physical power-off kept alive
   * by USB wakes with the latch held low and must re-latch without a dip. */
  if (err == ESP_OK) {
    err = rtc_gpio_hold_dis(gpio);
  }
  if (err == ESP_OK && requested != 0) {
    err = rtc_gpio_hold_en(gpio);
  }
  const int actual = err == ESP_OK ? (int)rtc_gpio_get_level(gpio) : -1;
  printf(
      "H2_BOARD_POWER_HOLD board=tiga_esp_v4_2 requested=%d actual=%d "
      "code=%d\n",
      requested, actual, (int)err);
  if (err != ESP_OK) {
    return H2_PAL_ERR_IO;
  }
  s_power_hold_enabled = requested;
  return H2_PAL_OK;
}

static h2_pal_result_t power_set_hold(void* user, int enabled) {
  (void)user;
  const int requested = enabled ? 1 : 0;
  if (requested == s_power_hold_enabled) {
    return H2_PAL_OK;
  }
  if (requested != 0 && boot_info_mutex() == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  if (requested != 0 &&
      h2_atomic_bool_load(&s_boot_info_initialized, H2_ATOMIC_ACQUIRE)) {
    /*
     * External USB power can keep the MCU alive after a logical shutdown.
     * Re-enabling the hold in that same boot cancels the persisted transition;
     * otherwise a later unrelated reboot would be attributed to shutdown.
     * On a real power-cycle the boot snapshot has not been consumed yet, so
     * the marker remains available to initialize_boot_info().
     */
    const h2_pal_result_t rc = discard_planned_transition();
    if (rc != H2_PAL_OK) {
      return rc;
    }
  }
  if (requested == 0) {
    const h2_pal_result_t rc =
        save_planned_transition(H2_PAL_POWER_PREVIOUS_TRANSITION_SHUTDOWN, 0u);
    if (rc != H2_PAL_OK) {
      return rc;
    }
    shutdown_non_wake_peripherals();
  }
  const h2_pal_result_t rc = apply_power_hold_level(requested);
  if (rc != H2_PAL_OK) {
    if (requested == 0) {
      (void)discard_planned_transition();
    }
    return rc;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t power_get_hold(void* user,
                                      h2_pal_power_hold_state_t* out_state) {
  (void)user;
  if (out_state == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  out_state->enabled = s_power_hold_enabled;
  return H2_PAL_OK;
}

static h2_pal_result_t power_reboot(void* user, uint32_t reason) {
  (void)user;
  h2_pal_result_t rc =
      save_planned_transition(H2_PAL_POWER_PREVIOUS_TRANSITION_REBOOT, reason);
  if (rc == H2_PAL_OK) {
    rc = h2_pal_power_reboot(base_power(), reason);
  }
  if (rc != H2_PAL_OK) {
    (void)discard_planned_transition();
  }
  return rc;
}

/* Retain the modem only with a current preparation receipt. Inverted DTR
 * stays low in both terminal modes; GPIO15 keeps the MCU rail latched. */
static h2_pal_result_t prepare_sleep_power_domains(bool modem_on) {
  esp_err_t err =
      esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  if (err == ESP_OK) err = h2_tiga_sleep_modem_hold(modem_on);
  if (err == ESP_OK) h2_esp_board_modem_record_power(modem_on);
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
  /* On ESP32-S3 a pad hold outlives deep sleep only with the digital
   * deep-sleep hold enabled as well. */
  if (err == ESP_OK) gpio_deep_sleep_hold_en();
#endif
  return err == ESP_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
}

/* Must run after the wake-source reset in the sleep GPIO preparation. */
static h2_pal_result_t arm_deep_sleep_wake_timer(bool log_result) {
  const uint32_t delay_ms = s_deep_sleep_wake_ms;
  if (delay_ms == 0u) return H2_PAL_OK;
  const esp_err_t err = h2_tiga_sleep_arm_timer(delay_ms);
  if (log_result)
    printf("H2_BOARD_DEEP_SLEEP stage=wake_timer delay_ms=%lu rc=%d\n",
           (unsigned long)delay_ms, err == ESP_OK ? 0 : (int)H2_PAL_ERR_IO);
  return err == ESP_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t power_set_deep_sleep_wake_timer(void* user,
                                                       uint32_t delay_ms) {
  (void)user;
  s_deep_sleep_wake_ms = delay_ms;
  return H2_PAL_OK;
}

static h2_pal_result_t power_deep_sleep(void* user, uint32_t reason) {
  (void)user;
  /* Allocate the internal-stack handoff before any hardware is dismantled. */
  const esp_err_t reserved = h2_tiga_sleep_prepare_deep_sleep();
  if (reserved != ESP_OK)
    return reserved == ESP_ERR_NO_MEM ? H2_PAL_ERR_NO_MEMORY
                                      : H2_PAL_ERR_INVALID_STATE;
  bool transition_saved = false;
  printf("H2_BOARD_DEEP_SLEEP stage=prepare_begin reason=%lu\n",
         (unsigned long)reason);
  h2_pal_result_t rc = h2_esp_board_modem_is_powered()
                           ? h2_esp_board_modem_prepare_sleep(false)
                           : H2_PAL_OK;
  printf("H2_BOARD_DEEP_SLEEP stage=ri_ready rc=%d\n", rc);
  if (rc != H2_PAL_OK) {
    goto failed;
  }
  const uint64_t wake_mask =
      (1ULL << H2_TIGA_ESP_BOOT_BUTTON_GPIO) |
      (h2_esp_board_modem_is_powered() ? (1ULL << H2_TIGA_ESP_MODEM_RI_GPIO)
                                       : 0u);
  printf(
      "H2_BOARD_DEEP_SLEEP stage=wake_levels button_gpio=%u button_level=%d "
      "ri_gpio=%u ri_level=%d wake_mask=0x%016llx\n",
      (unsigned int)H2_TIGA_ESP_BOOT_BUTTON_GPIO,
      gpio_get_level((gpio_num_t)H2_TIGA_ESP_BOOT_BUTTON_GPIO),
      (unsigned int)H2_TIGA_ESP_MODEM_RI_GPIO,
      gpio_get_level((gpio_num_t)H2_TIGA_ESP_MODEM_RI_GPIO),
      (unsigned long long)wake_mask);
  if (esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW) !=
      ESP_OK) {
    printf("H2_BOARD_DEEP_SLEEP stage=wake_enable rc=%d\n", H2_PAL_ERR_IO);
    rc = H2_PAL_ERR_IO;
    goto failed;
  }
  printf("H2_BOARD_DEEP_SLEEP stage=wake_enable rc=0\n");
  rc = save_planned_transition(H2_PAL_POWER_PREVIOUS_TRANSITION_DEEP_SLEEP,
                               reason);
  printf("H2_BOARD_DEEP_SLEEP stage=transition_saved rc=%d\n", rc);
  if (rc != H2_PAL_OK) {
    goto failed;
  }
  transition_saved = true;
  shutdown_non_wake_peripherals();
  if (h2_tiga_sleep_gpio_prepare(!h2_esp_board_modem_is_powered()) != ESP_OK) {
    rc = H2_PAL_ERR_IO;
    goto failed;
  }
  rc = arm_deep_sleep_wake_timer(true);
  if (rc != H2_PAL_OK) {
    goto failed;
  }
  rc = prepare_sleep_power_domains(h2_esp_board_modem_is_powered());
  printf("H2_BOARD_MODEM_RETENTION enable=1 dtr_high=1 rc=%d\n", rc);
  if (rc != H2_PAL_OK) goto failed;
  /* On battery the latched rail also feeds EC25; releasing GPIO15 here drops
   * network standby and RI wake. Only physical shutdown releases it. */
  rc = apply_power_hold_level(1);
  printf("H2_BOARD_DEEP_SLEEP stage=power_hold_retained rc=%d\n", rc);
  if (rc != H2_PAL_OK) {
    goto failed;
  }
  printf("H2_BOARD_DEEP_SLEEP stage=enter\n");
  fflush(stdout);
  const esp_err_t enter_err = h2_tiga_sleep_enter_deep_sleep();
  /* Reaching this point means the chip stayed awake, so the planned
   * transition is discarded and the caller must restore the running App
   * instead of treating the request as a completed sleep. */
  printf("H2_BOARD_DEEP_SLEEP stage=unexpected_return err=%d\n",
         (int)enter_err);
  rc = H2_PAL_ERR_IO;
failed:
  h2_tiga_sleep_cancel_prepared();
  if (transition_saved) (void)discard_planned_transition();
  return rc;
}

static h2_pal_result_t drive_gpio_group_low(uint64_t mask) {
  const gpio_config_t config = {
      .pin_bit_mask = mask,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  if (mask == 0u || gpio_config(&config) != ESP_OK) return H2_PAL_ERR_IO;
  for (int gpio = 0; gpio < GPIO_NUM_MAX && gpio < 64; ++gpio) {
    if ((mask & (1ULL << (uint32_t)gpio)) != 0u &&
        gpio_set_level((gpio_num_t)gpio, 0) != ESP_OK)
      return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}

void h2_esp_board_quiesce_peripherals(void);

static void shutdown_non_wake_peripherals(void) {
  h2_pal_display_t* display = h2_esp_board_display_if_initialized();
  const int display_rc =
      display != NULL ? h2_pal_display_close(display) : H2_PAL_OK;
  printf("H2_BOARD_DEEP_SLEEP stage=panel_sleep rc=%d\n", display_rc);
  const int led_rc = h2_esp_board_led_prepare_sleep();
  printf("H2_BOARD_DEEP_SLEEP stage=led_off rc=%d\n", led_rc);
  h2_pal_audio_t* audio = h2_esp_board_audio_if_initialized();
  if (audio != NULL) {
    (void)h2_pal_audio_stop_mic(audio);
    (void)h2_pal_audio_stop_speaker(audio);
  }
  /* NFC needs no scan initialization to enter soft power-down. An I2C
   * error must not prevent GPIO shutdown or terminal deep sleep. */
  (void)h2_tiga_sleep_nfc_prepare(h2_esp_tiga_board_i2c_bus());
  h2_tiga_sleep_codec_prepare(h2_esp_board_audio_power_down);

  drive_gpio_group_low((1ULL << H2_TIGA_ESP_DISPLAY_SCLK_GPIO) |
                       (1ULL << H2_TIGA_ESP_DISPLAY_MOSI_GPIO) |
                       (1ULL << H2_TIGA_ESP_DISPLAY_DC_GPIO) |
                       (1ULL << H2_TIGA_ESP_DISPLAY_BL_GPIO) |
                       (1ULL << H2_TIGA_ESP_DISPLAY_CS_GPIO) |
                       (1ULL << H2_TIGA_ESP_DISPLAY_RST_GPIO));
  drive_gpio_group_low((1ULL << H2_TIGA_ESP_AUDIO_MCLK_GPIO) |
                       (1ULL << H2_TIGA_ESP_AUDIO_BCLK_GPIO) |
                       (1ULL << H2_TIGA_ESP_AUDIO_WS_GPIO) |
                       (1ULL << H2_TIGA_ESP_AUDIO_DOUT_GPIO) |
                       (1ULL << H2_TIGA_ESP_AUDIO_PA_GPIO));
  drive_gpio_group_low((1ULL << H2_TIGA_ESP_LED_STRIP_GPIO) |
                       (1ULL << H2_TIGA_ESP_VIBRATION_GPIO) |
                       (1ULL << H2_TIGA_ESP_AUDIO_MODEM_ROUTE_GPIO));
}

/* Drives every non-wake peripheral rail and bus pin low; used by the
 * charging-idle boot gate so a logically-off device charges with all
 * peripheral pins actively held low instead of floating. */
void h2_esp_board_quiesce_peripherals(void) {
  shutdown_non_wake_peripherals();
  if (drive_gpio_group_low(1ULL << H2_TIGA_ESP_MODEM_ENABLE_GPIO) == H2_PAL_OK)
    h2_esp_board_modem_record_power(false);
}

const h2_pal_power_api_t* h2_esp_board_power_api(void) {
  static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;
  static h2_pal_power_vtable_t vtable;
  static h2_pal_power_api_t api;
  const h2_pal_power_api_t* native = base_power();
  portENTER_CRITICAL(&init_lock);
  if (api.vtable == NULL) {
    /* Keep native boot/image operations and their borrowed callback context. */
    vtable = *native->vtable;
    vtable.get_capabilities = power_get_capabilities;
    vtable.get_boot_info = power_get_boot_info;
    vtable.set_hold = power_set_hold;
    vtable.get_hold = power_get_hold;
    vtable.reboot = power_reboot;
    vtable.deep_sleep = power_deep_sleep;
    vtable.set_deep_sleep_wake_timer = power_set_deep_sleep_wake_timer;
    api = *native;
    api.vtable = &vtable;
  }
  portEXIT_CRITICAL(&init_lock);
  return &api;
}

const h2_pal_power_api_t* h2_esp_board_power(void) {
  return h2_esp_board_power_api();
}

h2_pal_result_t h2_esp_board_shutdown_prepare(void* user, bool physical_off) {
  (void)user;
  const bool prepare_modem = !physical_off && h2_esp_board_modem_is_powered();
  h2_pal_result_t rc =
      prepare_modem ? h2_esp_board_modem_prepare_sleep(true) : H2_PAL_OK;
  if (rc != H2_PAL_OK)
    printf("H2_MODEM_SLEEP prepare_failed rc=%d fallback=power_cut\n", rc);
  rc = save_planned_transition(
      physical_off ? H2_PAL_POWER_PREVIOUS_TRANSITION_SHUTDOWN
                   : H2_PAL_POWER_PREVIOUS_TRANSITION_DEEP_SLEEP,
      0u);
  /* A modem/storage error must not skip the remaining best-effort shutdown. */
  shutdown_non_wake_peripherals();
  return rc;
}

void h2_esp_board_shutdown_commit(void* user, bool physical_off) {
  (void)user;
  const bool retained = h2_esp_board_modem_sleep_commit();
  const bool powered = h2_esp_board_modem_is_powered();
  const bool modem_on = h2_tiga_sleep_modem_on(physical_off, powered, retained);
  h2_tiga_modem_sleep_fallback_set(&s_modem_sleep_fallback_marker,
                                   !physical_off && powered && !modem_on);
  h2_esp_board_physical_off_marker_set(&s_physical_off_marker, physical_off);
  /* Do not call the normal PAL path here: its AT, audio and NVS waits can
   * depend on a worker that failed to exit. This is the sole terminal owner;
   * it deliberately retains all software resources until hardware stops.
   * NFC soft power-down is attempted by shutdown_prepare only.
   * Codec power-down is skipped here: worker/I2C waits can block the
   * supervisor. I2C device add/remove can block on SDK locks without a timeout,
   * so commit MUST skip those writes (and logging) even if preparation never
   * reached them. */
  uint64_t wake_mask = 1ULL << H2_TIGA_ESP_BOOT_BUTTON_GPIO;
  if (modem_on) wake_mask |= 1ULL << H2_TIGA_ESP_MODEM_RI_GPIO;
  if (esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL) != ESP_OK ||
      esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW) !=
          ESP_OK)
    abort();
  if (h2_tiga_sleep_gpio_prepare(!modem_on) != ESP_OK) abort();
  /* A failed wake timer must not abort the terminal path: the device still
   * sleeps, it just misses the scheduled wake. */
  if (!physical_off) (void)arm_deep_sleep_wake_timer(false);
  /* Only rail/enable pins: no driver stop and no SDK peripheral mutex. */
  if (drive_gpio_group_low((1ULL << H2_TIGA_ESP_DISPLAY_BL_GPIO) |
                           (1ULL << H2_TIGA_ESP_MODEM_DTR_INVERTED_GPIO) |
                           (1ULL << H2_TIGA_ESP_AUDIO_PA_GPIO) |
                           (1ULL << H2_TIGA_ESP_LED_STRIP_GPIO)) != H2_PAL_OK)
    abort();
  if (!modem_on &&
      drive_gpio_group_low(1ULL << H2_TIGA_ESP_MODEM_ENABLE_GPIO) != H2_PAL_OK)
    abort();
  if (prepare_sleep_power_domains(modem_on) != H2_PAL_OK) abort();
  /* Deep sleep keeps the latch RTC-held high so EC25 stays powered for
   * network standby and RI wake. Physical shutdown holds it low: sleep
   * starts before the rail collapses, and an unheld pad stops driving the
   * latch off, leaving the ESP powered in deep sleep. */
  const gpio_num_t hold_gpio = (gpio_num_t)H2_TIGA_ESP_POWER_HOLD_GPIO;
  if (rtc_gpio_init(hold_gpio) != ESP_OK ||
      rtc_gpio_set_direction(hold_gpio, RTC_GPIO_MODE_INPUT_OUTPUT) != ESP_OK ||
      rtc_gpio_set_level(hold_gpio, physical_off ? 0u : 1u) != ESP_OK ||
      rtc_gpio_hold_dis(hold_gpio) != ESP_OK ||
      rtc_gpio_hold_en(hold_gpio) != ESP_OK)
    abort();
  /* External power may keep the CPU alive after a physical hold drop. Sleep
   * must still terminate execution, without returning to the dismantled App. */
  /* IDF 6 normally flushes every UART before deep sleep. An AT UART can be
   * flow-controlled by the modem, so even ROM FIFO draining is unsafe here.
   * This SDK-private configuration is confined to the private ESP BSP. */
  for (int uart = 0; uart < SOC_UART_HP_NUM; ++uart)
    if (sleep_uart_set_handling_mode(uart, ESP_SLEEP_NO_HANDLING) != ESP_OK)
      abort();
  (void)h2_tiga_sleep_enter_deep_sleep();
  abort();
}
