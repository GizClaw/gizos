#include "h2_g99_loader_controller.h"

#include "h2_desktop_platform.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>

typedef struct display_fixture {
  pthread_t ui_thread;
  bool thread_set;
  bool wrong_thread;
  bool fail_present;
  bool opened;
  unsigned int frames;
} display_fixture_t;

static display_fixture_t display_fixture;
static bool fail_join;
static const h2_pal_task_api_t* base_tasks;
static const h2_pal_queue_api_t* base_queues;
static h2_pal_queue_t* response_queue;
static bool fail_response;

static int queue_create(void* user, const h2_pal_queue_config_t* config,
                        h2_pal_queue_t** out_queue) {
  (void)user;
  const int rc = h2_pal_queue_create(base_queues, config, out_queue);
  if (rc == H2_PAL_OK && strcmp(config->name, "g99-loader-rendered") == 0)
    response_queue = *out_queue;
  return rc;
}

static void queue_destroy(void* user, h2_pal_queue_t* queue) {
  (void)user;
  h2_pal_queue_destroy(base_queues, queue);
}

static int queue_send(void* user, h2_pal_queue_t* queue, const void* item,
                      uint32_t timeout_ms) {
  (void)user;
  if (queue == response_queue && fail_response) return H2_PAL_ERR_IO;
  return h2_pal_queue_send(base_queues, queue, item, timeout_ms);
}

static int queue_recv(void* user, h2_pal_queue_t* queue, void* out_item,
                      uint32_t timeout_ms) {
  (void)user;
  return h2_pal_queue_recv(base_queues, queue, out_item, timeout_ms);
}

static int queue_close(void* user, h2_pal_queue_t* queue) {
  (void)user;
  return h2_pal_queue_close(base_queues, queue);
}

static void record_thread(void) {
  if (!display_fixture.thread_set) {
    display_fixture.ui_thread = pthread_self();
    display_fixture.thread_set = true;
  } else if (!pthread_equal(display_fixture.ui_thread, pthread_self())) {
    display_fixture.wrong_thread = true;
  }
}

static int display_open(void* user) {
  (void)user;
  record_thread();
  display_fixture.opened = true;
  return H2_PAL_OK;
}

static int display_info(void* user, h2_display_info_t* info) {
  (void)user;
  record_thread();
  memset(info, 0, sizeof(*info));
  info->width = 320u;
  info->height = 240u;
  return H2_PAL_OK;
}

static int display_draw(void* user, const h2_display_rect_t* rect,
                        const void* pixels, size_t stride,
                        h2_display_pixel_format_t format) {
  (void)user;
  (void)rect;
  (void)pixels;
  (void)stride;
  (void)format;
  record_thread();
  return H2_PAL_OK;
}

