#include "h2_pal_pref_e2e.h"

#include <stdint.h>
#include <string.h>

#define H2_PAL_PREF_E2E_CONTROL_NAMESPACE "h2e2eprefctl"
#define H2_PAL_PREF_E2E_DATA_NAMESPACE "h2e2eprefdata"
#define H2_PAL_PREF_E2E_PHASE_KEY "phase"
#define H2_PAL_PREF_E2E_BLOB_SIZE (16u * 1024u)
#define H2_PAL_PREF_E2E_REWRITE_COUNT 1000u

static void record_case(h2_pal_e2e_result_t *result, int rc) {
  result->selected++;
  if (rc == H2_PAL_OK) result->passed++;
  else result->failed++;
  if (result->result == H2_PAL_OK && rc != H2_PAL_OK) result->result = rc;
}

static int close_namespace(h2_pal_pref_namespace_t **namespace_ptr) {
  int rc;
  if (namespace_ptr == NULL || *namespace_ptr == NULL) return H2_PAL_OK;
  rc = (*namespace_ptr)->close(*namespace_ptr);
  *namespace_ptr = NULL;
  return rc;
}

static int get_phase(h2_runtime_t *runtime, h2_pal_pref_namespace_t *control,
                     h2_pal_e2e_pref_phase_t *out_phase) {
  uint32_t value = 0u;
  int rc = control->get_u32(control, H2_PAL_PREF_E2E_PHASE_KEY, &value);
  (void)runtime;
  if (rc == H2_PAL_ERR_NOT_FOUND) {
    *out_phase = H2_PAL_E2E_PREF_PHASE_SEED;
    return H2_PAL_OK;
  }
  if (rc != H2_PAL_OK || value > H2_PAL_E2E_PREF_PHASE_COMPLETE)
    return rc == H2_PAL_OK ? H2_PAL_ERR_IO : rc;
  *out_phase = (h2_pal_e2e_pref_phase_t)value;
  return H2_PAL_OK;
}

static int verify_iteration(h2_pal_pref_namespace_t *data, size_t expected) {
  h2_pal_pref_cursor_t *cursor = NULL;
  h2_pal_pref_entry_t entry;
  size_t count = 0u;
  int rc;
  while ((rc = data->iterate(data, &cursor, &entry)) == H2_PAL_OK) count++;
  if (cursor != NULL) (void)data->iterate_close(data, &cursor);
  return rc == H2_PAL_ERR_NOT_FOUND && count == expected ? H2_PAL_OK
                                                         : H2_PAL_ERR_IO;
}

