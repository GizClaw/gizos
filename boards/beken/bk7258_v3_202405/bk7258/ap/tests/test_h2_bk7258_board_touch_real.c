#define _POSIX_C_SOURCE 200809L
#include "h2_bk7258_board_private.h"
#include "h2_bk7258_board_touch_test_sdk.h"

#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t critical = PTHREAD_MUTEX_INITIALIZER;
static gpio_isr_t interrupt_callback;
static bool irq_enabled, bus_live, task_live, no_sensor;
static bool interrupt_input, input_settled;
static int fail_bus, fail_sensor, fail_start, fail_join, fail_enable;
static int fail_range, change_range_on_init, initialize_range_on_init;
static int status_reads, retry_delays;
static unsigned submitted, sampled, published;
/* Each scripted fault applies to one report. A stuck bus outlasts the IRQ. */
static enum fault {
  HEALTHY,
  PROBE_IO,
  READER_STATUS_IO,
  READER_IDLE,
  DATA_IO,
  ACK_IO,
  ACK_UNCERTAIN,
  BAD_COUNT,
  PROBE_SAYS_RELEASE,
  STUCK_BUS,
  WORKER_DIES
} fault;
static bool in_reader, fail_status, sdk_pressed;
static uint16_t expected_range_register = 0x8048;
static uint16_t display_width = 480, display_height = 800;
static uint16_t controller_width = 480, controller_height = 800;
static int bus_closes, starts, joins;
static tp_data_t next_sample;
static h2_pal_task_entry_t worker_entry;
static void* worker_context;
struct h2_pal_task {
  pthread_t thread;
};
static struct h2_pal_task task;
static const h2_pal_touch_api_t* touch;

uint32_t rtos_enter_critical(void) {
  assert(pthread_mutex_lock(&critical) == 0);
  return 0;
}
void rtos_exit_critical(uint32_t flags) {
  (void)flags;
  assert(pthread_mutex_unlock(&critical) == 0);
}
static void pause_thread(void) {
  struct timespec time = {.tv_sec = 0, .tv_nsec = 1000000};
  nanosleep(&time, NULL);
}
int rtos_delay_milliseconds(uint32_t ms) {
  /* Reset settling delays require no wall time in this hardware mock. */
  if (ms == 5u || ms == 50u) {
    uint32_t flags = rtos_enter_critical();
    published = sampled;
    if (ms == 50u) ++retry_delays;
    bool die = fault == WORKER_DIES && sampled == submitted;
    if (die) fault = HEALTHY;
    rtos_exit_critical(flags);
    pause_thread();
    if (die) return -1;
  } else if (ms >= 50u && interrupt_input) {
    input_settled = true;
  }
  return BK_OK;
}
int gpio_dev_unmap(gpio_id_t id) {
  (void)id;
  return BK_OK;
}
int bk_gpio_set_config(gpio_id_t id, const gpio_config_t* config) {
  if (id == CONFIG_TP_INT_GPIO_ID) {
    interrupt_input = config->io_mode == GPIO_INPUT_ENABLE;
    if (!interrupt_input) input_settled = false;
  }
  return BK_OK;
}
int bk_gpio_set_output_high(gpio_id_t id) {
  (void)id;
  return BK_OK;
}
int bk_gpio_set_output_low(gpio_id_t id) {
  (void)id;
  return BK_OK;
}
int bk_gpio_disable_interrupt(gpio_id_t id) {
  assert(id == CONFIG_TP_INT_GPIO_ID);
  irq_enabled = false;
  return BK_OK;
}
int bk_gpio_enable_interrupt(gpio_id_t id) {
  assert(id == CONFIG_TP_INT_GPIO_ID);
  if (fail_enable) return -1;
  assert(task_live && interrupt_callback != NULL);
  irq_enabled = true;
  return BK_OK;
}
int bk_gpio_clear_interrupt(gpio_id_t id) {
  (void)id;
  return BK_OK;
}
int bk_gpio_set_interrupt_type(gpio_id_t id, gpio_int_type_t type) {
  assert(id == CONFIG_TP_INT_GPIO_ID);
  assert(type == GPIO_INT_TYPE_FALLING_EDGE);
  return BK_OK;
}
int bk_gpio_register_isr(gpio_id_t id, gpio_isr_t isr) {
  assert(id == CONFIG_TP_INT_GPIO_ID);
  if (isr != NULL)
    assert(interrupt_callback == NULL);
  else
    assert(!irq_enabled);
  interrupt_callback = isr;
  return BK_OK;
}
int bk_i2c_init_v2(int id, const i2c_config_t* config) {
  assert(id == 2 && config->baud_rate == I2C_BAUD_RATE_100KHZ);
  if (fail_bus) return -1;
  assert(!bus_live);
  bus_live = true;
  return BK_OK;
}
int bk_i2c_deinit_v2(int id) {
  assert(id == 2 && bus_live && !task_live && !irq_enabled);
  bus_live = false;
  ++bus_closes;
  return BK_OK;
}
#define TRANSPORT8(name)                                             \
  int name(uint8_t addr, uint8_t reg, uint8_t* data, uint16_t len) { \
    (void)addr;                                                      \
    (void)reg;                                                       \
    (void)data;                                                      \
    (void)len;                                                       \
    return BK_OK;                                                    \
  }
