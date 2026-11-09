#ifndef H2_COREHTTP_TEST_SHADOW_H
#define H2_COREHTTP_TEST_SHADOW_H

/* Host-test shadow of pal/providers/backends/corehttp/include/h2_corehttp.h.
 * It declares only what h2_esp_board_runtime.c consumes, so the runtime
 * teardown path can be linked and observed without the real provider. */

#include "application/h2_pal_http.h"
#include "net/h2_pal_net.h"
#include "os/h2_pal_log.h"
#include "os/h2_pal_mem.h"
#include "os/h2_pal_time.h"

typedef struct h2_corehttp h2_corehttp_t;

typedef struct h2_corehttp_config {
  const h2_pal_mem_api_t* allocator;
  const h2_pal_net_api_t* net;
  const h2_pal_time_api_t* time;
  const h2_pal_log_api_t* log;
} h2_corehttp_config_t;

h2_pal_result_t h2_corehttp_create(const h2_corehttp_config_t* config,
                                   h2_corehttp_t** out_http,
                                   h2_pal_http_api_t* out_api);

void h2_corehttp_destroy(h2_corehttp_t* http);

#endif