static int seed_phase(h2_runtime_t *runtime, h2_pal_pref_namespace_t *control,
                      h2_pal_pref_namespace_t *data,
                      h2_pal_e2e_result_t *result) {
  uint8_t *blob = (uint8_t *)h2_pal_mem_alloc(runtime->mem,
                                               H2_PAL_PREF_E2E_BLOB_SIZE);
  void *read_blob = NULL;
  size_t read_blob_size = 0u;
  uint32_t u32 = 0u;
  int32_t i32 = 0;
  int boolean = 0;
  char *string = NULL;
  unsigned index;
  int rc;
  if (blob == NULL) return H2_PAL_ERR_NO_MEMORY;
  for (index = 0u; index < H2_PAL_PREF_E2E_BLOB_SIZE; ++index)
    blob[index] = (uint8_t)(index * 31u);
  rc = data->clear(data); record_case(result, rc);
  rc = data->set_blob(data, "blob", blob, H2_PAL_PREF_E2E_BLOB_SIZE); record_case(result, rc);
  if (rc == H2_PAL_OK) rc = data->get_blob(data, runtime->mem, "blob", &read_blob, &read_blob_size);
  if (rc == H2_PAL_OK && (read_blob_size != H2_PAL_PREF_E2E_BLOB_SIZE || memcmp(blob, read_blob, read_blob_size) != 0)) rc = H2_PAL_ERR_IO;
  record_case(result, rc); h2_pal_mem_free(runtime->mem, read_blob);
  rc = data->set_string(data, "string", "preference-e2e"); record_case(result, rc);
  if (rc == H2_PAL_OK) rc = data->get_string(data, runtime->mem, "string", &string);
  if (rc == H2_PAL_OK && strcmp(string, "preference-e2e") != 0) rc = H2_PAL_ERR_IO;
  record_case(result, rc); h2_pal_mem_free(runtime->mem, string);
  rc = data->set_u32(data, "u32", 0x89abcdefu); record_case(result, rc);
  if (rc == H2_PAL_OK) rc = data->get_u32(data, "u32", &u32);
  if (rc == H2_PAL_OK && u32 != 0x89abcdefu) rc = H2_PAL_ERR_IO;
  record_case(result, rc);
  rc = data->set_i32(data, "i32", -1234567); record_case(result, rc);
  if (rc == H2_PAL_OK) rc = data->get_i32(data, "i32", &i32);
  if (rc == H2_PAL_OK && i32 != -1234567) rc = H2_PAL_ERR_IO;
  record_case(result, rc);
  rc = data->set_bool(data, "bool", 1); record_case(result, rc);
  if (rc == H2_PAL_OK) rc = data->get_bool(data, "bool", &boolean);
  if (rc == H2_PAL_OK && !boolean) rc = H2_PAL_ERR_IO;
  record_case(result, rc);
  rc = verify_iteration(data, 5u); record_case(result, rc);
  rc = data->set_string(data, "string", "preference-e2e"); record_case(result, rc);
  for (index = 0u; rc == H2_PAL_OK && index < H2_PAL_PREF_E2E_REWRITE_COUNT; ++index)
    rc = data->set_u32(data, "u32", index);
  record_case(result, rc);
  if (rc == H2_PAL_OK) rc = data->commit(data);
  record_case(result, rc);
  h2_pal_mem_free(runtime->mem, blob);
  if (result->failed != 0u) return result->result;
  rc = control->set_u32(control, H2_PAL_PREF_E2E_PHASE_KEY,
                        H2_PAL_E2E_PREF_PHASE_VERIFY);
  record_case(result, rc);
  if (rc == H2_PAL_OK) rc = control->commit(control);
  record_case(result, rc);
  if (rc == H2_PAL_OK) result->action = H2_PAL_E2E_ACTION_REBOOT;
  return rc;
}

static int verify_phase(h2_runtime_t *runtime,
                        h2_pal_pref_namespace_t *control,
                        h2_pal_pref_namespace_t *data,
                        h2_pal_e2e_result_t *result) {
  uint8_t *expected_blob = NULL;
  void *blob = NULL;
  size_t blob_size = 0u;
  uint32_t value = 0u;
  int32_t signed_value = 0;
  int boolean = 0;
  char *string = NULL;
  unsigned index;
  int rc = data->get_u32(data, "u32", &value);
  if (rc == H2_PAL_OK && value != H2_PAL_PREF_E2E_REWRITE_COUNT - 1u) rc = H2_PAL_ERR_IO;
  record_case(result, rc);
  expected_blob = (uint8_t *)h2_pal_mem_alloc(runtime->mem,
                                              H2_PAL_PREF_E2E_BLOB_SIZE);
  if (expected_blob == NULL) {
    record_case(result, H2_PAL_ERR_NO_MEMORY);
  } else {
    for (index = 0u; index < H2_PAL_PREF_E2E_BLOB_SIZE; ++index)
      expected_blob[index] = (uint8_t)(index * 31u);
    rc = data->get_blob(data, runtime->mem, "blob", &blob, &blob_size);
    if (rc == H2_PAL_OK &&
        (blob_size != H2_PAL_PREF_E2E_BLOB_SIZE ||
         memcmp(expected_blob, blob, blob_size) != 0))
      rc = H2_PAL_ERR_IO;
    record_case(result, rc);
  }
  h2_pal_mem_free(runtime->mem, expected_blob);
  h2_pal_mem_free(runtime->mem, blob);
  rc = data->get_string(data, runtime->mem, "string", &string);
  if (rc == H2_PAL_OK && strcmp(string, "preference-e2e") != 0)
    rc = H2_PAL_ERR_IO;
  record_case(result, rc);
  h2_pal_mem_free(runtime->mem, string);
  rc = data->get_i32(data, "i32", &signed_value);
  if (rc == H2_PAL_OK && signed_value != -1234567) rc = H2_PAL_ERR_IO;
  record_case(result, rc);
  rc = data->get_bool(data, "bool", &boolean);
  if (rc == H2_PAL_OK && !boolean) rc = H2_PAL_ERR_IO;
  record_case(result, rc);
  rc = verify_iteration(data, 5u); record_case(result, rc);
  rc = data->remove(data, "bool"); record_case(result, rc);
  rc = data->clear(data); record_case(result, rc);
  if (rc == H2_PAL_OK) rc = data->commit(data);
  record_case(result, rc);
  if (result->failed != 0u) return result->result;
  rc = control->set_u32(control, H2_PAL_PREF_E2E_PHASE_KEY,
                        H2_PAL_E2E_PREF_PHASE_CLEAN);
  record_case(result, rc);
  if (rc == H2_PAL_OK) rc = control->commit(control);
  record_case(result, rc);
  if (rc == H2_PAL_OK) result->action = H2_PAL_E2E_ACTION_REBOOT;
  return rc;
}