TRANSPORT8(tp_i2c_read_uint8)
TRANSPORT8(tp_i2c_write_uint8)
int tp_i2c_read_uint16(uint8_t addr, uint16_t reg, uint8_t* data,
                       uint16_t len) {
  assert(bus_live && interrupt_input && input_settled);
  assert(addr == 0x14);
  if (reg == 0x814e) {
    assert(len == 1);
    uint32_t flags = rtos_enter_critical();
    ++status_reads;
    sampled = submitted;
    if (fault == STUCK_BUS) fail_status = true;
    bool failed = fail_status || (in_reader && fault == READER_STATUS_IO) ||
                  (!in_reader && fault == PROBE_IO);
    data[0] = next_sample.event == TP_EVENT_TYPE_NONE ? 0
              : next_sample.event == TP_EVENT_TYPE_UP ? 0x80
                                                      : 0x81;
    if (in_reader && fault == READER_IDLE) data[0] = 0;
    if (in_reader && fault == BAD_COUNT) data[0] = 0x82;
    if (!in_reader && fault == PROBE_SAYS_RELEASE) data[0] = 0x80;
    if (failed && !in_reader) fault = HEALTHY;
    rtos_exit_critical(flags);
    return failed ? -1 : BK_OK;
  }
  if (reg == 0x814f) {
    assert(len == 8u);
    uint32_t flags = rtos_enter_critical();
    memset(data, 0, len);
    data[0] = next_sample.track_id;
    data[1] = (uint8_t)next_sample.x_coordinate;
    data[2] = (uint8_t)(next_sample.x_coordinate >> 8);
    data[3] = (uint8_t)next_sample.y_coordinate;
    data[4] = (uint8_t)(next_sample.y_coordinate >> 8);
    bool failed = fault == DATA_IO;
    rtos_exit_critical(flags);
    return failed ? -1 : BK_OK;
  }
  assert(len == 6 && reg == expected_range_register);
  if (fail_range) return -1;
  data[0] = (uint8_t)controller_width;
  data[1] = (uint8_t)(controller_width >> 8);
  data[2] = (uint8_t)controller_height;
  data[3] = (uint8_t)(controller_height >> 8);
  data[4] = 1;
  data[5] = 1;
  return BK_OK;
}
int tp_i2c_write_uint16(uint8_t addr, uint16_t reg, uint8_t* data,
                        uint16_t len) {
  assert(in_reader && addr == 0x14 && reg == 0x814e && len == 1 && *data == 0);
  uint32_t flags = rtos_enter_critical();
  bool failed = fault == ACK_IO || fault == ACK_UNCERTAIN;
  if (fault != ACK_IO) next_sample = (tp_data_t){0};
  if (fault != WORKER_DIES) fault = HEALTHY;
  rtos_exit_critical(flags);
  return failed ? -1 : BK_OK;
}

