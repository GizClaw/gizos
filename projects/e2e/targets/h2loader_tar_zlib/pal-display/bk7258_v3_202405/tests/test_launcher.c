/* Execute the production launcher with PAL interfaces and only SDK boundaries
 * replaced. The two targets compile its Board capture and wire capture paths.
 */
#include "h2_bk7258_board.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_printf(const char* format, ...);
static int test_puts(const char* line);
#define printf test_printf
#define puts test_puts
#define H2_DISPLAY_VERSION "host-launcher-test"
#define main native_launcher_main
#include "ap_main_for_test.inc"
#undef main
#undef puts
#undef printf

typedef enum scenario {
  SUCCESS,
  POLICY_ERROR,
  ENTRY_ERROR,
  BOARD_ERROR,
  RUNTIME_ERROR,
  COMMAND_ERROR,
  WATCHDOG_ERROR,
  RUNNER_ERROR,
  INFO_ERROR,
  OPEN_ERROR,
  CLOSE_ERROR,
  BAD_GEOMETRY,
  BAD_FORMAT,
  ALLOCATION_ERROR,
  QUALIFICATION_ERROR,
  CAPTURE_ERROR,
  PIXEL_MISMATCH,
  BRIGHTNESS_MISMATCH,
  CONFIRM_ERROR,
  DEMO_ERROR,
  PATTERN_ERROR,
} scenario_t;

enum { WIDTH = 16, HEIGHT = 16, PIXELS = WIDTH * HEIGHT };
static jmp_buf terminal;
static struct {
  scenario_t scenario;
  h2_runtime_t runtime;
  void (*entry)(void*);
  h2_pal_task_entry_t runner;
  void (*watchdog)(void*);
  unsigned policy, init, entries, boards, runtimes, commands, tasks;
  unsigned allocator_calls, allocations, frees, captures, observations;
  unsigned qualifications, confirms, demos, patterns, replays, reboots;
  unsigned opens, infos, closes, waits, failed, ready, stable;
  int display_open;
  uint32_t brightness;
  int observation_rc, failure_rc, terminal_reason;
  char failure_stage[32];
  uint16_t expected[PIXELS], panel[PIXELS], scratch[PIXELS];
#ifdef H2_TEST_DISPLAY_TRANSFER
  test_transfer_fn transfer;
  void* transfer_user;
  unsigned registrations, detachments;
#endif
} f;
static const h2_pal_mem_api_t control_memory = {0};

static void capture_log(const char* line) {
  if (strstr(line, "H2_DISPLAY_OBSERVATION ") == line) {
    const char* rc = strstr(line, " rc=");
    assert(rc && sscanf(rc, " rc=%d", &f.observation_rc) == 1);
#ifdef H2_TEST_DISPLAY_TRANSFER
    assert(strstr(line, "source=completed-transfer"));
#else
    assert(strstr(line, "source=LCD-active-DMA"));
#endif
    ++f.observations;
  }
  if (strstr(line, "H2_DISPLAY_LAUNCHER_FAIL ") == line) {
    assert(sscanf(line, "H2_DISPLAY_LAUNCHER_FAIL stage=%31s rc=%d",
                  f.failure_stage, &f.failure_rc) == 2);
    ++f.failed;
  }
  if (strstr(line, "H2_DISPLAY_READY ") == line) ++f.ready;
  if (strstr(line, "H2_DISPLAY_STABLE ") == line) ++f.stable;
}
static int test_printf(const char* format, ...) {
  char line[1024];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  assert(length > 0 && (size_t)length < sizeof(line));
  capture_log(line);
  return length;
}
static int test_puts(const char* line) {
  capture_log(line);
  return 0;
}