static int display_present(void* user) {
  (void)user;
  record_thread();
  ++display_fixture.frames;
  return display_fixture.fail_present ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static int display_close(void* user) {
  (void)user;
  record_thread();
  display_fixture.opened = false;
  return H2_PAL_OK;
}

static int task_start(void* user, const h2_pal_task_options_t* options,
                      h2_pal_task_entry_t entry, void* context,
                      h2_pal_task_t** out_task) {
  (void)user;
  return h2_pal_task_start(base_tasks, options, entry, context, out_task);
}

static int task_join(void* user, h2_pal_task_t* task) {
  (void)user;
  if (fail_join) {
    fail_join = false;
    return H2_PAL_ERR_TASK;
  }
  return h2_pal_task_join(base_tasks, task);
}

typedef struct submitter {
  h2_g99_loader_controller_t* controller;
  h2_pal_result_t result;
} submitter_t;

static void* send_progress(void* user) {
  submitter_t* submitter = user;
  submitter->result = H2_PAL_OK;
  for (unsigned int completed = 1u; completed <= 4u; ++completed) {
    const h2_pal_result_t rc = h2_g99_loader_controller_progress(
        submitter->controller, H2_LOADER_INSTALL_PHASE_IMAGE, completed, 4u);
    if (rc != H2_PAL_OK) submitter->result = rc;
  }
  return NULL;
}

int main(void) {
  const h2_pal_display_vtable_t display_vtable = {.open = display_open,
                                                  .get_info = display_info,
                                                  .draw_bitmap = display_draw,
                                                  .present = display_present,
                                                  .close = display_close};
  const h2_pal_display_api_t display = {.vtable = &display_vtable};
  base_tasks = h2_desktop_platform_task_api();
  const h2_pal_task_vtable_t task_vtable = {.start = task_start,
                                            .join = task_join};
  const h2_pal_task_api_t tasks = {.vtable = &task_vtable};
  base_queues = h2_desktop_platform_queue_api();
  const h2_pal_queue_vtable_t queue_vtable = {.create = queue_create,
                                              .destroy = queue_destroy,
                                              .send = queue_send,
                                              .recv = queue_recv,
                                              .close = queue_close};
  const h2_pal_queue_api_t queues = {.vtable = &queue_vtable};
  h2_runtime_t runtime = {.mem = h2_desktop_platform_default_allocator(),
                          .time = h2_desktop_platform_time_api(),
                          .task = &tasks,
                          .sync = h2_desktop_platform_sync_api(),
                          .queue = &queues,
                          .display = &display};
  h2_g99_loader_controller_t* controller = NULL;
  assert(h2_g99_loader_controller_open(&runtime, &controller) == H2_PAL_OK);
  assert(display_fixture.opened && display_fixture.frames > 0u);
  assert(!pthread_equal(display_fixture.ui_thread, pthread_self()));
  assert(h2_g99_loader_controller_installing(controller) == H2_PAL_OK);
  submitter_t submitters[3];
  pthread_t threads[3];
  for (size_t index = 0u; index < 3u; ++index) {
    submitters[index].controller = controller;
    assert(pthread_create(&threads[index], NULL, send_progress,
                          &submitters[index]) == 0);
  }
  for (size_t index = 0u; index < 3u; ++index) {
    assert(pthread_join(threads[index], NULL) == 0);
    assert(submitters[index].result == H2_PAL_OK);
  }
  assert(h2_g99_loader_controller_failed(controller, -123) == H2_PAL_OK);
  h2_g99_loader_state_t state;
  assert(h2_g99_loader_controller_state(controller, &state) == H2_PAL_OK);
  assert(state.screen == H2_G99_LOADER_SCREEN_FAILED && state.code == -123);
  const unsigned int before_launch = display_fixture.frames;
  assert(h2_g99_loader_controller_launching(controller) == H2_PAL_OK);
  assert(display_fixture.frames > before_launch);
  display_fixture.fail_present = true;
  assert(h2_g99_loader_controller_installing(controller) == H2_PAL_ERR_IO);
  display_fixture.fail_present = false;
  assert(h2_g99_loader_controller_installing(controller) == H2_PAL_OK);
  fail_join = true;
  assert(h2_g99_loader_controller_close(&controller) == H2_PAL_ERR_TASK);
  assert(controller != NULL);
  assert(h2_g99_loader_controller_close(&controller) == H2_PAL_OK);
  assert(controller == NULL && !display_fixture.opened);
  assert(!display_fixture.wrong_thread);
  assert(h2_g99_loader_controller_close(&controller) == H2_PAL_OK);
  memset(&display_fixture, 0, sizeof(display_fixture));
  display_fixture.fail_present = true;
  assert(h2_g99_loader_controller_open(&runtime, &controller) == H2_PAL_ERR_IO);
  assert(controller == NULL && !display_fixture.opened);
  assert(!display_fixture.wrong_thread);
  memset(&display_fixture, 0, sizeof(display_fixture));
  assert(h2_g99_loader_controller_open(&runtime, &controller) == H2_PAL_OK);
  fail_response = true;
  assert(h2_g99_loader_controller_installing(controller) == H2_PAL_ERR_CLOSED);
  assert(h2_g99_loader_controller_close(&controller) == H2_PAL_OK);
  assert(controller == NULL && !display_fixture.opened);
  fail_response = false;
  runtime.queue = NULL;
  assert(h2_g99_loader_controller_open(&runtime, &controller) ==
         H2_PAL_ERR_INVALID_ARG);
  return 0;
}