static int sensor_init(const tp_i2c_callback_t* cb,
                       tp_sensor_user_config_t* config) {
  assert(bus_live && interrupt_input && input_settled);
  assert(cb->read_uint16 == tp_i2c_read_uint16);
  if (initialize_range_on_init) {
    assert(config->x_size == 800 && config->y_size == 480);
    controller_width = 400;
    controller_height = 240;
  } else {
    assert(config->x_size == controller_width &&
           config->y_size == controller_height);
  }
  assert(config->tp_num == 1);
  if (change_range_on_init) {
    controller_width = change_range_on_init == 2 ? 0 : 400;
    controller_height = change_range_on_init == 2 ? 0 : 240;
  }
  return fail_sensor ? -1 : BK_OK;
}
static int sensor_read(const tp_i2c_callback_t* cb, uint8_t max_num,
                       uint8_t* data) {
  /* Mirror the pinned reader's second status read, tracking before ACK, and
   * ACK even after a read error. Point data and status use separate reads. */
  assert(bus_live && max_num == TP_SUPPORT_MAX_NUM);
  uint8_t status = 0, point[8] = {0}, ack = 0;
  in_reader = true;
  int rc = cb->read_uint16(0x14, 0x814e, &status, 1u);
  if (rc == BK_OK && !(status & 0x80u)) rc = -1;
  if (rc == BK_OK) rc = cb->read_uint16(0x14, 0x814f, point, sizeof(point));
  if (rc == BK_OK) {
    uint32_t flags = rtos_enter_critical();
    tp_data_t sample = next_sample;
    if (sample.event == TP_EVENT_TYPE_UP && !sdk_pressed)
      sample.event = TP_EVENT_TYPE_NONE;
    sdk_pressed = (status & 0x0fu) != 0u;
    ((tp_data_t*)data)[0] = sample;
    rtos_exit_critical(flags);
  }
  if (cb->write_uint16(0x14, 0x814e, &ack, 1u) != BK_OK) rc = -1;
  in_reader = false;
  return rc;
}
static tp_sensor_config_t sensor = {
    .name = "gt911",
    .id = TP_ID_GT911,
    .address = 0x14,
    .def_int_type = TP_INT_TYPE_FALLING_EDGE,
    .def_refresh_rate = 20,
    .init = sensor_init,
    .read_tp_info = sensor_read,
};
void tp_sensor_devices_init(void) { assert(bus_live); }
const tp_sensor_config_t* tp_get_sensor_auto_detect(
    const tp_i2c_callback_t* cb) {
  assert(interrupt_input && input_settled);
  assert(cb->write_uint8 == tp_i2c_write_uint8);
  return no_sensor ? NULL : &sensor;
}
static void* run_worker(void* ctx) {
  worker_entry(ctx);
  return NULL;
}
static int start_task(void* user, const h2_pal_task_options_t* options,
                      h2_pal_task_entry_t entry, void* ctx,
                      h2_pal_task_t** out) {
  (void)user;
  assert(strcmp(options->name, "h2_touch") == 0);
  if (fail_start) return H2_PAL_ERR_TASK;
  worker_entry = entry;
  worker_context = ctx;
  task_live = true;
  ++starts;
  assert(pthread_create(&task.thread, NULL, run_worker, worker_context) == 0);
  *out = &task;
  return H2_PAL_OK;
}
static int join_task(void* user, h2_pal_task_t* handle) {
  (void)user;
  assert(handle == &task && task_live && !irq_enabled);
  assert(interrupt_callback == NULL);
  if (fail_join) return H2_PAL_ERR_IO;
  assert(pthread_join(task.thread, NULL) == 0);
  task_live = false;
  ++joins;
  return H2_PAL_OK;
}
const h2_pal_task_api_t* h2_bk7258_board_task_api(void) {
  static const h2_pal_task_vtable_t vtable = {.start = start_task,
                                              .join = join_task};
  static const h2_pal_task_api_t api = {.vtable = &vtable};
  return &api;
}
static int display_info(void* user, h2_display_info_t* info) {
  (void)user;
  *info = (h2_display_info_t){.width = display_width, .height = display_height};
  return H2_PAL_OK;
}
h2_pal_display_t* h2_bk7258_board_display(void) {
  static const h2_pal_display_vtable_t vtable = {.get_info = display_info};
  static h2_pal_display_t api = {.vtable = &vtable};
  return &api;
}