static void* allocate(void* user, size_t size) {
  assert(user == &f && size == PIXELS * sizeof(uint16_t));
#ifndef H2_TEST_DISPLAY_TRANSFER
  assert(f.opens == 1 && f.closes == 1);
#endif
  ++f.allocations;
  return f.scenario == ALLOCATION_ERROR ? NULL : malloc(size);
}
static void release(void* user, void* pointer) {
  assert(user == &f && pointer == scanout);
#ifdef H2_TEST_DISPLAY_TRANSFER
  assert(!f.transfer);
#endif
  ++f.frees;
  free(pointer);
}
static const h2_pal_mem_vtable_t memory_vtable = {.alloc = allocate,
                                                  .free = release};
static const h2_pal_mem_api_t bitmap_allocator = {.user = &f,
                                                  .vtable = &memory_vtable};
const h2_pal_mem_api_t* h2_bk7258_board_display_allocator(void) {
  ++f.allocator_calls;
  return &bitmap_allocator;
}
static h2_display_info_t board_info(void) {
  h2_display_info_t info = {WIDTH, HEIGHT, H2_DISPLAY_PIXEL_RGB565};
  if (f.scenario == BAD_GEOMETRY) info.width = 4097;
  if (f.scenario == BAD_FORMAT) info.native_format = H2_DISPLAY_PIXEL_RGB888;
  return info;
}
static int get_info(void* user, h2_display_info_t* out) {
  assert(user == &f && out && f.display_open && f.opens == 1 && !f.closes);
  ++f.infos;
  *out = board_info();
  return f.scenario == INFO_ERROR ? H2_DISPLAY_ERR_IO : H2_DISPLAY_OK;
}
static int open_display(void* user) {
  assert(user == &f && !f.display_open);
  ++f.opens;
  if (f.scenario == OPEN_ERROR) return H2_DISPLAY_ERR_IO;
  f.display_open = 1;
  return H2_DISPLAY_OK;
}
static int close_display(void* user) {
  assert(user == &f && f.opens == 1 && f.display_open && f.infos == 1);
  ++f.closes;
  if (f.scenario == CLOSE_ERROR) return H2_DISPLAY_ERR_IO;
  f.display_open = 0;
  return H2_DISPLAY_OK;
}
static const h2_pal_display_vtable_t display_vtable = {
    .get_info = get_info, .open = open_display, .close = close_display};
static const h2_pal_display_api_t display_api = {.user = &f,
                                                 .vtable = &display_vtable};
int h2_bk7258_board_display_capture(uint16_t* out, size_t count) {
  assert(out == scanout && count == PIXELS);
  ++f.captures;
  if (f.scenario == CAPTURE_ERROR) return H2_DISPLAY_ERR_IO;
  memcpy(out, f.panel, sizeof(f.panel));
  return H2_DISPLAY_OK;
}
#ifdef H2_TEST_DISPLAY_TRANSFER
h2_display_info_t test_board_transfer_capture(test_transfer_fn callback,
                                              void* user) {
  f.transfer = callback;
  f.transfer_user = user;
  if (callback) {
    assert(scanout);
    ++f.registrations;
  } else {
    ++f.detachments;
  }
  return board_info();
}
uint32_t test_board_backlight_percent(void) { return f.brightness; }
/* Serialize independent panel pixels in the Board's MSB-first wire format. */
static void stripe(int y, int height) {
  uint8_t bytes[PIXELS * 2];
  assert(f.transfer && y >= 0 && height > 0 && y + height <= HEIGHT);
  for (int i = 0; i < WIDTH * height; ++i) {
    uint16_t pixel = f.panel[y * WIDTH + i];
    bytes[i * 2] = (uint8_t)(pixel >> 8);
    bytes[i * 2 + 1] = (uint8_t)pixel;
  }
  h2_display_rect_t rect = {0, y, WIDTH, height};
  f.transfer(f.transfer_user, &rect, bytes, (size_t)WIDTH * height * 2u);
  /* A callback cannot retain this temporary SDK transfer buffer. */
  memset(bytes, 0xa5, sizeof(bytes));
}
static void frame(void) {
  stripe(0, 5);
  stripe(5, 7);
  stripe(12, 4);
}
#endif

static h2_pal_result_t start_task(void* user,
                                  const h2_pal_task_options_t* options,
                                  h2_pal_task_entry_t callback, void* context,
                                  h2_pal_task_t** out) {
  assert(user == &f && f.commands == 1 && commands_ready);
  assert(options && !strcmp(options->name, "pal-display/e2e/runner"));
  assert(options->min_stack_size == 65536 && callback && !context && out);
  ++f.tasks;
  if (f.scenario == RUNNER_ERROR) return H2_PAL_ERR_TASK;
  f.runner = callback;
  *out = (h2_pal_task_t*)&f.tasks;
  return H2_PAL_OK;
}
static const h2_pal_task_vtable_t task_vtable = {.start = start_task};
static const h2_pal_task_api_t task_api = {.user = &f, .vtable = &task_vtable};
h2_pal_result_t h2_bk7258_board_runtime_config(h2_runtime_config_t* config) {
  ++f.boards;
  if (f.scenario == BOARD_ERROR) return H2_PAL_ERR_IO;
  *config = (h2_runtime_config_t){
      .mem = &control_memory, .display = &display_api, .task = &task_api};
  return H2_PAL_OK;
}
h2_pal_result_t h2_runtime_init(const h2_runtime_config_t* config,
                                h2_runtime_t** out) {
  assert(config->mem == &control_memory && config->display == &display_api);
  assert(config->system_event == h2_pal_unsupported_system_event_api());
  ++f.runtimes;
  *out = NULL;
  if (f.scenario == RUNTIME_ERROR) return H2_PAL_ERR_IO;
  f.runtime = (h2_runtime_t){
      .mem = config->mem, .display = config->display, .task = config->task};
  *out = &f.runtime;
  return H2_PAL_OK;
}
int h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
    h2_runtime_t* value, const char* name, uint32_t capabilities) {
  assert(value == &f.runtime && value->mem == &control_memory);
  assert(!strcmp(name, "pal-display") &&
         capabilities == H2_LOADER_CAPABILITY_UART);
  ++f.commands;
  return f.scenario == COMMAND_ERROR ? H2_PAL_ERR_IO : H2_PAL_OK;
}
int h2_bk_h2loader_confirm_current_app(h2_runtime_t* value) {
  assert(value == &f.runtime && commands_ready && finished);
  assert(f.qualifications == 1 && f.frees == 1 && !scanout);
  ++f.confirms;
  return f.scenario == CONFIRM_ERROR ? H2_PAL_ERR_IO : H2_PAL_OK;
}
static h2_pal_result_t reboot(void* user, uint32_t reason) {
  assert(user == &f && reason == 0 && !commands_ready);
  ++f.reboots;
  return H2_PAL_ERR_IO; /* Even a returning reboot must terminate in hold(). */
}
static const h2_pal_power_vtable_t power_vtable = {.reboot = reboot};
static const h2_pal_power_api_t power_api = {.user = &f,
                                             .vtable = &power_vtable};
