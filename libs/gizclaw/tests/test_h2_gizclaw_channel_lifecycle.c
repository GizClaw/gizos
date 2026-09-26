#define _POSIX_C_SOURCE 200809L

#include "h2_atomic.h"
#include "h2_desktop_platform.h"
#include "h2_gizclaw_internal.h"
#include "h2_webrtc_compat_factory.h"
#include "h2_webrtc_pion_fixture.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum { HELD_CHANNELS = 6, SOAK_CYCLES = 180 };

static volatile sig_atomic_t fixture_pid;

static void stop_on_failure(int signal_number) {
  (void)signal_number;
  if (fixture_pid > 0) {
    (void)kill((pid_t)fixture_pid, SIGKILL);
    (void)waitpid((pid_t)fixture_pid, NULL, 0);
  }
  _Exit(1);
}

typedef struct lifecycle {
  const gzc_webrtc_vtable_t *api;
  gzc_rtc_peer_t *peer;
  char offer[16384];
  size_t offer_len;
  unsigned opened, closed, remote, messages;
  unsigned reused_sids;
  bool seen_sids[300];
} lifecycle_t;

typedef struct allocation_state {
  h2_atomic_size_t live;
  h2_atomic_bool_t fail_realloc;
} allocation_state_t;

static void *allocate(void *user, size_t size) {
  allocation_state_t *state = user;
  void *p = malloc(size);
  if (p != NULL)
    h2_atomic_fetch_add(&state->live, 1);
  return p;
}

static void *resize(void *user, void *p, size_t size) {
  allocation_state_t *state = user;
  if (h2_atomic_exchange(&state->fail_realloc, false))
    return NULL;
  bool fresh = p == NULL;
  void *replacement = realloc(p, size);
  if (replacement != NULL && fresh)
    h2_atomic_fetch_add(&state->live, 1);
  return replacement;
}

static void release(void *user, void *p) {
  allocation_state_t *state = user;
  if (p != NULL) {
    h2_atomic_fetch_sub(&state->live, 1);
    free(p);
  }
}

static uint64_t now_ms(void) {
  uint64_t value = 0;
  assert(h2_pal_time_get_monotonic_ms(h2_desktop_platform_time_api(), &value) ==
         H2_PAL_OK);
  return value;
}

static void local_sdp(void *user, gzc_rtc_peer_t *peer, gzc_rtc_sdp_type_t type,
                      gzc_str_t sdp) {
  lifecycle_t *test = user;
  assert(peer == test->peer && type == GZC_RTC_SDP_OFFER);
  assert(sdp.len < sizeof(test->offer));
  memcpy(test->offer, sdp.data, sdp.len);
  test->offer_len = sdp.len;
}

static void channel_state(void *user, gzc_rtc_peer_t *peer,
                          gzc_rtc_channel_t *channel,
                          const gzc_rtc_channel_info_t *info,
                          gzc_rtc_channel_state_t state) {
  lifecycle_t *test = user;
  (void)channel;
  assert(peer == test->peer && info != NULL);
  if (state == GZC_RTC_CHANNEL_OPEN) {
    ++test->opened;
    assert(info->stream_id <
           sizeof(test->seen_sids) / sizeof(test->seen_sids[0]));
    if (test->seen_sids[info->stream_id])
      ++test->reused_sids;
    test->seen_sids[info->stream_id] = true;
  } else {
    assert(state == GZC_RTC_CHANNEL_CLOSED);
    ++test->closed;
  }
}

static void remote_channel(void *user, gzc_rtc_peer_t *peer,
                           gzc_rtc_channel_t *channel,
                           const gzc_rtc_channel_info_t *info) {
  lifecycle_t *test = user;
  (void)peer;
  (void)channel;
  (void)info;
  ++test->remote;
}

static void channel_message(void *user, gzc_rtc_peer_t *peer,
                            gzc_rtc_channel_t *channel,
                            const gzc_rtc_channel_info_t *info,
                            const uint8_t *data, size_t len, bool text) {
  lifecycle_t *test = user;
  (void)peer;
  (void)channel;
  (void)info;
  static const char expected[] = "server-service-ack-text:echo";
  assert(text && len == sizeof(expected) - 1 &&
         memcmp(data, expected, len) == 0);
  ++test->messages;
}

