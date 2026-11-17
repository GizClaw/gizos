#ifndef H2_IPERF_CLIENT_OUTPUT_H
#define H2_IPERF_CLIENT_OUTPUT_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Caller-owned output for one run. The stream, fallback Log, Memory and target
 * remain borrowed until all App logging stops. Iperf records and lifecycle
 * calls are serialized on the App's calling task; other scopes use fallback.
 * Physical stream configuration and management handoff belong to the target. */
typedef struct h2_iperf_client_output {
  const h2_pal_io_stream_api_t* stream;
  const h2_pal_log_api_t* fallback;
  const h2_pal_mem_api_t* mem;
  const char* target;
  h2_pal_log_api_t log;
  h2_pal_result_t error;
  bool active;
} h2_iperf_client_output_t;

void h2_iperf_client_output_init(h2_iperf_client_output_t* output,
                                 const h2_runtime_config_t* config,
                                 const char* target);
/* Write one complete CRLF record through IO Stream PAL with a 5-second budget.
 * Malformed, partial and failed writes latch the first error. No suffix retry.
 */
h2_pal_result_t h2_iperf_client_output_record(h2_iperf_client_output_t* output,
                                              const char* message);
void h2_iperf_client_output_checkpoint(void* user, const char* checkpoint);

#ifdef __cplusplus
}
#endif
#endif