const h2_pal_power_api_t* h2_bk_h2loader_power_api(void) { return &power_api; }
int h2_display_device_run(h2_runtime_t* value, const char* version,
                          h2_pal_display_e2e_observe_fn observer, void* user,
                          const h2_pal_mem_api_t* memory) {
  assert(value == &f.runtime && value->mem == &control_memory);
  assert(!strcmp(version, H2_DISPLAY_VERSION) && observer && !user);
  assert(memory == &bitmap_allocator && scanout && !finished && !f.confirms);
  assert(!f.display_open);
#ifndef H2_TEST_DISPLAY_TRANSFER
  assert(f.opens == 1 && f.infos == 1 && f.closes == 1);
#endif
  ++f.qualifications;
  if (f.scenario == QUALIFICATION_ERROR) return H2_DISPLAY_ERR_IO;
  if (f.scenario == PIXEL_MISMATCH) f.panel[PIXELS - 1] ^= 0x0100;
#ifdef H2_TEST_DISPLAY_TRANSFER
  assert(f.registrations == 1);
  if (f.scenario != CAPTURE_ERROR) frame();
#endif
  uint32_t expected_brightness = f.brightness;
  if (f.scenario == BRIGHTNESS_MISMATCH) ++f.brightness;
  int rc =
      observer(user, f.expected, WIDTH, HEIGHT, expected_brightness, "fixture");
  assert(f.observations == 1 && f.observation_rc == rc);
  return rc;
}
int h2_display_device_demo(h2_runtime_t* value) {
  assert(value == &f.runtime && f.confirms == 1);
  ++f.demos;
  return f.scenario == DEMO_ERROR ? H2_DISPLAY_ERR_IO : H2_DISPLAY_OK;
}
int h2_display_device_show_pattern(h2_runtime_t* value) {
  assert(value == &f.runtime && f.demos == 1);
  ++f.patterns;
  return f.scenario == PATTERN_ERROR ? H2_DISPLAY_ERR_IO : H2_DISPLAY_OK;
}
void h2_display_device_replay(h2_runtime_t* value) {
  assert(value == &f.runtime && f.stable == 1);
  ++f.replays;
}
void rtos_delay_milliseconds(uint32_t ms) {
  if (ms == 1000 || ms == 3000) {
    f.terminal_reason = ms == 1000 ? 1 : 2;
    longjmp(terminal, 1);
  }
  if (ms == 200)
    assert(!commands_ready && f.failed);
  else if (ms == 5000)
    ++f.waits;
  else
    assert(ms == 600000);
}
int rtos_core0_create_psram_thread(beken_thread_t* thread, unsigned priority,
                                   const char* name, void (*callback)(void*),
                                   unsigned stack, void* user) {
  assert(thread && priority == 7 && !strcmp(name, "display_deadline"));
  assert(callback && stack == 4096 && !user && commands_ready);
  if (f.scenario == WATCHDOG_ERROR) return -1;
  f.watchdog = callback;
  *thread = &f;
  return kNoErr;
}
int h2_bk_target_task_policy_install(void) {
  ++f.policy;
  return f.scenario == POLICY_ERROR ? H2_PAL_ERR_IO : H2_PAL_OK;
}
void bk_init(void) {
  assert(f.policy == 1);
  ++f.init;
}
h2_pal_result_t h2_bk7258_board_start_entry_task(const char* name,
                                                 void (*callback)(void*),
                                                 void* user) {
  assert(f.init == 1 && !strcmp(name, "bk/pal-display") && callback && !user);
  ++f.entries;
  if (f.scenario == ENTRY_ERROR) return H2_PAL_ERR_TASK;
  f.entry = callback;
  return H2_PAL_OK;
}
static void reset(scenario_t scenario) {
  memset(&f, 0, sizeof(f));
  f.scenario = scenario;
  f.brightness = 73;
  for (unsigned i = 0; i < PIXELS; ++i)
    f.expected[i] = f.panel[i] = (uint16_t)(0x1301u + i * 0x0301u);
  runtime = NULL;
  finished = commands_ready = 0;
  scanout = NULL;
  bitmap_memory = NULL;
  display_info = (h2_display_info_t){0};
#ifdef H2_TEST_DISPLAY_TRANSFER
  memset(&capture_state, 0, sizeof(capture_state));
#endif
}
static void drive(void (*callback)(void*), void* user) {
  f.terminal_reason = 0;
  if (!setjmp(terminal)) {
    callback(user);
    assert(0 && "launcher returned before a terminal boundary");
  }
  assert(f.terminal_reason);
}
static void boot(void* user) {
  (void)user;
  (void)native_launcher_main();
}
static void test_startup(void) {
  reset(POLICY_ERROR);
  assert(native_launcher_main() == -1 && f.policy == 1 && !f.init &&
         !f.entries);
  reset(ENTRY_ERROR);
  drive(boot, NULL);
  assert(f.reboots == 1 && !f.runner && !strcmp(f.failure_stage, "entry"));
}
static void test_launcher(scenario_t scenario, const char* failure) {
  reset(scenario);
  assert(native_launcher_main() == 0 && f.entry);
  drive(f.entry, NULL);
  if (!f.runner) {
    assert(f.failed == 1 && !strcmp(f.failure_stage, failure));
    assert(f.reboots == (scenario <= COMMAND_ERROR ? 1u : 0u));
    assert(!f.qualifications && !f.confirms && !f.allocations);
    return;
  }
  assert(f.watchdog && commands_ready && !f.reboots && !f.qualifications);
  drive(f.runner, NULL);
  assert(f.waits == 1 && !scanout && !f.reboots);
#ifndef H2_TEST_DISPLAY_TRANSFER
  if (scenario == INFO_ERROR) assert(f.closes == 1 && !f.display_open);
  if (scenario == OPEN_ERROR) assert(!f.infos && !f.closes);
#endif
  if (failure) {
    assert(f.failed == 1 && !strcmp(f.failure_stage, failure));
    assert(f.terminal_reason == 1 && !f.stable && !f.replays);
  } else {
    assert(!f.failed && f.terminal_reason == 2 && f.stable == 1 &&
           f.replays == 1);
  }
  unsigned qualified = scenario == SUCCESS || scenario == CONFIRM_ERROR ||
                       scenario == DEMO_ERROR || scenario == PATTERN_ERROR;
  assert(f.confirms == qualified);
  assert(f.frees == (f.allocations && scenario != ALLOCATION_ERROR ? 1u : 0u));
  assert(f.ready == (qualified && scenario != CONFIRM_ERROR ? 1u : 0u));
#ifdef H2_TEST_DISPLAY_TRANSFER
  if (f.qualifications) assert(f.registrations == 1 && f.detachments == 2);
#endif
}
static void prepare_observer(void) {
  reset(SUCCESS);
  display_info = board_info();
  scanout = f.scratch;
#ifdef H2_TEST_DISPLAY_TRANSFER
  (void)test_board_transfer_capture(capture, NULL);
#endif
}
static void check_observation(int expected) {
  unsigned previous = f.observations;
  int rc = observe(NULL, f.expected, WIDTH, HEIGHT, f.brightness, "pixels");
  assert(rc == expected && f.observations == previous + 1);
  assert(f.observation_rc == expected);
}
static void test_observation(void) {
  prepare_observer();
#ifdef H2_TEST_DISPLAY_TRANSFER
  /* A zero-filled or stale draw buffer is never completed-transfer evidence. */
  check_observation(H2_DISPLAY_ERR_IO);
  prepare_observer();
  stripe(0, HEIGHT - 1);
  check_observation(H2_DISPLAY_ERR_IO);
  prepare_observer();
  frame();
  check_observation(H2_DISPLAY_OK);
  check_observation(H2_DISPLAY_ERR_IO); /* No fresh completed frame. */
  prepare_observer();
  frame();
  check_observation(H2_DISPLAY_OK);
  stripe(0, 5); /* A fresh prefix cannot borrow the previous frame's tail. */
  check_observation(H2_DISPLAY_ERR_IO);
  prepare_observer();
  frame();
  check_observation(H2_DISPLAY_OK);
  frame();
  check_observation(H2_DISPLAY_OK); /* Consecutive full frames reset y. */
  prepare_observer();
  stripe(5, 7); /* Missing first stripe / out of order. */
  stripe(0, HEIGHT);
  check_observation(H2_DISPLAY_ERR_IO);
  prepare_observer();
  stripe(0, 5);
  stripe(0, 5); /* Duplicate transfer. */
  stripe(5, 11);
  check_observation(H2_DISPLAY_ERR_IO);
  prepare_observer();
  frame();
  ++f.brightness;
  int rc = observe(NULL, f.expected, WIDTH, HEIGHT, 73, "brightness");
  assert(rc == H2_DISPLAY_ERR_IO && f.observation_rc == rc);

  const h2_display_rect_t bad[] = {
      {1, 0, WIDTH, HEIGHT}, {0, -1, WIDTH, HEIGHT}, {0, 0, WIDTH - 1, HEIGHT},
      {0, 0, WIDTH, 0},      {0, HEIGHT, WIDTH, 1},  {0, 0, WIDTH, HEIGHT + 1}};
  const uint8_t bytes[PIXELS * 2] = {0};
  for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    prepare_observer();
    f.transfer(NULL, &bad[i], bytes, sizeof(bytes));
    check_observation(H2_DISPLAY_ERR_IO);
  }
  for (unsigned i = 0; i < 5; ++i) {
    prepare_observer();
    h2_display_rect_t rect = {0, 0, WIDTH, HEIGHT};
    if (i == 0) scanout = NULL;
    f.transfer(NULL, i == 1 ? NULL : &rect, i == 2 ? NULL : bytes,
               i == 3   ? sizeof(bytes) - 1
               : i == 4 ? sizeof(bytes) + 1
                        : sizeof(bytes));
    scanout = f.scratch;
    check_observation(H2_DISPLAY_ERR_IO);
  }
