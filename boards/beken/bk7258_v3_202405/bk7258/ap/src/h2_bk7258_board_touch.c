#include "h2_bk7258_board_private.h"

#if defined(H2_BK7258_BOARD_TOUCH_HOST_TEST)
#include "h2_bk7258_board_touch_test_sdk.h"
#elif defined(CONFIG_TP) && CONFIG_TP
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/tp.h"
#include "gpio_driver.h"
#include "os/os.h"
#include "tp_sensor_devices.h"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#if defined(CONFIG_TP) && CONFIG_TP

/* This SDK export runs its registered chip-ID detectors without starting the
 * SDK TP task. drv_tp_open ignores detection errors and drv_tp_close destroys
 * callback storage before stopping that task; neither owns this PAL session. */
extern const tp_sensor_config_t* tp_get_sensor_auto_detect(
    const tp_i2c_callback_t* cb);

#ifndef SOC_I2C_UNIT_NUM
#define SOC_I2C_UNIT_NUM 2
#endif
#define TOUCH_I2C_ID SOC_I2C_UNIT_NUM
#define TOUCH_QUEUE_SIZE 32u
#define TOUCH_STATUS_REG 0x814eu
#define TOUCH_READY 0x80u
#define TOUCH_CONTACTS 0x0fu
#define TOUCH_RETRY_MS 50u
/* Some vendor readers index by hardware tracking ID rather than max_num. */
#define TOUCH_SAMPLE_COUNT 11u

static struct {
  const tp_sensor_config_t* sensor;
  h2_pal_task_t* task;
  h2_pal_touch_info_t info;
  uint16_t source_width;
  uint16_t source_height;
  bool xy_exchange;
  unsigned reported_points;
  h2_pal_touch_event_t events[TOUCH_QUEUE_SIZE];
  size_t head;
  size_t count;
  h2_pal_result_t error;  /* Terminal for this session. */
  const char* pending_io; /* Delivered before any subsequent gesture. */
  bool discard_contact, fault_reported;
  unsigned sync_logs;
  /* Written only by the worker during a GT911 SDK report read. */
  uint8_t report_status;
  const char* report_stage;
  bool opened;
  bool stop;
  bool pending;
  bool pressed;
  uint8_t track_id;
  bool reset_owned;
  bool interrupt_owned;
  bool isr_registered;
  bool bus_owned;
} s_touch;

static const tp_i2c_callback_t s_touch_i2c = {
    .read_uint8 = tp_i2c_read_uint8,
    .write_uint8 = tp_i2c_write_uint8,
    .read_uint16 = tp_i2c_read_uint16,
    .write_uint16 = tp_i2c_write_uint16,
};

static void touch_interrupt(gpio_id_t id) {
  (void)id;
  uint32_t flags = rtos_enter_critical();
  if (!s_touch.stop) s_touch.pending = true;
  rtos_exit_critical(flags);
}

