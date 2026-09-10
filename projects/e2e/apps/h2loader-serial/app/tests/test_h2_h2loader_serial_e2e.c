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

#define OLD_IMAGE "1111111111111111111111111111111111111111111111111111111111111111"
#define NEW_IMAGE "2222222222222222222222222222222222222222222222222222222222222222"
#define NEW_PACKAGE "3333333333333333333333333333333333333333333333333333333333333333"

static void set_loader_metadata(h2_h2loader_host_metadata_t *metadata,
                                const char *image, const char *version) {
  memset(metadata, 0, sizeof(*metadata));
  strcpy(metadata->image_checksum, image);
  strcpy(metadata->package_checksum, NEW_PACKAGE);
  strcpy(metadata->version, version);
  strcpy(metadata->board, "board");
  strcpy(metadata->target, "target");
  metadata->role = H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER;
  metadata->valid = 1u;
}

static h2_h2loader_host_catalog_entry_t loader_asset(void) {
  h2_h2loader_host_catalog_entry_t asset;
  memset(&asset, 0, sizeof(asset));
  strcpy(asset.board, "board");
  strcpy(asset.target, "target");
  strcpy(asset.version, "2.0.0");
  strcpy(asset.sha256, NEW_PACKAGE);
  strcpy(asset.image_sha256, NEW_IMAGE);
  asset.role = H2_H2LOADER_HOST_ASSET_ROLE_LOADER;
  asset.operation = H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL;
  return asset;
}

static h2_h2loader_host_status_t updated_status(void) {
  h2_h2loader_host_status_t status;
  memset(&status, 0, sizeof(status));
  strcpy(status.board, "board");
  strcpy(status.target, "target");
  strcpy(status.active_version, "2.0.0");
  strcpy(status.active_checksum, NEW_IMAGE);
  status.active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER;
  status.running_partition = 1u;
  status.next_partition = 1u;
  set_loader_metadata(&status.partition_1, NEW_IMAGE, "2.0.0");
  set_loader_metadata(&status.partition_2, NEW_IMAGE, "2.0.0");
  return status;
}

static void test_loader_update_ready(void) {
  h2_h2loader_host_catalog_entry_t asset = loader_asset();
  h2_h2loader_host_status_t before = updated_status();
  set_loader_metadata(&before.partition_1, OLD_IMAGE, "1.0.0");
  assert(h2_h2loader_serial_e2e_loader_update_ready(&before, &asset) ==
         H2_PAL_OK);
  /* Running App with an invalid Partition 1 record still needs the relay. */
  before.active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_APP;
  before.partition_1.valid = 0u;
  strcpy(before.partition_1.image_checksum, NEW_IMAGE);
  assert(h2_h2loader_serial_e2e_loader_update_ready(&before, &asset) ==
         H2_PAL_OK);
  /* The same image takes the no-switch path and proves nothing. */
  before.partition_1.valid = 1u;
  assert(h2_h2loader_serial_e2e_loader_update_ready(&before, &asset) ==
         H2_PAL_ERR_INVALID_STATE);
  set_loader_metadata(&before.partition_1, OLD_IMAGE, "1.0.0");
  strcpy(before.board, "other");
  assert(h2_h2loader_serial_e2e_loader_update_ready(&before, &asset) ==
         H2_PAL_ERR_INVALID_STATE);
  strcpy(before.board, "board");
  asset.role = H2_H2LOADER_HOST_ASSET_ROLE_APP;
  assert(h2_h2loader_serial_e2e_loader_update_ready(&before, &asset) ==
         H2_PAL_ERR_INVALID_ARG);
  asset = loader_asset();
  asset.operation = H2_H2LOADER_HOST_ASSET_OPERATION_RECOVERY;
  assert(h2_h2loader_serial_e2e_loader_update_ready(&before, &asset) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_h2loader_serial_e2e_loader_update_ready(NULL, &asset) ==
         H2_PAL_ERR_INVALID_ARG);
}

static void test_loader_update_complete(void) {
  const h2_h2loader_host_catalog_entry_t asset = loader_asset();
  h2_h2loader_host_status_t after = updated_status();
  assert(h2_h2loader_serial_e2e_loader_update_complete(&after, &asset) ==
         H2_PAL_OK);
  /* Candidate Loader still on Partition 2 before its writeback. */
  after.running_partition = 2u;
  after.stage.valid = 1u;
  assert(h2_h2loader_serial_e2e_loader_update_complete(&after, &asset) ==
         H2_PAL_ERR_INVALID_STATE);
  after = updated_status();
  after.stage.valid = 1u;
  assert(h2_h2loader_serial_e2e_loader_update_complete(&after, &asset) ==
         H2_PAL_ERR_INVALID_STATE);
  /* Partition 2 only carried the relay; its content does not matter. */
  after = updated_status();
  set_loader_metadata(&after.partition_2, OLD_IMAGE, "1.0.0");
  assert(h2_h2loader_serial_e2e_loader_update_complete(&after, &asset) ==
         H2_PAL_OK);
  after.partition_2.valid = 0u;
  assert(h2_h2loader_serial_e2e_loader_update_complete(&after, &asset) ==
         H2_PAL_OK);
  after = updated_status();
  after.partition_1.valid = 0u;
  assert(h2_h2loader_serial_e2e_loader_update_complete(&after, &asset) ==
         H2_PAL_ERR_INVALID_STATE);
  after = updated_status();
  strcpy(after.partition_1.package_checksum, OLD_IMAGE);
  assert(h2_h2loader_serial_e2e_loader_update_complete(&after, &asset) ==
         H2_PAL_ERR_INVALID_STATE);
  after = updated_status();
  strcpy(after.active_version, "1.0.0");
  assert(h2_h2loader_serial_e2e_loader_update_complete(&after, &asset) ==
         H2_PAL_ERR_INVALID_STATE);
  after = updated_status();
  after.active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_APP;
  assert(h2_h2loader_serial_e2e_loader_update_complete(&after, &asset) ==
         H2_PAL_ERR_INVALID_STATE);
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

  config.suite_mask = H2_H2LOADER_SERIAL_E2E_SUITE_LOADER_UPDATE;
  assert(h2_h2loader_serial_e2e_run(&runtime, &config, &result) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(result.complete == 1 && result.case_count == 0u);
  config.suite_mask = 1u << 5;
  assert(h2_h2loader_serial_e2e_run(&runtime, &config, &result) ==
         H2_PAL_ERR_INVALID_ARG);

  config.suite_mask = H2_H2LOADER_SERIAL_E2E_SUITE_PREFLIGHT |
                      H2_H2LOADER_SERIAL_E2E_SUITE_LOADER_UPDATE;
  config.port_id = "missing";
  config.expected_board = "board";
  config.expected_target = "target";
  assert(h2_h2loader_serial_e2e_run(&runtime, &config, &result) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(result.skipped == 1u &&
         result.cases[1].case_id == H2_H2LOADER_SERIAL_E2E_CASE_LOADER_UPDATE &&
         result.cases[1].result == H2_PAL_ERR_UNAVAILABLE);
  config.expected_board = NULL;
  config.expected_target = NULL;

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
  test_loader_update_ready();
  test_loader_update_complete();
  puts("h2loader serial e2e tests passed");
  return 0;
}
