#ifndef H2_H2LOADER_SERIAL_E2E_H
#define H2_H2LOADER_SERIAL_E2E_H

#include "h2_h2loader_host.h"
#include "h2_runtime.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_H2LOADER_SERIAL_E2E_MAX_CASES 5u

typedef enum h2_h2loader_serial_e2e_suite {
  H2_H2LOADER_SERIAL_E2E_SUITE_PREFLIGHT = 1u << 0,
  H2_H2LOADER_SERIAL_E2E_SUITE_STATUS = 1u << 1,
  H2_H2LOADER_SERIAL_E2E_SUITE_COMMAND = 1u << 2,
  H2_H2LOADER_SERIAL_E2E_SUITE_INSTALL = 1u << 3,
  H2_H2LOADER_SERIAL_E2E_SUITE_LOADER_UPDATE = 1u << 4,
} h2_h2loader_serial_e2e_suite_t;

typedef enum h2_h2loader_serial_e2e_case_id {
  H2_H2LOADER_SERIAL_E2E_CASE_PREFLIGHT = 1,
  H2_H2LOADER_SERIAL_E2E_CASE_STATUS,
  H2_H2LOADER_SERIAL_E2E_CASE_COMMAND,
  H2_H2LOADER_SERIAL_E2E_CASE_INSTALL,
  H2_H2LOADER_SERIAL_E2E_CASE_LOADER_UPDATE,
} h2_h2loader_serial_e2e_case_id_t;

typedef struct h2_h2loader_serial_e2e_config {
  uint32_t suite_mask;
  const h2_pal_serial_host_api_t *serial;
  /** Optional for preflight-only runs; required by every live suite. */
  const char *port_id;
  const char *expected_board;
  const char *expected_target;
  h2_h2loader_host_command_t command;
  const uint8_t *catalog_json;
  size_t catalog_json_len;
  const char *asset_sha256;
  h2_h2loader_host_resource_read_fn read_resource;
  void *resource_user;
  h2_h2loader_host_cancelled_fn is_cancelled;
  void *cancel_user;
  uint32_t handshake_timeout_ms;
  uint32_t command_timeout_ms;
  uint32_t reconnect_delay_ms;
  uint32_t reconnect_attempts;
} h2_h2loader_serial_e2e_config_t;

typedef struct h2_h2loader_serial_e2e_case_result {
  h2_h2loader_serial_e2e_case_id_t case_id;
  h2_pal_result_t result;
} h2_h2loader_serial_e2e_case_result_t;

typedef struct h2_h2loader_serial_e2e_result {
  h2_pal_result_t result;
  h2_pal_result_t cleanup_result;
  h2_h2loader_serial_e2e_case_result_t cases[H2_H2LOADER_SERIAL_E2E_MAX_CASES];
  size_t case_count;
  size_t selected;
  size_t passed;
  size_t failed;
  size_t skipped;
  size_t enumerated_ports;
  size_t command_output_bytes;
  h2_pal_result_t command_transport_result;
  h2_h2loader_host_command_terminal_t command_terminal;
  uint8_t command_output_truncated;
  uint8_t command_lifecycle_transition;
  uint64_t acknowledged_bytes;
  uint64_t total_bytes;
  uint64_t elapsed_ms;
  /** Loader-update only: a reconnect saw the candidate Loader on Partition 2.
   * Timing-dependent evidence of the relay; not a pass condition. */
  uint8_t loader_trial_observed;
  h2_h2loader_host_status_t initial_status;
  h2_h2loader_host_status_t final_status;
  int complete;
} h2_h2loader_serial_e2e_result_t;

/**
 * Loader-update precondition: the asset is a managed Loader install for this
 * board and its image differs from the running Loader in Partition 1, so the
 * device must relay through Partition 2 instead of taking the same-image path.
 */
h2_pal_result_t h2_h2loader_serial_e2e_loader_update_ready(
    const h2_h2loader_host_status_t *before,
    const h2_h2loader_host_catalog_entry_t *asset);

/**
 * Loader-update completion: running Loader on Partition 1, Partition 1 and 2
 * valid Loader metadata with the asset image, the asset package recorded in
 * Partition 1, the asset version active, and no Stage left.
 */
h2_pal_result_t h2_h2loader_serial_e2e_loader_update_complete(
    const h2_h2loader_host_status_t *after,
    const h2_h2loader_host_catalog_entry_t *asset);

/** Run selected blocking cases and close every App-owned Host Core handle. */
h2_pal_result_t h2_h2loader_serial_e2e_run(
    h2_runtime_t *runtime,
    const h2_h2loader_serial_e2e_config_t *config,
    h2_h2loader_serial_e2e_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif
