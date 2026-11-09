#define _POSIX_C_SOURCE 200809L
#include "h2_bk7258_board_private.h"
#include "h2_bk7258_board_touch_test_sdk.h"

#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t critical = PTHREAD_MUTEX_INITIALIZER;
static gpio_isr_t interrupt_callback;
static bool irq_enabled, bus_live, task_live, no_sensor, read_complete_pending;
static bool interrupt_input, input_settled;
static int fail_bus, fail_sensor, fail_start, fail_join, fail_read, fail_enable;
static int fail_range, change_range_on_init, initialize_range_on_init;
static int status_reads, fail_status;
static uint16_t expected_range_register = 0x8048;
static uint16_t display_width = 480, display_height = 800;
static uint16_t controller_width = 480, controller_height = 800;
static int read_count, bus_closes, starts, joins;
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
  if (ms >= 50u && interrupt_input) input_settled = true;
  if (ms == 5u) {
    uint32_t flags = rtos_enter_critical();
    if (read_complete_pending) {
      ++read_count;
      read_complete_pending = false;
    }
    rtos_exit_critical(flags);
    pause_thread();
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
#define TRANSPORT16(name)                                             \
  int name(uint8_t addr, uint16_t reg, uint8_t* data, uint16_t len) { \
    (void)addr;                                                       \
    (void)reg;                                                        \
    (void)data;                                                       \
    (void)len;                                                        \
    return BK_OK;                                                     \
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
    int result = fail_status ? -1 : BK_OK;
    data[0] = next_sample.event == TP_EVENT_TYPE_NONE ? 0
              : next_sample.event == TP_EVENT_TYPE_UP ? 0x80
                                                      : 0x81;
    if (fail_status) {
      read_complete_pending = next_sample.event != TP_EVENT_TYPE_NONE;
      next_sample = (tp_data_t){0};
    }
    rtos_exit_critical(flags);
    return result;
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
TRANSPORT16(tp_i2c_write_uint16)

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
  (void)cb;
  assert(bus_live && max_num == TP_SUPPORT_MAX_NUM);
  uint32_t flags = rtos_enter_critical();
  ((tp_data_t*)data)[0] = next_sample;
  read_complete_pending = next_sample.event != TP_EVENT_TYPE_NONE;
  int result =
      fail_read || next_sample.event == TP_EVENT_TYPE_NONE ? -1 : BK_OK;
  next_sample = (tp_data_t){0};
  rtos_exit_critical(flags);
  return result;
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

static void emit(uint8_t kind, uint8_t track, uint16_t x, uint16_t y) {
  assert(irq_enabled && interrupt_callback != NULL);
  uint32_t flags = rtos_enter_critical();
  int before = read_count;
  next_sample = (tp_data_t){
      .event = kind, .track_id = track, .x_coordinate = x, .y_coordinate = y};
  rtos_exit_critical(flags);
  interrupt_callback(CONFIG_TP_INT_GPIO_ID);
  for (unsigned i = 0; i < 1000; ++i) {
    flags = rtos_enter_critical();
    bool done = read_count != before;
    rtos_exit_critical(flags);
    /* The worker's next delay acknowledges the entire read/publication batch.
     */
    if (done) return;
    pause_thread();
  }
  assert(!"touch worker did not read the panel");
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
  fail_enable = 0;

  assert(h2_pal_touch_open(touch) == H2_PAL_OK);
  fail_join = 1;
  int closes_before = bus_closes;
  assert(h2_pal_touch_close(touch) == H2_PAL_ERR_IO);
  assert(bus_live && bus_closes == closes_before);
  assert(h2_pal_touch_open(touch) == H2_PAL_ERR_BUSY);
  fail_join = 0;
  clean();

  assert(h2_pal_touch_open(touch) == H2_PAL_OK);
  fail_read = 1;
  emit(TP_EVENT_TYPE_DOWN, 0, 1, 2);
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_IO);
  clean();
  fail_read = 0;
  assert(h2_pal_touch_open(touch) == H2_PAL_OK);
  uint32_t status_flags = rtos_enter_critical();
  fail_status = 1;
  rtos_exit_critical(status_flags);
  emit(TP_EVENT_TYPE_DOWN, 0, 1, 2);
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_IO);
  clean();
  fail_status = 0;
  assert(h2_pal_touch_open(touch) == H2_PAL_OK);
  emit(TP_EVENT_TYPE_DOWN, 0, 480, 799);
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_IO);
  clean();

  assert(h2_pal_touch_open(touch) == H2_PAL_OK);
  for (unsigned i = 0; i < 17; ++i) {
    emit(TP_EVENT_TYPE_DOWN, 0, 10, 10);
    emit(TP_EVENT_TYPE_UP, 0, 10, 10);
  }
  assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_FULL);
  clean();
  assert(starts == joins);
  return 0;
}
