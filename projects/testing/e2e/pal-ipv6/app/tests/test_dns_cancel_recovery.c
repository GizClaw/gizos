#include "h2_pal_ipv6_e2e.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

/* Model a bounded provider whose canceled jobs finish only when scheduled.
 * Keep the real IPv6 App case and public resolver calls; unrelated raw Net/TLS
 * cases are replaced so this test needs no sockets or external fixtures. */
typedef struct fixture {
  uint64_t now_ms;
  unsigned canceled, pending, polls, sleeps;
  int active, completed, stalled, poll_failure;
  uint32_t recovery_poll_budget;
} fixture_t;

int h2_test_transport_run(const h2_net_tls_config_t* config,
                          h2_net_tls_result_t* out) {
  (void)config;
  memset(out, 0, sizeof(*out));
  for (unsigned i = 0; i < H2_PAL_IPV6_CASES - H2_PAL_IPV6_EXTRA_CASES; ++i) {
    out->cases[i].id = "transport-fixture";
    out->cases[i].mandatory = 1;
    out->cases[i].passed = 1;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t resolve_all(void* user, const char* host,
                                   h2_pal_net_family_t family,
                                   h2_pal_net_addr_list_t* out) {
  (void)user;
  (void)host;
  memset(out, 0, sizeof(*out));
  if (family != H2_PAL_NET_FAMILY_ANY && family != H2_PAL_NET_FAMILY_IPV4 &&
      family != H2_PAL_NET_FAMILY_IPV6)
    return H2_PAL_ERR_INVALID_ARG;
  if (family != H2_PAL_NET_FAMILY_IPV6) {
    out->addrs[out->count].family = H2_PAL_NET_FAMILY_IPV4;
    out->addrs[out->count].ip[0] = 127;
    out->addrs[out->count++].ip[3] = 1;
  }
  if (family != H2_PAL_NET_FAMILY_IPV4) {
    out->addrs[out->count].family = H2_PAL_NET_FAMILY_IPV6;
    out->addrs[out->count++].ip[15] = 1;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t start(void* user, const char* host,
                             h2_pal_net_family_t family,
                             h2_pal_net_resolver_t** out) {
  fixture_t* f = user;
  assert(strcmp(host, "::1") == 0 && family == H2_PAL_NET_FAMILY_IPV6);
  *out = NULL;
  assert(!f->active);
  if (f->pending == 16u) return H2_PAL_ERR_NO_SPACE;
  f->active = 1;
  f->completed = 0;
  *out = (h2_pal_net_resolver_t*)f;
  return H2_PAL_OK;
}

static h2_pal_result_t poll_all(void* user, h2_pal_net_resolver_t* resolver,
                                h2_pal_net_addr_list_t* out,
                                uint32_t timeout_ms) {
  fixture_t* f = user;
  assert(resolver == (h2_pal_net_resolver_t*)f && f->active);
  ++f->polls;
  f->completed = 1;
  if (f->canceled == 32u) {
    /* The probe must use the original cancellation budget, not a fresh 5s. */
    assert(timeout_ms == 5000u - f->now_ms);
    f->recovery_poll_budget = timeout_ms;
    if (f->poll_failure) return H2_PAL_ERR_IO;
  }
  return resolve_all(user, "::1", H2_PAL_NET_FAMILY_IPV6, out);
}

static void close_resolver(void* user, h2_pal_net_resolver_t* resolver) {
  fixture_t* f = user;
  if (!resolver) return;
  assert(resolver == (h2_pal_net_resolver_t*)f && f->active);
  f->active = 0;
  if (!f->completed) {
    ++f->canceled;
    ++f->pending;
  }
}

static h2_pal_result_t now_ms(void* user, uint64_t* out) {
  *out = ((fixture_t*)user)->now_ms;
  return H2_PAL_OK;
}
static int sleep_ms(void* user, uint32_t ms) {
  fixture_t* f = user;
  assert(ms > 0u && ms <= 10u);
  f->now_ms += ms;
  ++f->sleeps;
  if (f->pending && (!f->stalled || f->canceled < 32u)) --f->pending;
  return H2_PAL_OK;
}

static void run_case(int stalled, int poll_failure) {
  fixture_t f = {.stalled = stalled, .poll_failure = poll_failure};
  const h2_pal_net_vtable_t net_ops = {.resolve_all = resolve_all,
                                       .resolve_start_family = start,
                                       .resolve_poll_all = poll_all,
                                       .resolve_close = close_resolver};
  const h2_pal_net_api_t net = {&f, &net_ops};
  const h2_pal_time_vtable_t time_ops = {.get_monotonic_ms = now_ms,
                                         .sleep_ms = sleep_ms};
  const h2_pal_time_api_t time = {&f, &time_ops};
  const h2_runtime_t runtime = {.net = &net, .time = &time};
  const h2_pal_ipv6_config_t config = {
      .transport = {.runtime = &runtime, .session = "cancel-recovery"}};
  h2_pal_ipv6_result_t result = {0};
  (void)h2_pal_ipv6_e2e_run(&config, &result);
  const h2_net_tls_case_result_t* cancellation = NULL;
  for (unsigned i = 0; i < H2_PAL_IPV6_CASES; ++i)
    if (result.cases[i].id &&
        strcmp(result.cases[i].id, "dns-family-cancel") == 0)
      cancellation = &result.cases[i];
  assert(cancellation && f.canceled == 32u && !f.active);
  if (stalled) {
    assert(!cancellation->passed && cancellation->detail == H2_PAL_ERR_TIMEOUT);
    assert(f.now_ms == 5000u && f.polls == 1u);
  } else if (poll_failure) {
    assert(!cancellation->passed && cancellation->detail == H2_PAL_ERR_IO);
    assert(f.polls == 2u && f.pending < 16u);
  } else {
    assert(cancellation->passed && f.pending < 16u);
    assert(f.polls == 2u && f.recovery_poll_budget < 5000u);
    assert(f.sleeps > 0u);
  }
}

int main(void) {
  run_case(0, 0);
  run_case(1, 0);
  run_case(0, 1);
  return 0;
}
