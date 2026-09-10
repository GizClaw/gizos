#include "h2_app_test_task.h"
#include <string.h>
h2_pal_result_t h2_app_test_task_run(h2_app_test_task_t *t) {
  if (!t || !t->active || t->running || t->complete)
    return H2_PAL_ERR_INVALID_STATE;
  t->running = true;
  h2_pal_task_entry_t entry = t->entry;
  t->entry = NULL;
  entry(t->context);
  t->running = false;
  t->complete = true;
  return H2_PAL_OK;
}
static int start(void *user, const h2_pal_task_options_t *options,
                 h2_pal_task_entry_t entry, void *context,
                 h2_pal_task_t **out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  h2_app_test_task_t *t = user;
  if (!options || !entry)
    return H2_PAL_ERR_INVALID_ARG;
  if (t->active)
    return H2_PAL_ERR_BUSY;
  int rc = h2_app_test_fault_take(&t->start);
  if (rc)
    return rc;
  t->options = *options;
  t->entry = entry;
  t->context = context;
  t->active = true;
  t->complete = false;
  *out = (h2_pal_task_t *)t;
  if (t->run_on_start)
    return h2_app_test_task_run(t);
  return H2_PAL_OK;
}
static int join(void *user, h2_pal_task_t *handle) {
  h2_app_test_task_t *t = user;
  if (handle != (h2_pal_task_t *)t || !t->active)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_app_test_fault_take(&t->join);
  if (rc)
    return rc;
  if (!t->complete || t->running)
    return H2_PAL_ERR_BUSY;
  t->active = false;
  t->entry = NULL;
  t->context = NULL;
  memset(&t->options, 0, sizeof(t->options));
  return H2_PAL_OK;
}
static const h2_pal_task_vtable_t vtable = {.start = start, .join = join};
void h2_app_test_task_init(h2_app_test_task_t *t) {
  if (!t)
    return;
  memset(t, 0, sizeof(*t));
  t->api = (h2_pal_task_api_t){t, &vtable};
}
