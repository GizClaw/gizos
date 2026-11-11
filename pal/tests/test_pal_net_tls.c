#include "net/h2_pal_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int condition, const char* expression, int line) {
  if (!condition) {
    fprintf(stderr, "Net TLS contract failed at line %d: %s\n", line,
            expression);
    abort();
  }
}
#define CHECK(condition) check((condition), #condition, __LINE__)

typedef struct tls_probe {
  unsigned legacy_calls;
  unsigned bounded_calls;
  int stall_legacy;
  h2_pal_result_t bounded_result;
  h2_pal_net_socket_t socket;
  const h2_pal_net_tls_config_t* config;
  uint32_t timeout_ms;
} tls_probe_t;

static h2_pal_result_t legacy_wrap(void* user, h2_pal_net_socket_t socket,
                                   const h2_pal_net_tls_config_t* config,
                                   uint32_t timeout_ms,
                                   h2_pal_net_socket_t* out_socket) {
  tls_probe_t* probe = user;
  ++probe->legacy_calls;
  probe->socket = socket;
  probe->config = config;
  probe->timeout_ms = timeout_ms;
  if (probe->stall_legacy) {
    /* The subprocess owner holds stdin open and releases this opaque wait
     * only after it has observed a bounded-call failure. */
    puts("LEGACY_ENTERED");
    CHECK(fflush(stdout) == 0);
    (void)getchar();
  }
  *out_socket = socket + 1;
  return H2_PAL_OK;
}

static h2_pal_result_t bounded_wrap(void* user, h2_pal_net_socket_t socket,
                                    const h2_pal_net_tls_config_t* config,
                                    uint32_t timeout_ms,
                                    h2_pal_net_socket_t* out_socket) {
  tls_probe_t* probe = user;
  ++probe->bounded_calls;
  probe->socket = socket;
  probe->config = config;
  probe->timeout_ms = timeout_ms;
  if (probe->bounded_result != H2_PAL_OK) return probe->bounded_result;
  *out_socket = socket + 1;
  return H2_PAL_OK;
}

static void test_dispatch(void) {
  tls_probe_t probe = {0};
  h2_pal_net_vtable_t vtable = {.tls_wrap = legacy_wrap};
  h2_pal_net_api_t api = {.vtable = &vtable, .user = &probe};
  h2_pal_net_tls_config_t config = {0};
  h2_pal_net_socket_t out = 123;

  CHECK(h2_pal_net_tls_wrap_bounded(&api, 7, &config, 25u, &out) ==
        H2_PAL_ERR_UNSUPPORTED);
  CHECK(out == -1 && probe.legacy_calls == 0u && probe.bounded_calls == 0u);

  CHECK(h2_pal_net_tls_wrap(&api, 7, &config, 25u, &out) == H2_PAL_OK);
  CHECK(out == 8 && probe.legacy_calls == 1u && probe.bounded_calls == 0u);
  CHECK(probe.socket == 7 && probe.config == &config &&
        probe.timeout_ms == 25u);

  probe = (tls_probe_t){.bounded_result = H2_PAL_ERR_UNSUPPORTED};
  vtable.tls_wrap_bounded = bounded_wrap;
  out = 123;
  CHECK(h2_pal_net_tls_wrap_bounded(&api, 7, &config, 25u, &out) ==
        H2_PAL_ERR_UNSUPPORTED);
  CHECK(out == -1 && probe.legacy_calls == 0u && probe.bounded_calls == 1u);

  probe = (tls_probe_t){0};
  CHECK(h2_pal_net_tls_wrap_bounded(&api, 7, &config, 25u, &out) == H2_PAL_OK);
  CHECK(out == 8 && probe.legacy_calls == 0u && probe.bounded_calls == 1u);
  CHECK(probe.socket == 7 && probe.config == &config &&
        probe.timeout_ms == 25u);

  api.vtable = NULL;
  out = 123;
  CHECK(h2_pal_net_tls_wrap_bounded(&api, 7, &config, 25u, &out) ==
        H2_PAL_ERR_UNSUPPORTED);
  CHECK(out == -1);
  CHECK(h2_pal_net_tls_wrap_bounded(NULL, 7, &config, 25u, &out) ==
        H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_net_tls_wrap_bounded(&api, 7, NULL, 25u, &out) ==
        H2_PAL_ERR_INVALID_ARG);
  CHECK(h2_pal_net_tls_wrap_bounded(&api, 7, &config, 25u, NULL) ==
        H2_PAL_ERR_INVALID_ARG);
}

int main(int argc, char** argv) {
  if (argc == 1) {
    test_dispatch();
    return 0;
  }
  CHECK(argc == 2);
  tls_probe_t probe = {.stall_legacy = 1};
  h2_pal_net_vtable_t vtable = {.tls_wrap = legacy_wrap};
  h2_pal_net_api_t api = {.vtable = &vtable, .user = &probe};
  h2_pal_net_tls_config_t config = {0};
  h2_pal_net_socket_t out = 123;
  if (strcmp(argv[1], "ordinary") == 0) {
    CHECK(h2_pal_net_tls_wrap(&api, 7, &config, 25u, &out) == H2_PAL_OK);
    CHECK(out == 8 && probe.legacy_calls == 1u && probe.bounded_calls == 0u);
    return 0;
  }
  if (strcmp(argv[1], "rejected") == 0) {
    vtable.tls_wrap_bounded = bounded_wrap;
    probe.bounded_result = H2_PAL_ERR_UNSUPPORTED;
  } else {
    CHECK(strcmp(argv[1], "missing") == 0);
  }
  CHECK(h2_pal_net_tls_wrap_bounded(&api, 7, &config, 25u, &out) ==
        H2_PAL_ERR_UNSUPPORTED);
  CHECK(out == -1 && probe.legacy_calls == 0u);
  CHECK(probe.bounded_calls == (vtable.tls_wrap_bounded != NULL ? 1u : 0u));
  return 0;
}
