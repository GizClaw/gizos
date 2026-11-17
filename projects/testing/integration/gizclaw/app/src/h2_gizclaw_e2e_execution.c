#include "h2_gizclaw_e2e_execution.h"

#include "h2_atomic.h"
#include "ledger.h"

#include <stdio.h>
#include <string.h>

#define H2_GIZCLAW_E2E_LEDGER_CAPACITY (384u * 1024u)
#define H2_GIZCLAW_E2E_RECORD_CAPACITY 1024u

typedef enum execution_state {
  EXECUTION_CREATED,
  EXECUTION_RUNNING,
  EXECUTION_FINISHED,
  EXECUTION_PUBLISHED,
} execution_state_t;

struct h2_gizclaw_e2e_execution {
  h2_runtime_t* runtime;
  h2_gizclaw_e2e_config_t config;
  h2_gizclaw_e2e_execution_config_t evidence;
  h2_gizclaw_e2e_result_t result;
  h2_gizclaw_e2e_ledger_t ledger;
  h2_pal_mutex_t* capture_mutex;
  h2_pal_mutex_t* output_mutex;
  h2_atomic_bool_t capture_failed;
  bool capture_initialized;
  execution_state_t state;
  h2_gizclaw_e2e_exit_t exit_code;
  int confirm_rc;
  bool admitted;
  char execution[33];
  h2_pal_firmware_info_t firmware;
  char summary[H2_GIZCLAW_E2E_RECORD_CAPACITY];
  size_t summary_size;
};

