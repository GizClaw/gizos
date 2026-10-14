#include "h2_bk_board_application_host.h"
#include "h2_bk7258_board.h"
static h2_pal_result_t prepare(void *user, struct h2_runtime_config *out) {
  (void)user;
  return h2_bk7258_board_runtime_config(out);
}
static h2_pal_result_t activate(void *user, struct h2_runtime *runtime) {
  (void)user;
  (void)runtime;
  return H2_PAL_OK;
}
static h2_pal_result_t poll(void *user, int *out_stop) {
  (void)user;
  *out_stop = 0;
  return H2_PAL_OK;
}
static h2_pal_result_t deactivate(void *user) {
  (void)user;
  return H2_PAL_OK;
}
static h2_pal_result_t release(void *user) {
  (void)user;
  return h2_bk7258_board_runtime_deinit();
}
static void notify(void *user, h2_pal_application_host_event_t event) {
  (void)user;
  (void)event;
}
static const h2_pal_application_host_vtable_t vtable = {.prepare = prepare,
                                                        .activate = activate,
                                                        .poll = poll,
                                                        .deactivate =
                                                            deactivate,
                                                        .release = release,
                                                        .notify = notify};
h2_pal_result_t
h2_bk_board_application_host_init(h2_bk_application_host_t *provider) {
  const h2_pal_application_host_api_t platform = {.vtable = &vtable};
  return h2_bk_application_host_init(provider, &platform);
}
