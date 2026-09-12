#include "h2_jieli_br35_platform_core.h"
#include "h2_jieli_br35_sdk_port.h"

#include "h2_jieli_br35_atomic.h"
#include <stdio.h>
#include <string.h>

#ifndef H2_JIELI_BR35_TASK_DEFAULT_STACK_BYTES
#define H2_JIELI_BR35_TASK_DEFAULT_STACK_BYTES 4096u
#endif

struct h2_pal_task {
  h2_pal_task_entry_t entry;
  void *ctx;
  char name[16];
  volatile uint32_t done;
  const void *owner;
};

static void task_trampoline(void *arg) {
  h2_pal_task_t *task = (h2_pal_task_t *)arg;
  task->entry(task->ctx);
  h2_jieli_atomic_store_u32(&task->done, 1u);
  /* The joining owner deletes the parked native task before freeing us. */
  h2_jieli_sdk_task_park();
}

static int task_start(void *user, const h2_pal_task_options_t *options,
                      h2_pal_task_entry_t entry, void *ctx,
                      h2_pal_task_t **out_task) {
  h2_pal_task_t *task;
  size_t stack_bytes = H2_JIELI_BR35_TASK_DEFAULT_STACK_BYTES;
  static volatile uint32_t sequence;
  uint32_t id;
  (void)user;
  if (entry == NULL || out_task == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_task = NULL;
  if (options != NULL) {
    if (options->min_stack_size > stack_bytes) {
      stack_bytes = options->min_stack_size;
    }
  }
  task = (h2_pal_task_t *)h2_jieli_sdk_malloc(sizeof(*task));
  if (task == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(task, 0, sizeof(*task));
  task->entry = entry;
  task->ctx = ctx;
  id = h2_jieli_atomic_load_u32(&sequence);
  while (!h2_jieli_atomic_cas_u32(&sequence, &id, id + 1u)) {
  }
  snprintf(task->name, sizeof(task->name), "h2_%08x", (unsigned)id);
  task->owner = h2_jieli_sdk_task_current();
  if (h2_jieli_sdk_task_create(task_trampoline, task, task->name,
                               stack_bytes) != 0) {
    h2_jieli_sdk_free(task);
    return H2_PAL_ERR_TASK;
  }
  *out_task = task;
  return H2_PAL_OK;
}

static int task_join(void *user, h2_pal_task_t *task) {
  (void)user;
  if (task == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (task->owner != h2_jieli_sdk_task_current())
    return H2_PAL_ERR_INVALID_STATE;
  while (!h2_jieli_atomic_load_u32(&task->done))
    h2_jieli_sdk_sleep_ms(10u);
  if (h2_jieli_sdk_task_delete(task->name) != 0)
    return H2_PAL_ERR_BUSY;
  h2_jieli_sdk_free(task);
  return H2_PAL_OK;
}

static const h2_pal_task_vtable_t s_task_vtable = {
    .start = task_start,
    .join = task_join,
};

static const h2_pal_task_api_t s_task_api = {
    .user = NULL,
    .vtable = &s_task_vtable,
};

const h2_pal_task_api_t *h2_jieli_br35_platform_task_api(void) {
  return &s_task_api;
}