static bool marker_identity_valid(const char* value) {
  if (value == NULL || value[0] == '\0') return false;
  size_t size = 0u;
  for (; value[size] != '\0'; ++size) {
    const char c = value[size];
    if (size >= 63u ||
        !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
      return false;
  }
  return true;
}

static int output_record(h2_gizclaw_e2e_execution_t* self, const char* record) {
  int rc = h2_pal_mutex_lock(self->runtime->sync, self->output_mutex);
  if (rc == H2_PAL_OK) {
    rc = h2_pal_log_write(self->evidence.output, H2_PAL_LOG_INFO, "gizclaw-e2e",
                          record);
    const int unlocked =
        h2_pal_mutex_unlock(self->runtime->sync, self->output_mutex);
    if (unlocked != H2_PAL_OK) rc = unlocked;
  }
  if (rc != H2_PAL_OK)
    h2_atomic_store_explicit(&self->capture_failed, true, H2_ATOMIC_RELEASE);
  return rc;
}

static bool record_has(const char* record, size_t size, const char* text) {
  const size_t length = strlen(text);
  if (record == NULL || length > size) return false;
  for (size_t offset = 0u; offset <= size - length; ++offset)
    if (memcmp(record + offset, text, length) == 0) return true;
  return false;
}

static void capture_evidence(void* user, const char* record, size_t size) {
  h2_gizclaw_e2e_execution_t* self = user;
  if (h2_pal_mutex_lock(self->runtime->sync, self->capture_mutex) !=
      H2_PAL_OK) {
    h2_atomic_store_explicit(&self->capture_failed, true, H2_ATOMIC_RELEASE);
    return;
  }
  h2_gizclaw_e2e_ledger_append(&self->ledger, record, size);
  if (size >= H2_GIZCLAW_E2E_RECORD_CAPACITY || self->ledger.error != H2_PAL_OK)
    h2_atomic_store_explicit(&self->capture_failed, true, H2_ATOMIC_RELEASE);
  if (h2_pal_mutex_unlock(self->runtime->sync, self->capture_mutex) !=
      H2_PAL_OK)
    h2_atomic_store_explicit(&self->capture_failed, true, H2_ATOMIC_RELEASE);
  /* Retained callbacks forbid freezing the acceptance ledger. Preserve only
   * the existing bounded, redacted failure/location diagnostics during a run.
   * Console output never holds the capture mutex or streams the full ledger. */
  if (record != NULL && size > 0u && size < H2_GIZCLAW_E2E_RECORD_CAPACITY &&
      (record_has(record, size, " rc=-") ||
       record_has(record, size, " stage=device-api ") ||
       record_has(record, size, " stage=local-player-error ") ||
       record_has(record, size, " stage=player-ended ") ||
       record_has(record, size, " stage=player-cadence "))) {
    char diagnostic[H2_GIZCLAW_E2E_RECORD_CAPACITY + 32u];
    const int count = snprintf(diagnostic, sizeof(diagnostic),
                               "H2_GIZCLAW_DIAGNOSTIC %.*s", (int)size, record);
    if (count <= 0 || (size_t)count >= sizeof(diagnostic) ||
        output_record(self, diagnostic) != H2_PAL_OK)
      h2_atomic_store_explicit(&self->capture_failed, true, H2_ATOMIC_RELEASE);
  }
}

int h2_gizclaw_e2e_execution_destroy(h2_gizclaw_e2e_execution_t** execution) {
  if (execution == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_e2e_execution_t* self = *execution;
  if (self == NULL) return H2_PAL_OK;
  if (self->state == EXECUTION_RUNNING || self->result.retained_resources != 0u)
    return H2_PAL_ERR_BUSY;
  if (self->output_mutex != NULL) {
    const int rc =
        h2_pal_mutex_destroy(self->runtime->sync, self->output_mutex);
    if (rc != H2_PAL_OK) return rc;
    self->output_mutex = NULL;
  }
  if (self->capture_mutex != NULL) {
    const int rc =
        h2_pal_mutex_destroy(self->runtime->sync, self->capture_mutex);
    if (rc != H2_PAL_OK) return rc;
    self->capture_mutex = NULL;
  }
  if (self->capture_initialized) h2_atomic_destroy(&self->capture_failed);
  h2_gizclaw_e2e_ledger_deinit(&self->ledger);
  h2_pal_mem_free(self->runtime->mem, self);
  *execution = NULL;
  return H2_PAL_OK;
}

int h2_gizclaw_e2e_execution_create(
    h2_runtime_t* runtime, const h2_gizclaw_e2e_config_t* config,
    const h2_gizclaw_e2e_execution_config_t* evidence,
    h2_gizclaw_e2e_execution_t** out_execution) {
  if (out_execution == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_execution = NULL;
  if (runtime == NULL || config == NULL || evidence == NULL ||
      runtime->mem == NULL || evidence->output == NULL ||
      evidence->output->vtable == NULL ||
      evidence->output->vtable->write == NULL ||
      !marker_identity_valid(evidence->board) ||
      !marker_identity_valid(evidence->platform) ||
      config->suites != H2_GIZCLAW_E2E_SUITE_ALL ||
      config->on_evidence != NULL || config->server_endpoint.data == NULL ||
      config->server_endpoint.len == 0u ||
      config->server_endpoint.len >= H2_GIZCLAW_E2E_ENDPOINT_CAPACITY)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_e2e_execution_t* self = h2_pal_mem_alloc(
      runtime->mem, sizeof(*self) + H2_GIZCLAW_E2E_LEDGER_CAPACITY);
  if (self == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(self, 0, sizeof(*self));
  self->runtime = runtime;
  self->config = *config;
  self->evidence = *evidence;
  self->confirm_rc = H2_PAL_ERR_INVALID_STATE;
  self->exit_code = H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  *out_execution = self;
  int rc = h2_gizclaw_e2e_ledger_init(&self->ledger, (char*)(self + 1),
                                      H2_GIZCLAW_E2E_LEDGER_CAPACITY);
  uint8_t nonce[16];
  if (rc == H2_PAL_OK)
    rc = h2_pal_crypto_random(runtime->crypto, nonce, sizeof(nonce));
  if (rc == H2_PAL_OK)
    rc = h2_pal_firmware_info_get_current(runtime->firmware_info,
                                          &self->firmware);
  if (rc == H2_PAL_OK) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < sizeof(nonce); ++i) {
      self->execution[i * 2u] = hex[nonce[i] >> 4u];
      self->execution[i * 2u + 1u] = hex[nonce[i] & 15u];
    }
    if (h2_atomic_init(&self->capture_failed, false) != H2_ATOMIC_OK)
      rc = H2_PAL_ERR_NO_MEMORY;
    else
      self->capture_initialized = true;
  }
  const h2_pal_mutex_config_t capture_config = {.name = "gizclaw-e2e-evidence",
                                                .allocator = runtime->mem};
  const h2_pal_mutex_config_t output_config = {.name = "gizclaw-e2e-output",
                                               .allocator = runtime->mem};
  if (rc == H2_PAL_OK)
    rc = h2_pal_mutex_create(runtime->sync, &capture_config,
                             &self->capture_mutex);
  if (rc == H2_PAL_OK)
    rc =
        h2_pal_mutex_create(runtime->sync, &output_config, &self->output_mutex);
  if (rc != H2_PAL_OK) {
    const int cleaned = h2_gizclaw_e2e_execution_destroy(out_execution);
    return cleaned == H2_PAL_OK ? rc : cleaned;
  }
  self->config.on_evidence = capture_evidence;
  self->config.evidence_user = self;
  return H2_PAL_OK;
}

h2_gizclaw_e2e_exit_t h2_gizclaw_e2e_execution_run(
    h2_gizclaw_e2e_execution_t* self) {
  if (self == NULL || self->state != EXECUTION_CREATED)
    return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  self->state = EXECUTION_RUNNING;
  char boot[256];
  const int boot_size = snprintf(
      boot, sizeof(boot), "H2_GIZCLAW_BOOT board=%s version=%s execution=%s\n",
      self->evidence.board, self->firmware.version, self->execution);
  if (boot_size <= 0 || (size_t)boot_size >= sizeof(boot) ||
      output_record(self, boot) != H2_PAL_OK) {
    h2_atomic_store_explicit(&self->capture_failed, true, H2_ATOMIC_RELEASE);
  } else {
    self->exit_code =
        h2_gizclaw_e2e_run(self->runtime, &self->config, &self->result);
  }
  const int size = snprintf(
      self->summary, sizeof(self->summary),
      "H2_GIZCLAW_E2E stage=summary platform=%s endpoint=%.*s backend=h2peer "
      "suite=all profile=%s selected=%zu terminal=%zu pass=%zu fail=%zu "
      "error=%zu blocked=%zu cancelled=%zu first_failure_case=%s "
      "first_failure_rc=%d cleanup_rc=%d retained_resources=%zu complete=%s "
      "exit_code=%d\n",
      self->evidence.platform, (int)self->config.server_endpoint.len,
      self->config.server_endpoint.data,
      self->result.runtime_profile_name[0] ? self->result.runtime_profile_name
                                           : "-",
      self->result.selected, self->result.terminal, self->result.passed,
      self->result.failed, self->result.errors, self->result.blocked,
      self->result.cancelled,
      self->result.first_failure_case[0] ? self->result.first_failure_case
                                         : "-",
      self->result.first_failure_rc, self->result.cleanup_rc,
      self->result.retained_resources, self->result.complete ? "true" : "false",
      self->exit_code);
  if (size > 0 && (size_t)size < sizeof(self->summary))
    self->summary_size = (size_t)size;
  else
    h2_atomic_store_explicit(&self->capture_failed, true, H2_ATOMIC_RELEASE);
  /* A zero-retention return joins all evidence producers. Otherwise both the
   * copied configuration and capture storage must survive retained callbacks;
   * neither final-summary append nor freezing is permitted. */
  if (self->result.retained_resources == 0u &&
      !h2_atomic_load_explicit(&self->capture_failed, H2_ATOMIC_ACQUIRE)) {
    capture_evidence(self, self->summary, self->summary_size);
    if (!h2_atomic_load_explicit(&self->capture_failed, H2_ATOMIC_ACQUIRE) &&
        h2_gizclaw_e2e_ledger_freeze(&self->ledger) != H2_PAL_OK)
      h2_atomic_store_explicit(&self->capture_failed, true, H2_ATOMIC_RELEASE);
  }
  self->state = EXECUTION_FINISHED;
  return self->exit_code;
}

bool h2_gizclaw_e2e_execution_qualified(
    const h2_gizclaw_e2e_execution_t* self) {
  return self != NULL && self->state >= EXECUTION_FINISHED &&
         self->exit_code == H2_GIZCLAW_E2E_EXIT_PASS && self->ledger.frozen &&
         !h2_atomic_load_explicit(&self->capture_failed, H2_ATOMIC_ACQUIRE) &&
         h2_gizclaw_e2e_result_all_passed(&self->result);
}

int h2_gizclaw_e2e_execution_publish(h2_gizclaw_e2e_execution_t* self,
                                     int confirm_rc) {
  if (self == NULL || self->state != EXECUTION_FINISHED ||
      (confirm_rc == H2_PAL_OK && !h2_gizclaw_e2e_execution_qualified(self)))
    return H2_PAL_ERR_INVALID_STATE;
  self->confirm_rc = confirm_rc;
  self->admitted = confirm_rc == H2_PAL_OK;
  /* Mark before writing: an output error must not resume or duplicate a partly
   * accepted summary/READY sequence. A later full replay is independently
   * framed. */
  self->state = EXECUTION_PUBLISHED;
  int rc = H2_PAL_OK;
  if (self->summary_size != 0u) rc = output_record(self, self->summary);
  if (rc != H2_PAL_OK) return rc;
  char ready[256];
  const int size =
      snprintf(ready, sizeof(ready),
               "H2_GIZCLAW_READY board=%s rc=%d confirm=%d admitted=%d\n",
               self->evidence.board, self->exit_code, self->confirm_rc,
               self->admitted ? 1 : 0);
  if (size <= 0 || (size_t)size >= sizeof(ready)) return H2_PAL_ERR_TRUNCATED;
  return output_record(self, ready);
}

int h2_gizclaw_e2e_execution_replay(h2_gizclaw_e2e_execution_t* self) {
  if (self == NULL || self->state != EXECUTION_PUBLISHED)
    return H2_PAL_ERR_INVALID_STATE;
  if (!self->ledger.frozen) return H2_PAL_OK;
  char marker[512];
  int size = snprintf(
      marker, sizeof(marker),
      "H2_GIZCLAW_LEDGER stage=begin version=%s execution=%s bytes=%zu "
      "records=%zu crc32=%08x admitted=%d confirm_rc=%d physical_audio=%d\n",
      self->firmware.version, self->execution, self->ledger.size,
      self->ledger.records, (unsigned)self->ledger.crc32,
      self->admitted ? 1 : 0, self->confirm_rc,
      self->evidence.physical_audio ? 1 : 0);
  if (size <= 0 || (size_t)size >= sizeof(marker)) return H2_PAL_ERR_TRUNCATED;
  int rc = output_record(self, marker);
  if (rc != H2_PAL_OK) return rc;
  /* Contiguous capture keeps each portable formatter record in one output call.
   * A failed call ends this replay; it never resumes a partially written
   * record. */
  for (size_t offset = 0u; offset < self->ledger.size;) {
    const char* record = self->ledger.data + offset;
    const char* end = memchr(record, '\n', self->ledger.size - offset);
    if (end == NULL) return H2_PAL_ERR_INVALID_STATE;
    const size_t count = (size_t)(end - record) + 1u;
    char line[H2_GIZCLAW_E2E_RECORD_CAPACITY];
    if (count >= sizeof(line)) return H2_PAL_ERR_TRUNCATED;
    memcpy(line, record, count);
    line[count] = '\0';
    rc = output_record(self, line);
    if (rc != H2_PAL_OK) return rc;
    offset += count;
  }
  size = snprintf(marker, sizeof(marker),
                  "H2_GIZCLAW_LEDGER stage=end execution=%s crc32=%08x\n",
                  self->execution, (unsigned)self->ledger.crc32);
  if (size <= 0 || (size_t)size >= sizeof(marker)) return H2_PAL_ERR_TRUNCATED;
  return output_record(self, marker);
}