static int wait_count(lifecycle_t *test, const unsigned *count,
                      unsigned expected, uint64_t timeout_ms) {
  uint64_t deadline = now_ms() + timeout_ms;
  while (*count < expected && now_ms() < deadline)
    if (test->api->peer_poll(test->peer, 10) != GZC_OK)
      return -1;
  return *count >= expected ? 0 : -1;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  h2_webrtc_pion_fixture_t fixture = {0};
  assert(h2_webrtc_pion_fixture_start(&fixture, argv[1], "udp") == 0);
  fixture_pid = (sig_atomic_t)fixture.pid;
  (void)signal(SIGABRT, stop_on_failure);
  (void)signal(SIGALRM, stop_on_failure);
  (void)alarm(60);
  h2_webrtc_compat_backend_t backend = {0};
  assert(h2_webrtc_compat_backend_create(&backend) == H2_PAL_OK);
  const h2_pal_http_api_t http = {0};
  const h2_pal_crypto_api_t crypto = {0};
  allocation_state_t allocations = {0};
  assert(h2_atomic_size_init(&allocations.live, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_bool_init(&allocations.fail_realloc, false) == H2_ATOMIC_OK);
  const h2_pal_mem_vtable_t memory_vtable = {
      .alloc = allocate, .realloc = resize, .free = release};
  const h2_pal_mem_api_t memory = {&allocations, &memory_vtable};
  const h2_gizclaw_config_t config = {
      .server_endpoint = {"127.0.0.1:1", 11},
      .private_key = {"test", 4},
      .connect_timeout_ms = 1000,
      .allocator = &memory,
      .http = &http,
      .crypto = &crypto,
      .webrtc = backend.api,
      .time = h2_desktop_platform_time_api(),
      .log = h2_desktop_platform_log_api(),
  };
  h2_gizclaw_client_t *client = NULL;
  assert(h2_gizclaw_client_init(&config, &client) == H2_PAL_OK);
  lifecycle_t test = {.api = h2_gizclaw_test_webrtc_api(client)};
  const gzc_webrtc_callbacks_t callbacks = {
      .userdata = &test,
      .on_local_sdp = local_sdp,
      .on_channel_state = channel_state,
      .on_channel_message = channel_message,
      .on_remote_channel = remote_channel,
  };
  assert(test.api->peer_create(test.api->userdata, &callbacks, &test.peer) ==
         GZC_OK);
  const gzc_rtc_channel_config_t channel_config = {
      .label = {"giznet/v1/service/0", sizeof("giznet/v1/service/0") - 1},
      .ordered = true,
      .reliable = true};
  gzc_rtc_channel_t *rejected = (void *)(uintptr_t)1;
  h2_atomic_store(&allocations.fail_realloc, true);
  assert(test.api->peer_create_data_channel(test.peer, &channel_config,
                                            &rejected) == GZC_ERR_NO_MEMORY);
  assert(rejected == NULL);
  gzc_rtc_channel_t *held[HELD_CHANNELS] = {0};
  for (unsigned i = 0; i < HELD_CHANNELS; ++i)
    assert(test.api->peer_create_data_channel(test.peer, &channel_config,
                                              &held[i]) == GZC_OK);
  assert(test.api->peer_start_offer(test.peer) == GZC_OK);
  uint64_t deadline = now_ms() + 10000;
  while (test.offer_len == 0 && now_ms() < deadline)
    assert(test.api->peer_poll(test.peer, 10) == GZC_OK);
  assert(test.offer_len != 0);
  char answer[16384];
  size_t answer_len = 0;
  assert(h2_webrtc_pion_fixture_exchange_performance(
             &fixture, (h2_pal_webrtc_str_t){test.offer, test.offer_len},
             answer, sizeof(answer), &answer_len) == 0);
  assert(test.api->peer_set_remote_sdp(test.peer, GZC_RTC_SDP_ANSWER,
                                       (gzc_str_t){answer, answer_len}) ==
         GZC_OK);
  assert(wait_count(&test, &test.opened, HELD_CHANNELS, 10000) == 0);

  unsigned completed = 0, failed_closes = 0, failed_sends = 0;
  int result = GZC_OK;
  for (; completed < SOAK_CYCLES; ++completed) {
    gzc_rtc_channel_t *channel = NULL;
    result = test.api->peer_create_data_channel(test.peer, &channel_config,
                                                &channel);
    if (result != GZC_OK)
      break;
    if ((completed & 1u) == 0) {
      unsigned messages = test.messages;
      unsigned opened = test.opened;
      assert(wait_count(&test, &test.opened, opened + 1, 10000) == 0);
      int send =
          test.api->channel_send(channel, (const uint8_t *)"echo", 4, true);
      if (send != GZC_OK)
        ++failed_sends;
      else
        assert(wait_count(&test, &test.messages, messages + 1, 10000) == 0);
    }
    /* Odd cycles cancel immediately, before dispatching DCEP OPEN. The
     * consumed alias must still close the PAL channel exactly once. */
    unsigned closed = test.closed;
    test.api->channel_close(channel);
    if (wait_count(&test, &test.closed, closed + 1, 250) != 0)
      ++failed_closes;
  }
  h2_webrtc_channel_stats_t stats = {0};
  deadline = now_ms() + 3000;
  do {
    assert(test.api->peer_poll(test.peer, 10) == GZC_OK);
    assert(h2_webrtc_pion_fixture_channel_stats(&fixture, &stats) == 0);
  } while (stats.current != HELD_CHANNELS && now_ms() < deadline);
  fprintf(
      stderr,
      "channel lifecycle cycles=%u rc=%d failed_closes=%u "
      "failed_sends=%u unexpected_remote=%u peer_current=%llu reused_sids=%u\n",
      completed, result, failed_closes, failed_sends, test.remote,
      stats.current, test.reused_sids);
  assert(completed == SOAK_CYCLES && result == GZC_OK && failed_closes == 0 &&
         failed_sends == 0 && test.remote == 0 &&
         stats.current == HELD_CHANNELS && test.reused_sids > 0);
  unsigned messages = test.messages;
  assert(test.api->channel_send(held[0], (const uint8_t *)"echo", 4, true) ==
         GZC_OK);
  assert(wait_count(&test, &test.messages, messages + 1, 10000) == 0);
  for (unsigned i = 0; i < HELD_CHANNELS; ++i)
    test.api->channel_close(held[i]);
  assert(wait_count(&test, &test.closed, SOAK_CYCLES + HELD_CHANNELS, 10000) ==
         0);
  test.api->peer_close(test.peer);
  h2_gizclaw_client_deinit(client);
  backend.destroy(backend.state);
  h2_webrtc_pion_fixture_stop(&fixture);
  fixture_pid = 0;
  (void)alarm(0);
  assert(h2_atomic_load(&allocations.live) == 0);
  h2_atomic_size_destroy(&allocations.live);
  h2_atomic_bool_destroy(&allocations.fail_realloc);
  return 0;
}
