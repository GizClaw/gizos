#include "h2_h2loader_serial_e2e.h"
#include "h2_pal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fake_snapshot {
  int unused;
} fake_snapshot_t;

static h2_pal_result_t fake_scan(void *user,
                                 h2_pal_serial_host_snapshot_t **out) {
  (void)user;
  *out = (h2_pal_serial_host_snapshot_t *)calloc(1u, sizeof(fake_snapshot_t));
  return *out == NULL ? H2_PAL_ERR_NO_MEMORY : H2_PAL_OK;
}
static h2_pal_result_t fake_count(void *user,
                                  const h2_pal_serial_host_snapshot_t *snapshot,
                                  size_t *out_count) {
  (void)user;
  (void)snapshot;
  *out_count = 1u;
  return H2_PAL_OK;
}
static h2_pal_result_t fake_get(void *user,
                                const h2_pal_serial_host_snapshot_t *snapshot,
                                size_t index,
                                h2_pal_serial_host_port_info_t *out) {
  (void)user;
  (void)snapshot;
  if (index != 0u) return H2_PAL_ERR_NOT_FOUND;
  strcpy(out->port_id, "test-port");
  strcpy(out->endpoint, "test-port");
  return H2_PAL_OK;
}
static h2_pal_result_t fake_destroy(void *user,
                                    h2_pal_serial_host_snapshot_t **snapshot) {
  (void)user;
  free(*snapshot);
  *snapshot = NULL;
  return H2_PAL_OK;
}

int main(void) {
  static const h2_pal_serial_host_vtable_t vtable = {
      .scan = fake_scan,
      .snapshot_count = fake_count,
      .snapshot_get = fake_get,
      .snapshot_destroy = fake_destroy,
  };
  const h2_pal_serial_host_api_t serial = {NULL, &vtable};
  h2_runtime_t runtime = {0};
  runtime.mem = h2_pal_unsupported_mem_api();
  runtime.time = h2_pal_unsupported_time_api();
  h2_h2loader_serial_e2e_config_t config = {
      .suite_mask = H2_H2LOADER_SERIAL_E2E_SUITE_PREFLIGHT,
      .serial = &serial,
      .port_id = "test-port",
  };
  h2_h2loader_serial_e2e_result_t result;
  assert(h2_h2loader_serial_e2e_run(&runtime, &config, &result) == H2_PAL_OK);
  assert(result.selected == 1u && result.passed == 1u && result.complete == 1);
  assert(result.enumerated_ports == 1u);
  config.port_id = NULL;
  assert(h2_h2loader_serial_e2e_run(&runtime, &config, &result) == H2_PAL_OK);
  assert(result.selected == 1u && result.passed == 1u &&
         result.enumerated_ports == 1u);
  config.port_id = "missing";
  assert(h2_h2loader_serial_e2e_run(&runtime, &config, &result) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(result.failed == 1u);

  config.suite_mask = H2_H2LOADER_SERIAL_E2E_SUITE_COMMAND;
  config.port_id = NULL;
  assert(h2_h2loader_serial_e2e_run(&runtime, &config, &result) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(result.complete == 1 && result.case_count == 0u);

  config.port_id = "test-port";
  config.command = (h2_h2loader_host_command_t)99;
  assert(h2_h2loader_serial_e2e_run(&runtime, &config, &result) ==
         H2_PAL_ERR_INVALID_ARG);

  config.suite_mask = H2_H2LOADER_SERIAL_E2E_SUITE_INSTALL;
  config.command = H2_H2LOADER_HOST_COMMAND_STATUS;
  assert(h2_h2loader_serial_e2e_run(&runtime, &config, &result) ==
         H2_PAL_ERR_INVALID_ARG);

  config.suite_mask = H2_H2LOADER_SERIAL_E2E_SUITE_PREFLIGHT |
                      H2_H2LOADER_SERIAL_E2E_SUITE_COMMAND;
  config.port_id = "missing";
  config.command = H2_H2LOADER_HOST_COMMAND_HELP;
  assert(h2_h2loader_serial_e2e_run(&runtime, &config, &result) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(result.selected == 2u && result.failed == 1u &&
         result.skipped == 1u && result.case_count == 2u);
  assert(result.cases[1].case_id == H2_H2LOADER_SERIAL_E2E_CASE_COMMAND &&
         result.cases[1].result == H2_PAL_ERR_UNAVAILABLE);
  puts("h2loader serial e2e tests passed");
  return 0;
}
