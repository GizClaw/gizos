#include "h2_iperf_client_output.h"

#include <stdio.h>
#include <string.h>

static h2_pal_result_t latch(h2_iperf_client_output_t* output,
                             h2_pal_result_t rc) {
  if (rc != H2_PAL_OK && output->error == H2_PAL_OK) output->error = rc;
  return rc;
}

h2_pal_result_t h2_iperf_client_output_record(h2_iperf_client_output_t* output,
                                              const char* message) {
  if (output == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!output->active) return latch(output, H2_PAL_ERR_INVALID_STATE);
  if (message == NULL) return latch(output, H2_PAL_ERR_INVALID_ARG);
  char line[1024];
  size_t length = strlen(message);
  if (length == 0u || length > sizeof(line) - 2u || strchr(message, '\r') ||
      strchr(message, '\n'))
    return latch(output, H2_PAL_ERR_INVALID_ARG);
  memcpy(line, message, length);
  line[length++] = '\r';
  line[length++] = '\n';
  size_t written = 0u;
  h2_pal_result_t rc =
      h2_pal_io_stream_write(output->stream, line, length, &written, 5000u);
  if (rc == H2_PAL_OK && written != length) rc = H2_PAL_ERR_IO;
  return latch(output, rc);
}

static h2_pal_result_t write_log(void* user, h2_pal_log_level_t level,
                                 const char* scope, const char* message) {
  h2_iperf_client_output_t* output = user;
  if (scope != NULL && strncmp(scope, "iperf", 5u) == 0)
    return h2_iperf_client_output_record(output, message);
  return h2_pal_log_write(output->fallback, level, scope, message);
}

void h2_iperf_client_output_init(h2_iperf_client_output_t* output,
                                 const h2_runtime_config_t* config,
                                 const char* target) {
  static const h2_pal_log_vtable_t vtable = {.write = write_log};
  *output = (h2_iperf_client_output_t){
      .stream = config->io_stream,
      .fallback = config->log,
      .mem = config->mem,
      .target = target,
      .log = {.user = output, .vtable = &vtable},
  };
}

void h2_iperf_client_output_checkpoint(void* user, const char* checkpoint) {
  h2_iperf_client_output_t* output = user;
  h2_pal_mem_stats_t internal = {0}, external = {0};
  const h2_pal_result_t internal_rc =
      h2_pal_mem_get_stats(output->mem, H2_PAL_MEM_REGION_INTERNAL, &internal);
  const h2_pal_result_t external_rc =
      h2_pal_mem_get_stats(output->mem, H2_PAL_MEM_REGION_EXTERNAL, &external);
  char line[320];
  int size = snprintf(
      line, sizeof(line),
      "H2_IPERF_CLIENT_MEMORY board=%s checkpoint=%s internal_free=%zu "
      "internal_min=%zu psram_free=%zu internal_stats_rc=%d external_stats_rc=%d",
      output->target, checkpoint, internal.free_bytes,
      internal.minimum_free_bytes, external.free_bytes, internal_rc,
      external_rc);
  if (size <= 0 || (size_t)size >= sizeof(line)) {
    (void)latch(output, H2_PAL_ERR_NO_SPACE);
    return;
  }
  (void)h2_iperf_client_output_record(output, line);
}
