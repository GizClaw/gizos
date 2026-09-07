#include "h2_app_test_sync.h"
#include <string.h>
static int index_of(h2_app_test_sync_t *s, h2_pal_mutex_t *m) {
  for (unsigned i = 0; i < H2_APP_TEST_SYNC_MUTEXES_MAX; ++i)
    if ((void *)m == (void *)&s->mutexes[i] && s->mutexes[i].active)
      return (int)i;
  return -1;
}
static h2_pal_result_t create(void *user, const h2_pal_mutex_config_t *config,
                              h2_pal_mutex_t **out) {
  h2_app_test_sync_t *s = user;
  if (!out || !config)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  if (config->flags != H2_PAL_MUTEX_FLAG_NONE)
    return H2_PAL_ERR_UNSUPPORTED;
  int rc = h2_app_test_fault_take(&s->create);
  if (rc)
    return rc;
  for (unsigned i = 0; i < H2_APP_TEST_SYNC_MUTEXES_MAX; ++i) {
    if (s->mutexes[i].active)
      continue;
    s->mutexes[i].active = true;
    s->mutexes[i].locked = false;
    *out = (h2_pal_mutex_t *)&s->mutexes[i];
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NO_SPACE;
}
static h2_pal_result_t destroy(void *user, h2_pal_mutex_t *mutex) {
  h2_app_test_sync_t *s = user;
  int i = index_of(s, mutex);
  if (i < 0)
    return H2_PAL_ERR_INVALID_ARG;
  if (s->mutexes[i].locked)
    return H2_PAL_ERR_BUSY;
  int rc = h2_app_test_fault_take(&s->destroy);
  if (!rc)
    s->mutexes[i].active = false;
  return rc;
}
static h2_pal_result_t lock(void *user, h2_pal_mutex_t *mutex) {
  h2_app_test_sync_t *s = user;
  int i = index_of(s, mutex);
  if (i < 0)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_app_test_fault_take(&s->lock);
  if (rc)
    return rc;
  if (s->mutexes[i].locked)
    return H2_PAL_ERR_BUSY;
  s->mutexes[i].locked = true;
  return H2_PAL_OK;
}
static h2_pal_result_t unlock(void *user, h2_pal_mutex_t *mutex) {
  h2_app_test_sync_t *s = user;
  int i = index_of(s, mutex);
  if (i < 0 || !s->mutexes[i].locked)
    return H2_PAL_ERR_INVALID_STATE;
  int rc = h2_app_test_fault_take(&s->unlock);
  if (!rc)
    s->mutexes[i].locked = false;
  return rc;
}
static const h2_pal_sync_vtable_t vtable = {.create_mutex = create,
                                            .destroy_mutex = destroy,
                                            .lock_mutex = lock,
                                            .try_lock_mutex = lock,
                                            .unlock_mutex = unlock};
void h2_app_test_sync_init(h2_app_test_sync_t *s) {
  if (!s)
    return;
  memset(s, 0, sizeof(*s));
  s->api = (h2_pal_sync_api_t){s, &vtable};
}