static h2_pal_result_t touch_read_coordinate_range(bool report) {
  uint16_t address;
  switch (s_touch.sensor->id) {
    case TP_ID_GT911:
      address = 0x8048u;
      break;
    case TP_ID_GT1151:
      address = 0x8051u;
      break;
    default:
      return H2_PAL_ERR_UNSUPPORTED;
  }
  uint8_t bytes[6] = {0};
  if (s_touch_i2c.read_uint16(s_touch.sensor->address, address, bytes,
                              sizeof(bytes)) != BK_OK)
    return H2_PAL_ERR_IO;
  s_touch.source_width = (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
  s_touch.source_height = (uint16_t)bytes[2] | (uint16_t)bytes[3] << 8;
  s_touch.xy_exchange = (bytes[5] & 0x08u) != 0u;
  if (report)
    printf(
        "H2_BK_TOUCH_CONFIG sensor=%s source_width=%u source_height=%u "
        "display_width=%lu display_height=%lu xy_exchange=%u\r\n",
        s_touch.sensor->name ? s_touch.sensor->name : "unknown",
        (unsigned)s_touch.source_width, (unsigned)s_touch.source_height,
        (unsigned long)s_touch.info.width, (unsigned long)s_touch.info.height,
        (unsigned)s_touch.xy_exchange);
  if (s_touch.source_width == 0u || s_touch.source_height == 0u)
    return H2_PAL_ERR_FORMAT;
  return H2_PAL_OK;
}

static int touch_coordinate(uint16_t value, uint16_t source_size,
                            uint32_t display_size) {
  if (source_size == 1u) return 0;
  return (int)((uint32_t)value * (display_size - 1u) / (source_size - 1u));
}

/* Called under the queue lock. Invalidate both halves of any queued gesture.
 * The consumer gets an I/O error to cancel even if recovery wins the race to
 * its next poll. Only another failed transaction can publish another error. */
static void touch_report_fault(const char* stage) {
  if (s_touch.sensor->id != TP_ID_GT911) {
    s_touch.error = H2_PAL_ERR_IO;
    return;
  }
  if (s_touch.pending_io == NULL) s_touch.pending_io = stage;
  s_touch.count = 0u;
  s_touch.pressed = false;
  s_touch.discard_contact = true;
}

static void touch_record_locked(const tp_data_t* sample) {
  if (s_touch.error != H2_PAL_OK) return;
  if (sample->event != TP_EVENT_TYPE_DOWN &&
      sample->event != TP_EVENT_TYPE_MOVE && sample->event != TP_EVENT_TYPE_UP)
    return;
  if (s_touch.pressed && sample->track_id != s_touch.track_id) return;
  if (!s_touch.pressed && sample->event == TP_EVENT_TYPE_UP) return;
  if (sample->x_coordinate >= s_touch.source_width ||
      sample->y_coordinate >= s_touch.source_height) {
    touch_report_fault("bounds");
    return;
  }
  if (s_touch.discard_contact) return;
  h2_pal_touch_event_t event = {
      .kind = sample->event == TP_EVENT_TYPE_UP
                  ? H2_PAL_TOUCH_EVENT_UP
                  : (s_touch.pressed ? H2_PAL_TOUCH_EVENT_MOVE
                                     : H2_PAL_TOUCH_EVENT_DOWN),
      .x = touch_coordinate(sample->x_coordinate, s_touch.source_width,
                            s_touch.info.width),
      .y = touch_coordinate(sample->y_coordinate, s_touch.source_height,
                            s_touch.info.height),
  };
  /* Coalesce motion only. A quick DOWN/UP must survive between PAL polls. */
  if (event.kind == H2_PAL_TOUCH_EVENT_MOVE && s_touch.count != 0u) {
    size_t tail = (s_touch.head + s_touch.count - 1u) % TOUCH_QUEUE_SIZE;
    if (s_touch.events[tail].kind == H2_PAL_TOUCH_EVENT_MOVE) {
      s_touch.events[tail] = event;
      return;
    }
  }
  if (s_touch.count == TOUCH_QUEUE_SIZE) {
    /* Losing an edge cannot be represented as a successful touch gesture. */
    s_touch.error = H2_PAL_ERR_FULL;
    return;
  }
  s_touch.events[(s_touch.head + s_touch.count) % TOUCH_QUEUE_SIZE] = event;
  ++s_touch.count;
  s_touch.pressed = event.kind != H2_PAL_TOUCH_EVENT_UP;
  s_touch.track_id = sample->track_id;
}

/* The SDK re-reads status, decodes points (possibly updating its private
 * tracking), then ACKs even on a read failure. Observe the count it actually
 * decoded. In particular, a zero-finger report need not produce an SDK UP. */
static int gt911_report_read(uint8_t addr, uint16_t reg, uint8_t* data,
                             uint16_t len) {
  int rc = tp_i2c_read_uint16(addr, reg, data, len);
  if (reg == TOUCH_STATUS_REG && len == 1u) {
    s_touch.report_stage = "report-status";
    if (rc == BK_OK) {
      s_touch.report_status = data[0];
      if ((data[0] & TOUCH_READY) &&
          (data[0] & TOUCH_CONTACTS) > TP_SUPPORT_MAX_NUM) {
        s_touch.report_stage = "count";
        return -1; /* The SDK's point buffer holds TP_SUPPORT_MAX_NUM. */
      }
    }
  } else {
    s_touch.report_stage = rc == BK_OK ? "ack" : "data";
  }
  return rc;
}

static bool touch_read_report(const tp_i2c_callback_t* reader) {
  uint8_t status = 0;
  tp_data_t samples[TOUCH_SAMPLE_COUNT] = {0};
  const char* fault = NULL;
  if (s_touch_i2c.read_uint16(s_touch.sensor->address, TOUCH_STATUS_REG,
                              &status, sizeof(status)) != BK_OK) {
    fault = "status";
  } else if (!(status & TOUCH_READY)) {
    return true; /* Idle says nothing about physical release. */
  } else {
    s_touch.report_status = 0;
    s_touch.report_stage = "report";
    if (s_touch.sensor->read_tp_info(reader, TP_SUPPORT_MAX_NUM,
                                     (uint8_t*)samples) != BK_OK)
      fault = s_touch.report_stage;
  }

  uint32_t flags = rtos_enter_critical();
  if (!s_touch.stop && s_touch.error == H2_PAL_OK) {
    if (fault != NULL) {
      touch_report_fault(fault);
    } else if (s_touch.discard_contact &&
               (s_touch.report_status & (TOUCH_READY | TOUCH_CONTACTS)) ==
                   TOUCH_READY) {
      /* Discard this ACKed release too, including any stale UP coordinates. */
      s_touch.discard_contact = false;
    } else {
      for (size_t i = 0u; i < TOUCH_SAMPLE_COUNT; ++i)
        touch_record_locked(&samples[i]);
    }
  }
  rtos_exit_critical(flags);
  return fault == NULL;
}

static void touch_worker(void* ctx) {
  (void)ctx;
  const bool gt911 = s_touch.sensor->id == TP_ID_GT911;
  tp_i2c_callback_t reader = s_touch_i2c;
  if (gt911) reader.read_uint16 = gt911_report_read;
  bool retry = false;
  for (;;) {
    uint32_t flags = rtos_enter_critical();
    bool stop = s_touch.stop;
    bool pending = s_touch.pending;
    s_touch.pending = false;
    rtos_exit_critical(flags);
    if (stop) return;
    /* A failed status read or ACK can leave INT asserted without a new edge.
     * Retry on the existing reader task. Resume IRQ-only sampling on success.
     */
    if (pending || retry) retry = !touch_read_report(&reader) && gt911;
    if (rtos_delay_milliseconds(retry ? TOUCH_RETRY_MS : 5u) != BK_OK) {
      flags = rtos_enter_critical();
      if (s_touch.error == H2_PAL_OK) s_touch.error = H2_PAL_ERR_IO;
      rtos_exit_critical(flags);
      return;
    }
  }
}

static h2_pal_result_t touch_cleanup(void) {
  uint32_t flags = rtos_enter_critical();
  s_touch.stop = true;
  s_touch.opened = false;
  rtos_exit_critical(flags);
  if (s_touch.interrupt_owned &&
      bk_gpio_disable_interrupt(CONFIG_TP_INT_GPIO_ID) != BK_OK)
    return H2_PAL_ERR_IO;
  if (s_touch.isr_registered) {
    /* The pinned SDK declares unregister_isr but exports callback removal
     * through register_isr(NULL), also used by its native SDIO teardown. */
    if (bk_gpio_register_isr(CONFIG_TP_INT_GPIO_ID, NULL) != BK_OK)
      return H2_PAL_ERR_IO;
    s_touch.isr_registered = false;
  }
  if (s_touch.task != NULL) {
    h2_pal_result_t result =
        h2_pal_task_join(h2_bk7258_board_task_api(), s_touch.task);
    if (result != H2_PAL_OK) return result;
    s_touch.task = NULL;
  }
  if (s_touch.bus_owned) {
#if CONFIG_SIM_I2C_HW_BOARD_V3
    if (bk_i2c_deinit_v2(TOUCH_I2C_ID) != BK_OK) return H2_PAL_ERR_IO;
#else
    if (bk_i2c_deinit(TOUCH_I2C_ID) != BK_OK) return H2_PAL_ERR_IO;
#endif
    s_touch.bus_owned = false;
  }
  gpio_config_t input = {.io_mode = GPIO_INPUT_ENABLE,
                         .pull_mode = GPIO_PULL_DISABLE};
  if (s_touch.reset_owned) {
    if (bk_gpio_set_config(CONFIG_TP_RST_GPIO_ID, &input) != BK_OK)
      return H2_PAL_ERR_IO;
    s_touch.reset_owned = false;
  }
  if (s_touch.interrupt_owned) {
    if (bk_gpio_set_config(CONFIG_TP_INT_GPIO_ID, &input) != BK_OK)
      return H2_PAL_ERR_IO;
    s_touch.interrupt_owned = false;
  }
  memset(&s_touch, 0, sizeof(s_touch));
  return H2_PAL_OK;
}

static h2_pal_result_t touch_open(void* user) {
  (void)user;
  if (s_touch.opened) return H2_PAL_ERR_BUSY;
  if (s_touch.task != NULL || s_touch.reset_owned || s_touch.interrupt_owned ||
      s_touch.bus_owned)
    return H2_PAL_ERR_BUSY;
  /* Share the display's actual viewport; do not infer it from a sensor model.
   */
  h2_display_info_t display;
  h2_pal_result_t result =
      h2_pal_display_get_info(h2_bk7258_board_display(), &display);
  if (result != H2_PAL_OK) return result;
  if (display.width <= 0 || display.height <= 0 || display.width > UINT16_MAX ||
      display.height > UINT16_MAX)
    return H2_PAL_ERR_INVALID_STATE;
  s_touch.info.width = (uint32_t)display.width;
  s_touch.info.height = (uint32_t)display.height;
  s_touch.stop = true;
  result = H2_PAL_ERR_IO;

  /* Match tp_driver.c's address-select/reset sequence, using configured pins.
   * These are SDK board settings, not a hard-coded claim about an attached LCD.
   */
  gpio_config_t output = {.io_mode = GPIO_OUTPUT_ENABLE,
                          .pull_mode = GPIO_PULL_DISABLE};
  if (gpio_dev_unmap(CONFIG_TP_INT_GPIO_ID) != BK_OK) goto fail;
  s_touch.interrupt_owned = true;
  if (bk_gpio_disable_interrupt(CONFIG_TP_INT_GPIO_ID) != BK_OK ||
      bk_gpio_set_config(CONFIG_TP_INT_GPIO_ID, &output) != BK_OK ||
      bk_gpio_set_output_high(CONFIG_TP_INT_GPIO_ID) != BK_OK)
    goto fail;
  if (gpio_dev_unmap(CONFIG_TP_RST_GPIO_ID) != BK_OK) goto fail;
  s_touch.reset_owned = true;
  if (bk_gpio_set_config(CONFIG_TP_RST_GPIO_ID, &output) != BK_OK ||
      bk_gpio_set_output_low(CONFIG_TP_RST_GPIO_ID) != BK_OK)
    goto fail;
#if CONFIG_TP_HY4633
  if (rtos_delay_milliseconds(220u) != BK_OK) goto fail;
#elif CONFIG_TP_FT6336
  if (rtos_delay_milliseconds(20u) != BK_OK) goto fail;
#else
  if (rtos_delay_milliseconds(2u) != BK_OK) goto fail;
#endif
  if (bk_gpio_set_output_high(CONFIG_TP_RST_GPIO_ID) != BK_OK) goto fail;
  i2c_config_t bus = {.baud_rate = I2C_BAUD_RATE_100KHZ,
                      .addr_mode = I2C_ADDR_MODE_7BIT};
#if CONFIG_SIM_I2C_HW_BOARD_V3
  if (bk_i2c_init_v2(TOUCH_I2C_ID, &bus) != BK_OK) goto fail;
#else
  if (bk_i2c_init(TOUCH_I2C_ID, &bus) != BK_OK) goto fail;
#endif
  s_touch.bus_owned = true;
  if (rtos_delay_milliseconds(10u) != BK_OK) goto fail;
  gpio_config_t input = {.io_mode = GPIO_INPUT_ENABLE,
                         .pull_mode = GPIO_PULL_DISABLE};
  /* Release the address-select line before controller detection/configuration,
   * as bk_tp_int_init does in the pinned SDK. Keep IRQ delivery disabled. */
  if (bk_gpio_set_config(CONFIG_TP_INT_GPIO_ID, &input) != BK_OK) goto fail;
#if CONFIG_TP_HY4633
  if (rtos_delay_milliseconds(1000u) != BK_OK) goto fail;
#elif CONFIG_TP_FT6336
  if (rtos_delay_milliseconds(300u) != BK_OK) goto fail;
#else
  if (rtos_delay_milliseconds(100u) != BK_OK) goto fail;
#endif
  tp_sensor_devices_init();
  s_touch.sensor = tp_get_sensor_auto_detect(&s_touch_i2c);
  if (s_touch.sensor == NULL) {
    result = H2_PAL_ERR_NOT_FOUND;
    goto fail;
  }
  if (s_touch.sensor->init == NULL || s_touch.sensor->read_tp_info == NULL)
    goto fail;
  result = touch_read_coordinate_range(false);
  if (result == H2_PAL_ERR_FORMAT) {
    /* Some panels expose zero ranges until their first configuration. Use
     * the detected controller's native profile only to initialize it; the
     * post-init register read remains authoritative for coordinate mapping. */
    uint32_t native_ppi = (uint32_t)s_touch.sensor->def_ppi;
    s_touch.source_width = (uint16_t)(native_ppi >> 16);
    s_touch.source_height = (uint16_t)native_ppi;
    if (s_touch.source_width == 0u || s_touch.source_height == 0u) goto fail;
  } else if (result != H2_PAL_OK) {
    goto fail;
  }
  /* Keep the panel's calibrated coordinate geometry. Conversion to display
   * pixels belongs to this PAL adapter, rather than controller reprogramming.
   */
  tp_sensor_user_config_t config = {
      .x_size = s_touch.source_width,
      .y_size = s_touch.source_height,
      .int_type = s_touch.sensor->def_int_type,
      .refresh_rate = s_touch.sensor->def_refresh_rate,
      .tp_num = TP_SUPPORT_MAX_NUM,
  };
  result = H2_PAL_ERR_IO;
  if (s_touch.sensor->init(&s_touch_i2c, &config) != BK_OK) goto fail;
  /* SDK GT1151 ignores the requested sizes; GT911 may retain a newer stored
   * configuration. Read the effective ranges rather than assuming acceptance.
   */
  if (rtos_delay_milliseconds(10u) != BK_OK) goto fail;
  result = touch_read_coordinate_range(true);
  if (result != H2_PAL_OK) goto fail;
  result = H2_PAL_ERR_IO;
  gpio_int_type_t edge;
  switch (config.int_type) {
    case TP_INT_TYPE_RISING_EDGE:
      edge = GPIO_INT_TYPE_RISING_EDGE;
      break;
    case TP_INT_TYPE_FALLING_EDGE:
      edge = GPIO_INT_TYPE_FALLING_EDGE;
      break;
    default:
      result = H2_PAL_ERR_UNSUPPORTED;
      goto fail;
  }
  if (bk_gpio_set_config(CONFIG_TP_INT_GPIO_ID, &input) != BK_OK ||
      bk_gpio_set_interrupt_type(CONFIG_TP_INT_GPIO_ID, edge) != BK_OK ||
      bk_gpio_clear_interrupt(CONFIG_TP_INT_GPIO_ID) != BK_OK ||
      bk_gpio_register_isr(CONFIG_TP_INT_GPIO_ID, touch_interrupt) != BK_OK)
    goto fail;
  s_touch.isr_registered = true;
  s_touch.stop = false;
  const h2_pal_task_options_t options = {.name = "h2_touch",
                                         .min_stack_size = 2048u};
  result = h2_pal_task_start(h2_bk7258_board_task_api(), &options, touch_worker,
                             NULL, &s_touch.task);
  if (result != H2_PAL_OK) goto fail;
  result = H2_PAL_ERR_IO;
  if (bk_gpio_enable_interrupt(CONFIG_TP_INT_GPIO_ID) != BK_OK) goto fail;
  /* A sample may already hold INT low before edge delivery is enabled. Drain
   * that initial status instead of waiting for a falling edge that has passed.
   */
  uint32_t flags = rtos_enter_critical();
  s_touch.pending = true;
  s_touch.opened = true;
  rtos_exit_critical(flags);
  return H2_PAL_OK;
fail: {
  h2_pal_result_t cleanup = touch_cleanup();
  return cleanup != H2_PAL_OK ? cleanup : result;
}
}

static h2_pal_result_t touch_get_info(void* user,
                                      h2_pal_touch_info_t* out_info) {
  (void)user;
  if (out_info == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!s_touch.opened) return H2_PAL_ERR_INVALID_STATE;
  *out_info = s_touch.info;
  return H2_PAL_OK;
}

static h2_pal_result_t touch_poll_event(void* user,
                                        h2_pal_touch_event_t* out_event) {
  (void)user;
  if (out_event == NULL) return H2_PAL_ERR_INVALID_ARG;
  bool report = false;
  const char* sync_stage = NULL;
  uint32_t flags = rtos_enter_critical();
  h2_pal_result_t result = H2_PAL_ERR_WOULD_BLOCK;
  if (!s_touch.opened)
    result = H2_PAL_ERR_INVALID_STATE;
  else if (s_touch.error != H2_PAL_OK)
    result = s_touch.error;
  else if (s_touch.pending_io != NULL) {
    if (!s_touch.fault_reported) sync_stage = s_touch.pending_io;
    s_touch.pending_io = NULL;
    s_touch.fault_reported = true;
    result = H2_PAL_ERR_IO;
  } else if (s_touch.count != 0u) {
    *out_event = s_touch.events[s_touch.head];
    s_touch.head = (s_touch.head + 1u) % TOUCH_QUEUE_SIZE;
    --s_touch.count;
    result = H2_PAL_OK;
    if (out_event->kind != H2_PAL_TOUCH_EVENT_MOVE &&
        s_touch.reported_points < 64u) {
      ++s_touch.reported_points;
      report = true;
    }
  }
  if (s_touch.fault_reported && !s_touch.discard_contact &&
      (result == H2_PAL_OK || result == H2_PAL_ERR_WOULD_BLOCK)) {
    sync_stage = "released";
    s_touch.fault_reported = false;
  }
  if (sync_stage != NULL && s_touch.sync_logs < 16u)
    ++s_touch.sync_logs;
  else
    sync_stage = NULL;
  rtos_exit_critical(flags);
  /* Log on the caller task, never in the ISR or the small reader stack. */
  if (sync_stage != NULL) printf("H2_BK_TOUCH_SYNC stage=%s\r\n", sync_stage);
  if (report)
    printf("H2_BK_TOUCH_POINT kind=%s x=%ld y=%ld\r\n",
           out_event->kind == H2_PAL_TOUCH_EVENT_DOWN ? "down" : "up",
           (long)out_event->x, (long)out_event->y);
  return result;
}

static h2_pal_result_t touch_close(void* user) {
  (void)user;
  return touch_cleanup();
}

#else

static h2_pal_result_t touch_open(void* user) {
  (void)user;
  return H2_PAL_ERR_UNAVAILABLE;
}

static h2_pal_result_t touch_get_info(void* user,
                                      h2_pal_touch_info_t* out_info) {
  (void)user;
  (void)out_info;
  return H2_PAL_ERR_UNAVAILABLE;
}

static h2_pal_result_t touch_poll_event(void* user,
                                        h2_pal_touch_event_t* out_event) {
  (void)user;
  (void)out_event;
  return H2_PAL_ERR_UNAVAILABLE;
}

static h2_pal_result_t touch_close(void* user) {
  (void)user;
  return H2_PAL_ERR_UNAVAILABLE;
}

#endif

static const h2_pal_touch_vtable_t s_touch_vtable = {
    .open = touch_open,
    .get_info = touch_get_info,
    .poll_event = touch_poll_event,
    .close = touch_close,
};

static const h2_pal_touch_api_t s_touch_api = {
    .user = NULL,
    .vtable = &s_touch_vtable,
};

const h2_pal_touch_api_t* h2_bk7258_board_touch_api(void) {
  return &s_touch_api;
}