static void emit_with_fault(enum fault failure, uint8_t kind, uint8_t track,
                            uint16_t x, uint16_t y) {
  assert(irq_enabled && interrupt_callback != NULL);
  uint32_t flags = rtos_enter_critical();
  unsigned sequence = ++submitted;
  fault = failure;
  next_sample = (tp_data_t){
      .event = kind, .track_id = track, .x_coordinate = x, .y_coordinate = y};
  rtos_exit_critical(flags);
  interrupt_callback(CONFIG_TP_INT_GPIO_ID);
  for (unsigned i = 0; i < 1000; ++i) {
    flags = rtos_enter_critical();
    bool done = published == sequence;
    rtos_exit_critical(flags);
    /* The worker's next delay acknowledges the entire read/publication batch.
     */
    if (done) return;
    pause_thread();
  }
  assert(!"touch worker did not read the panel");
}
static void emit(uint8_t kind, uint8_t track, uint16_t x, uint16_t y) {
  emit_with_fault(HEALTHY, kind, track, x, y);
}
static void expect(h2_pal_touch_event_kind_t kind, int x, int y) {
  h2_pal_touch_event_t event;
  h2_pal_result_t result = H2_PAL_ERR_WOULD_BLOCK;
  for (unsigned i = 0; i < 1000 && result == H2_PAL_ERR_WOULD_BLOCK; ++i) {
    result = h2_pal_touch_poll_event(touch, &event);
    if (result == H2_PAL_ERR_WOULD_BLOCK) pause_thread();
  }
  assert(result == H2_PAL_OK);
  assert(event.kind == kind && event.x == x && event.y == y);
}
static void clean(void) {
  assert(h2_pal_touch_close(touch) == H2_PAL_OK);
  assert(!bus_live && !task_live && interrupt_callback == NULL);
}
static void coordinate_range_tests(void) {
  const struct {
    uint16_t width, height, source_width, source_height;
    uint8_t id;
  } cases[] = {
      {480, 800, 480, 800, TP_ID_GT911},     {800, 480, 400, 240, TP_ID_GT1151},
      {800, 480, 480, 480, TP_ID_GT911},     {480, 800, 800, 480, TP_ID_GT1151},
      {800, 480, 65535, 65535, TP_ID_GT911}, {1, 1, 1, 1, TP_ID_GT911},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    display_width = cases[i].width;
    display_height = cases[i].height;
    controller_width = cases[i].source_width;
    controller_height = cases[i].source_height;
    sensor.id = cases[i].id;
    sensor.name = sensor.id == TP_ID_GT911 ? "gt911" : "gt1151";
    expected_range_register = sensor.id == TP_ID_GT911 ? 0x8048 : 0x8051;
    assert(h2_pal_touch_open(touch) == H2_PAL_OK);
    h2_pal_touch_info_t info;
    assert(h2_pal_touch_get_info(touch, &info) == H2_PAL_OK);
    assert(info.width == display_width && info.height == display_height);
    for (unsigned corner = 0; corner < 4; ++corner) {
      uint16_t x = corner & 1u ? controller_width - 1u : 0u;
      uint16_t y = corner & 2u ? controller_height - 1u : 0u;
      int expected_x = corner & 1u ? display_width - 1u : 0u;
      int expected_y = corner & 2u ? display_height - 1u : 0u;
      emit(TP_EVENT_TYPE_DOWN, 0, x, y);
      emit(TP_EVENT_TYPE_UP, 0, x, y);
      expect(H2_PAL_TOUCH_EVENT_DOWN, expected_x, expected_y);
      expect(H2_PAL_TOUCH_EVENT_UP, expected_x, expected_y);
    }
    if (i == 1) {
      /* Ignored SDK size requests must not compress an upper-right target. */
      emit(TP_EVENT_TYPE_DOWN, 0, 300, 60);
      emit(TP_EVENT_TYPE_UP, 0, 300, 60);
      expect(H2_PAL_TOUCH_EVENT_DOWN, 600, 120);
      expect(H2_PAL_TOUCH_EVENT_UP, 600, 120);
    }
    if (i == 2) {
      /* Actual DevKit geometry and observed upper-right samples. */
      emit(TP_EVENT_TYPE_DOWN, 0, 373, 135);
      emit(TP_EVENT_TYPE_UP, 0, 373, 135);
      expect(H2_PAL_TOUCH_EVENT_DOWN, 622, 135);
      expect(H2_PAL_TOUCH_EVENT_UP, 622, 135);
      emit(TP_EVENT_TYPE_DOWN, 0, 364, 147);
      emit(TP_EVENT_TYPE_UP, 0, 364, 147);
      expect(H2_PAL_TOUCH_EVENT_DOWN, 607, 147);
      expect(H2_PAL_TOUCH_EVENT_UP, 607, 147);
    }
    clean();
  }
  sensor.id = TP_ID_GT1151;
  sensor.name = "gt1151";
  expected_range_register = 0x8051;
  display_width = controller_width = 800;
  display_height = controller_height = 480;
  change_range_on_init = 1;
  assert(h2_pal_touch_open(touch) == H2_PAL_OK);
  emit(TP_EVENT_TYPE_DOWN, 0, 300, 60);
  emit(TP_EVENT_TYPE_UP, 0, 300, 60);
  expect(H2_PAL_TOUCH_EVENT_DOWN, 600, 120);
  expect(H2_PAL_TOUCH_EVENT_UP, 600, 120);
  clean();
  change_range_on_init = 0;
  controller_width = controller_height = 0;
  sensor.def_ppi = (800u << 16) | 480u;
  initialize_range_on_init = 1;
  assert(h2_pal_touch_open(touch) == H2_PAL_OK);
  emit(TP_EVENT_TYPE_DOWN, 0, 300, 60);
  emit(TP_EVENT_TYPE_UP, 0, 300, 60);
  expect(H2_PAL_TOUCH_EVENT_DOWN, 600, 120);
  expect(H2_PAL_TOUCH_EVENT_UP, 600, 120);
  clean();
  initialize_range_on_init = 0;
  sensor.def_ppi = 0;
  controller_width = 800;
  controller_height = 480;
  change_range_on_init = 2;
  assert(h2_pal_touch_open(touch) == H2_PAL_ERR_FORMAT);
  assert(!bus_live && !task_live);
  change_range_on_init = 0;
  sensor.id = TP_ID_GT911;
  sensor.name = "gt911";
  expected_range_register = 0x8048;
  display_width = controller_width = 480;
  display_height = controller_height = 800;
  fail_range = 1;
  assert(h2_pal_touch_open(touch) == H2_PAL_ERR_IO);
  assert(!bus_live && !task_live);
  fail_range = 0;
  controller_width = 0;
  assert(h2_pal_touch_open(touch) == H2_PAL_ERR_FORMAT);
  assert(!bus_live && !task_live);
  controller_width = 480;
}