static int clean_phase(h2_pal_pref_namespace_t *control,
                       h2_pal_pref_namespace_t *data,
                       h2_pal_e2e_result_t *result, int terminal_replay) {
  int rc = verify_iteration(data, 0u);
  record_case(result, rc);
  if (rc == H2_PAL_OK && !terminal_replay) {
    rc = control->set_u32(control, H2_PAL_PREF_E2E_PHASE_KEY,
                          H2_PAL_E2E_PREF_PHASE_COMPLETE);
    record_case(result, rc);
    if (rc == H2_PAL_OK) rc = control->commit(control);
    record_case(result, rc);
    if (rc == H2_PAL_OK)
      result->pref_phase = H2_PAL_E2E_PREF_PHASE_COMPLETE;
  }
  return rc;
}

h2_pal_result_t h2_pal_pref_e2e_run(h2_runtime_t *runtime,
                                    h2_pal_e2e_result_t *result) {
  h2_pal_pref_namespace_t *control = NULL;
  h2_pal_pref_namespace_t *data = NULL;
  h2_pal_e2e_pref_phase_t phase = H2_PAL_E2E_PREF_PHASE_SEED;
  int rc;
  if (runtime == NULL || runtime->pref == NULL || runtime->mem == NULL || result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(result, 0, sizeof(*result));
  result->stage = H2_PAL_E2E_STAGE_PREFLIGHT;
  result->result = H2_PAL_OK;
  rc = h2_pal_pref_open(runtime->pref, H2_PAL_PREF_E2E_CONTROL_NAMESPACE,
                        H2_PAL_PREF_OPEN_READ_WRITE, &control);
  if (rc == H2_PAL_OK)
    rc = h2_pal_pref_open(runtime->pref, H2_PAL_PREF_E2E_DATA_NAMESPACE,
                          H2_PAL_PREF_OPEN_READ_WRITE, &data);
  if (rc == H2_PAL_OK) rc = get_phase(runtime, control, &phase);
  result->pref_phase = phase;
  if (rc == H2_PAL_OK && phase == H2_PAL_E2E_PREF_PHASE_SEED)
    rc = seed_phase(runtime, control, data, result);
  else if (rc == H2_PAL_OK && phase == H2_PAL_E2E_PREF_PHASE_VERIFY)
    rc = verify_phase(runtime, control, data, result);
  else if (rc == H2_PAL_OK && (phase == H2_PAL_E2E_PREF_PHASE_CLEAN || phase == H2_PAL_E2E_PREF_PHASE_COMPLETE))
    rc = clean_phase(control, data, result, phase == H2_PAL_E2E_PREF_PHASE_COMPLETE);
  if (close_namespace(&data) != H2_PAL_OK && rc == H2_PAL_OK) rc = H2_PAL_ERR_IO;
  if (close_namespace(&control) != H2_PAL_OK && rc == H2_PAL_OK) rc = H2_PAL_ERR_IO;
  if (rc != H2_PAL_OK && result->failed == 0u) record_case(result, rc);
  result->result = rc;
  result->complete = 1;
  if (rc == H2_PAL_OK) result->stage = H2_PAL_E2E_STAGE_COMPLETE;
  return rc;
}
