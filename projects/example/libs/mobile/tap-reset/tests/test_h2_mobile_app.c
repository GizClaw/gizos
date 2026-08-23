#include "h2_mobile_app.h"
#include "h2_tap_reset.h"

#include <assert.h>

static int s_tap_reset_calls;

h2_pal_result_t h2_tap_reset_run(h2_runtime_t *runtime,
                                 const h2_tap_reset_config_t *config) {
  (void)runtime;
  (void)config;
  ++s_tap_reset_calls;
  return H2_PAL_ERR_IO;
}

static h2_pal_result_t read_pointer(void *user,
                                    h2_mobile_pointer_state_t *out_state) {
  (void)user;
  (void)out_state;
  return H2_PAL_OK;
}

static int should_stop(void *user) {
  (void)user;
  return 1;
}

int main(void) {
  const h2_mobile_app_config_t config = {
      .platform = (h2_mobile_platform_t)-1,
      .read_pointer = read_pointer,
      .should_stop = should_stop,
  };
  assert(h2_mobile_app_run((h2_runtime_t *)1, &config) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(s_tap_reset_calls == 0);
  return 0;
}