static void expect_result(h2_pal_result_t result) {
  h2_pal_touch_event_t event;
  assert(h2_pal_touch_poll_event(touch, &event) == result);
}

static void fresh_tap(void) {
  emit(TP_EVENT_TYPE_DOWN, 0, 60, 70);
  emit(TP_EVENT_TYPE_UP, 0, 60, 70);
  expect(H2_PAL_TOUCH_EVENT_DOWN, 60, 70);
  expect(H2_PAL_TOUCH_EVENT_UP, 60, 70);
  expect_result(H2_PAL_ERR_WOULD_BLOCK);
}

static void report_recovery_tests(void) {
  const enum fault failures[] = {PROBE_IO,  READER_STATUS_IO, READER_IDLE,
                                 DATA_IO,   ACK_IO,           ACK_UNCERTAIN,
                                 BAD_COUNT, HEALTHY /* out of bounds */};
  for (size_t i = 0u; i < sizeof(failures) / sizeof(failures[0]); ++i) {
    assert(h2_pal_touch_open(touch) == H2_PAL_OK);
    int started = starts;
    emit(TP_EVENT_TYPE_DOWN, 0, 20, 30);
    expect(H2_PAL_TOUCH_EVENT_DOWN, 20, 30);
    emit(TP_EVENT_TYPE_MOVE, 0, 21, 31); /* Undelivered at time of fault. */
    emit_with_fault(failures[i], TP_EVENT_TYPE_MOVE, 0,
                    failures[i] == HEALTHY ? controller_width : 22, 32);
    /* The worker can keep sampling before the consumer sees the error. An
     * idle bus, and even a zero count in just the outer probe, are ambiguous.
     */
    emit(TP_EVENT_TYPE_NONE, 0, 0, 0);
    emit_with_fault(PROBE_SAYS_RELEASE, TP_EVENT_TYPE_MOVE, 0, 60, 70);
    expect_result(H2_PAL_ERR_IO);
    for (int poll = 0; poll < 64; ++poll) expect_result(H2_PAL_ERR_WOULD_BLOCK);
    emit(TP_EVENT_TYPE_MOVE, 0, 61, 71);
    emit(TP_EVENT_TYPE_UP, 0, 61, 71);
    expect_result(H2_PAL_ERR_WOULD_BLOCK);
    fresh_tap();
    assert(starts == started);
    clean();
  }

  /* The first DOWN can fail before SDK tracking. Its actual ready release
   * has no SDK UP, yet the very next tap belongs to a fresh gesture. */
  assert(h2_pal_touch_open(touch) == H2_PAL_OK);
  emit_with_fault(DATA_IO, TP_EVENT_TYPE_DOWN, 0, 20, 30);
  emit(TP_EVENT_TYPE_UP, 0, 20, 30);
  emit(TP_EVENT_TYPE_DOWN, 0, 40, 50);
  emit(TP_EVENT_TYPE_UP, 0, 40, 50);
  expect_result(H2_PAL_ERR_IO); /* Cancellation precedes fresh queued edges. */
  expect(H2_PAL_TOUCH_EVENT_DOWN, 40, 50);
  expect(H2_PAL_TOUCH_EVENT_UP, 40, 50);

  /* The SDK advanced its tracking before a release ACK whose result is
   * uncertain. Never re-arm on idle or on a held contact's next DOWN. */
  emit(TP_EVENT_TYPE_DOWN, 0, 40, 50);
  expect(H2_PAL_TOUCH_EVENT_DOWN, 40, 50);
  emit_with_fault(ACK_UNCERTAIN, TP_EVENT_TYPE_UP, 0, 40, 50);
  expect_result(H2_PAL_ERR_IO);
  emit(TP_EVENT_TYPE_NONE, 0, 0, 0);
  emit(TP_EVENT_TYPE_DOWN, 0, 60, 70);
  emit(TP_EVENT_TYPE_UP, 0, 60, 70);
  expect_result(H2_PAL_ERR_WOULD_BLOCK);
  fresh_tap();

  /* A failed release ACK can also leave ready asserted. The retry must read
   * that release without another IRQ, even though SDK tracking is now up. */
  emit(TP_EVENT_TYPE_DOWN, 0, 40, 50);
  expect(H2_PAL_TOUCH_EVENT_DOWN, 40, 50);
  emit_with_fault(ACK_IO, TP_EVENT_TYPE_UP, 0, 40, 50);
  bool acknowledged = false;
  for (unsigned i = 0; i < 1000 && !acknowledged; ++i) {
    uint32_t flags = rtos_enter_critical();
    acknowledged = next_sample.event == TP_EVENT_TYPE_NONE;
    rtos_exit_critical(flags);
    if (!acknowledged) pause_thread();
  }
  assert(acknowledged);
  expect_result(H2_PAL_ERR_IO);
  fresh_tap();
  clean();
}