#else
  check_observation(H2_DISPLAY_OK);
  f.panel[PIXELS - 1] ^= 0x0100;
  check_observation(H2_DISPLAY_ERR_IO);
  assert(f.captures ==
         2); /* Capture anew for every case, including unchanged frames. */
  f.scenario = CAPTURE_ERROR;
  check_observation(H2_DISPLAY_ERR_IO);
#endif
  prepare_observer();
  f.panel[PIXELS - 1] ^= 0x0100;
#ifdef H2_TEST_DISPLAY_TRANSFER
  frame();
#endif
  check_observation(
      H2_DISPLAY_ERR_IO); /* The printed rc must include memcmp failure. */
  prepare_observer();
  assert(observe(NULL, f.expected, WIDTH - 1, HEIGHT, 73, "width") ==
         H2_DISPLAY_ERR_IO);
  assert(observe(NULL, f.expected, WIDTH, HEIGHT - 1, 73, "height") ==
         H2_DISPLAY_ERR_IO);
  assert(!f.captures);
  scanout = NULL;
}
int main(void) {
  test_startup();
  test_launcher(SUCCESS, NULL);
  test_launcher(BOARD_ERROR, "board");
  test_launcher(RUNTIME_ERROR, "runtime");
  test_launcher(COMMAND_ERROR, "commands");
  test_launcher(WATCHDOG_ERROR, "watchdog");
  test_launcher(RUNNER_ERROR, "runner");
#ifndef H2_TEST_DISPLAY_TRANSFER
  test_launcher(INFO_ERROR, "media-prepare");
  test_launcher(OPEN_ERROR, "media-prepare");
  test_launcher(CLOSE_ERROR, "media-prepare");
#endif
  test_launcher(BAD_GEOMETRY, "capture-info");
  test_launcher(BAD_FORMAT, "capture-info");
  test_launcher(ALLOCATION_ERROR, "capture-allocation");
  test_launcher(QUALIFICATION_ERROR, "qualification");
  test_launcher(CAPTURE_ERROR, "qualification");
  test_launcher(PIXEL_MISMATCH, "qualification");
#ifdef H2_TEST_DISPLAY_TRANSFER
  test_launcher(BRIGHTNESS_MISMATCH, "qualification");
#endif
  test_launcher(CONFIRM_ERROR, "confirm");
  test_launcher(DEMO_ERROR, "visual-demo");
  test_launcher(PATTERN_ERROR, "stable-pattern");
  test_observation();
  puts("BK Display launcher regression PASS");
  return 0;
}
