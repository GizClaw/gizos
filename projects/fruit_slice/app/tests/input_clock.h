/* The scripted App fixtures advance virtual time by frames. Let the real
 * Runtime input worker run a complete acquisition tick at each frame boundary
 * before the fixture mutates its next sample. All other tasks keep real time. */
#ifndef H2_FRUIT_TEST_INPUT_CLOCK_H
#define H2_FRUIT_TEST_INPUT_CLOCK_H
#include "h2_runtime_task_names.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>

static struct {
  pthread_mutex_t lock;
  pthread_cond_t changed;
  unsigned requested, completed;
  bool stopping;
  h2_pal_task_t* worker;
  h2_pal_task_entry_t entry;
  void* ctx;
  const h2_pal_task_api_t* tasks;
  const h2_pal_time_api_t* time;
} input_clock = {.lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};
static _Thread_local bool input_thread;

static void await_tick(void) {
  while (!input_clock.stopping && input_clock.completed == input_clock.requested)
    pthread_cond_wait(&input_clock.changed, &input_clock.lock);
}
static void input_entry(void* unused) {
  (void)unused;
  input_thread = true;
  pthread_mutex_lock(&input_clock.lock);
  await_tick();
  pthread_mutex_unlock(&input_clock.lock);
  input_clock.entry(input_clock.ctx);
}
static int input_sleep(void* user, uint32_t ms) {
  (void)user;
  if (!input_thread) return h2_pal_time_sleep_ms(input_clock.time, ms);
  pthread_mutex_lock(&input_clock.lock);
  ++input_clock.completed;
  pthread_cond_broadcast(&input_clock.changed);
  await_tick();
  pthread_mutex_unlock(&input_clock.lock);
  return H2_PAL_OK;
}
static void input_tick(void) {
  pthread_mutex_lock(&input_clock.lock);
  ++input_clock.requested;
  pthread_cond_broadcast(&input_clock.changed);
  while (input_clock.completed != input_clock.requested)
    pthread_cond_wait(&input_clock.changed, &input_clock.lock);
  pthread_mutex_unlock(&input_clock.lock);
}
static int input_start(void* user, const h2_pal_task_options_t* options,
                       h2_pal_task_entry_t entry, void* ctx, h2_pal_task_t** out) {
  (void)user;
  if (strcmp(options->name, h2_runtime_input_task_name) != 0)
    return h2_pal_task_start(input_clock.tasks, options, entry, ctx, out);
  input_clock.requested = input_clock.completed = 0u;
  input_clock.stopping = false;
  input_clock.entry = entry;
  input_clock.ctx = ctx;
  const int rc = h2_pal_task_start(h2_desktop_platform_task_api(), options, input_entry, NULL, out);
  if (rc == H2_PAL_OK) input_clock.worker = *out;
  return rc;
}
static int input_join(void* user, h2_pal_task_t* task) {
  (void)user;
  if (task != input_clock.worker) return h2_pal_task_join(input_clock.tasks, task);
  pthread_mutex_lock(&input_clock.lock);
  input_clock.stopping = true;
  pthread_cond_broadcast(&input_clock.changed);
  pthread_mutex_unlock(&input_clock.lock);
  const int rc = h2_pal_task_join(h2_desktop_platform_task_api(), task);
  if (rc == H2_PAL_OK) input_clock.worker = NULL;
  return rc;
}
static void input_clock_install(h2_runtime_config_t* cfg) {
  static const h2_pal_task_vtable_t task_ops = {.start = input_start, .join = input_join};
  static const h2_pal_task_api_t tasks = {.vtable = &task_ops};
  static h2_pal_time_vtable_t time_ops;
  static h2_pal_time_api_t time;
  input_clock.tasks = cfg->task;
  input_clock.time = cfg->time;
  time_ops = *cfg->time->vtable;
  time_ops.sleep_ms = input_sleep;
  time = (h2_pal_time_api_t){cfg->time->user, &time_ops};
  cfg->time = &time;
  cfg->task = &tasks;
}
#endif