static void persistent_fault_tests(void) {
  int delays_before = retry_delays;
  assert(h2_pal_touch_open(touch) == H2_PAL_OK);
  emit_with_fault(STUCK_BUS, TP_EVENT_TYPE_DOWN, 0, 20, 30);
  /* Each failed worker retry is observable even with INT held asserted. */
  for (int error = 0; error < 3; ++error) {
    h2_pal_touch_event_t event;
    h2_pal_result_t rc = H2_PAL_ERR_WOULD_BLOCK;
    for (unsigned i = 0; i < 1000 && rc == H2_PAL_ERR_WOULD_BLOCK; ++i) {
      rc = h2_pal_touch_poll_event(touch, &event);
      if (rc == H2_PAL_ERR_WOULD_BLOCK) pause_thread();
    }
    assert(rc == H2_PAL_ERR_IO);
  }
  uint32_t flags = rtos_enter_critical();
  assert(retry_delays - delays_before >= 2);
  fail_status = false;
  /* Release is already ready on the controller. No further IRQ arrives. */
  unsigned release = ++submitted;
  next_sample = (tp_data_t){.event = TP_EVENT_TYPE_UP};
  rtos_exit_critical(flags);
  bool processed = false;
  for (unsigned i = 0; i < 1000 && !processed; ++i) {
    flags = rtos_enter_critical();
    processed = published == release;
    rtos_exit_critical(flags);
    if (!processed) pause_thread();
  }
  assert(processed);
  h2_pal_touch_event_t event;
  /* A final real failure may have raced the poll above. */
  int last = h2_pal_touch_poll_event(touch, &event);
  assert(last == H2_PAL_ERR_WOULD_BLOCK || last == H2_PAL_ERR_IO);
  expect_result(H2_PAL_ERR_WOULD_BLOCK);
  fresh_tap();
  clean();

  assert(h2_pal_touch_open(touch) == H2_PAL_OK);
  emit_with_fault(WORKER_DIES, TP_EVENT_TYPE_NONE, 0, 0, 0);
  /* A dead acquisition worker remains terminal until shutdown. */
  for (unsigned i = 0; i < 1000u; ++i) {
    if (h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_IO) break;
    pause_thread();
  }
  expect_result(H2_PAL_ERR_IO);
  expect_result(H2_PAL_ERR_IO);
  clean();
}

int main(void) {
  touch = h2_bk7258_board_touch_api();
  h2_pal_touch_info_t info;
  h2_pal_touch_event_t event;
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_touch_get_info(touch, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_touch_open(touch) == H2_PAL_OK);
  assert(h2_pal_touch_open(touch) == H2_PAL_ERR_BUSY);
  assert(h2_pal_touch_get_info(touch, &info) == H2_PAL_OK);
  assert(info.width == 480 && info.height == 800);
  bool initial_status_read = false;
  for (unsigned i = 0; i < 1000 && !initial_status_read; ++i) {
    uint32_t flags = rtos_enter_critical();
    initial_status_read = status_reads != 0;
    rtos_exit_critical(flags);
    if (!initial_status_read) pause_thread();
  }
  assert(initial_status_read);
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_WOULD_BLOCK);
  emit(TP_EVENT_TYPE_UP, 0, 1, 2); /* no fabricated release */
  emit(TP_EVENT_TYPE_DOWN, 0, 20, 30);
  emit(TP_EVENT_TYPE_MOVE, 1, 400, 600); /* second pointer ignored */
  emit(TP_EVENT_TYPE_MOVE, 0, 21, 31);
  emit(TP_EVENT_TYPE_MOVE, 0, 22, 32); /* motion coalesces */
  emit(TP_EVENT_TYPE_UP, 0, 22, 32);
  expect(H2_PAL_TOUCH_EVENT_DOWN, 20, 30);
  expect(H2_PAL_TOUCH_EVENT_MOVE, 22, 32);
  expect(H2_PAL_TOUCH_EVENT_UP, 22, 32);
  clean();
  assert(starts == joins);
  coordinate_range_tests();
  /* The initial controller status must be consumed without a new IRQ edge. */
  next_sample = (tp_data_t){.event = TP_EVENT_TYPE_DOWN,
                            .track_id = 0,
                            .x_coordinate = 20,
                            .y_coordinate = 30};
  assert(h2_pal_touch_open(touch) == H2_PAL_OK);
  expect(H2_PAL_TOUCH_EVENT_DOWN, 20, 30);
  emit(TP_EVENT_TYPE_UP, 0, 20, 30);
  expect(H2_PAL_TOUCH_EVENT_UP, 20, 30);
  clean();

  no_sensor = true;
  assert(h2_pal_touch_open(touch) == H2_PAL_ERR_NOT_FOUND);
  assert(!bus_live && !task_live);
  no_sensor = false;
  fail_bus = 1;
  assert(h2_pal_touch_open(touch) == H2_PAL_ERR_IO);
  fail_bus = 0;
  fail_sensor = 1;
  assert(h2_pal_touch_open(touch) == H2_PAL_ERR_IO);
  fail_sensor = 0;
  fail_start = 1;
  assert(h2_pal_touch_open(touch) == H2_PAL_ERR_TASK);
  assert(!bus_live && interrupt_callback == NULL);
  fail_start = 0;
  fail_enable = 1;
  assert(h2_pal_touch_open(touch) == H2_PAL_ERR_IO);
  assert(!bus_live && !task_live && interrupt_callback == NULL);
  fail_join = 1; /* Partial open can return with its reader still owned. */
  assert(h2_pal_touch_open(touch) == H2_PAL_ERR_IO);
  assert(bus_live && task_live && !irq_enabled && interrupt_callback == NULL);
  assert(h2_pal_touch_open(touch) == H2_PAL_ERR_BUSY);
  fail_enable = fail_join = 0;
  clean();

  assert(h2_pal_touch_open(touch) == H2_PAL_OK);
  fail_join = 1;
  int closes_before = bus_closes;
  assert(h2_pal_touch_close(touch) == H2_PAL_ERR_IO);
  assert(bus_live && bus_closes == closes_before);
  assert(h2_pal_touch_open(touch) == H2_PAL_ERR_BUSY);
  fail_join = 0;
  clean();

  report_recovery_tests();
  persistent_fault_tests();

  assert(h2_pal_touch_open(touch) == H2_PAL_OK);
  for (unsigned i = 0; i < 17; ++i) {
    emit(TP_EVENT_TYPE_DOWN, 0, 10, 10);
    emit(TP_EVENT_TYPE_UP, 0, 10, 10);
  }
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_FULL);
  emit_with_fault(READER_STATUS_IO, TP_EVENT_TYPE_DOWN, 0, 10, 10);
  expect_result(H2_PAL_ERR_FULL);
  clean();

  sensor.id = TP_ID_GT1151;
  sensor.name = "gt1151";
  expected_range_register = 0x8051;
  assert(h2_pal_touch_open(touch) == H2_PAL_OK);
  emit_with_fault(DATA_IO, TP_EVENT_TYPE_DOWN, 0, 10, 10);
  expect_result(H2_PAL_ERR_IO);
  emit(TP_EVENT_TYPE_UP, 0, 10, 10);
  expect_result(H2_PAL_ERR_IO);
  clean();
  assert(starts == joins);
  return 0;
}
