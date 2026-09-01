#include "h2_gizclaw_client.h"
#include "h2_gizclaw_conversation.h"
#include "h2_gizclaw_internal.h"

#include "gzc_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_MAX_PACKET_CALLS 16u

typedef struct test_audio_state {
  size_t live_allocations;
  size_t event_send_calls;
  size_t packet_calls;
  int packet_results[TEST_MAX_PACKET_CALLS];
  size_t packet_result_count;
  size_t packet_lengths[TEST_MAX_PACKET_CALLS];
  uint8_t packet_first_bytes[TEST_MAX_PACKET_CALLS];
} test_audio_state_t;

static int expect(int condition, const char *message) {
  if (condition)
    return 0;
  printf("FAIL conversation audio %s\n", message);
  return 1;
}

static void *test_alloc(void *user, size_t len) {
  test_audio_state_t *test = user;
  void *ptr = malloc(len);
  if (ptr != NULL)
    ++test->live_allocations;
  return ptr;
}

static void *test_realloc(void *user, void *ptr, size_t len) {
  test_audio_state_t *test = user;
  if (ptr == NULL)
    return test_alloc(user, len);
  if (len == 0u) {
    free(ptr);
    --test->live_allocations;
    return NULL;
  }
  return realloc(ptr, len);
}

static void test_free(void *user, void *ptr) {
  test_audio_state_t *test = user;
  if (ptr == NULL)
    return;
  free(ptr);
  --test->live_allocations;
}

static h2_pal_result_t test_time(void *user, uint64_t *out_ms) {
  (void)user;
  *out_ms = 1234u;
  return H2_PAL_OK;
}

static int test_event_send(void *user, gzc_event_stream_t *stream,
                           const gzc_peer_event_t *event) {
  test_audio_state_t *test = user;
  if (stream == NULL || event == NULL)
    return GZC_ERR_INVALID_ARGUMENT;
  ++test->event_send_calls;
  return GZC_OK;
}

static void test_event_close(void *user, gzc_event_stream_t *stream) {
  (void)user;
  (void)stream;
}

static int test_packet_send(void *user, gzc_client_t *client, uint8_t protocol,
                            const uint8_t *payload, size_t payload_len) {
  test_audio_state_t *test = user;
  (void)client;
  if (protocol != GZC_PROTOCOL_OPUS_PACKET || payload == NULL ||
      payload_len == 0u || test->packet_calls >= TEST_MAX_PACKET_CALLS) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  const size_t call = test->packet_calls++;
  test->packet_lengths[call] = payload_len;
  test->packet_first_bytes[call] = payload[0];
  return call < test->packet_result_count ? test->packet_results[call] : GZC_OK;
}

static h2_gizclaw_conversation_t *open_conversation(h2_gizclaw_client_t *client,
                                                    uint64_t generation,
                                                    int *fails) {
  h2_gizclaw_conversation_t *conversation = NULL;
  const h2_gizclaw_str_t workspace = {
      .data = "demo-audio",
      .len = 10u,
  };
  *fails +=
      expect(h2_gizclaw_conversation_open(client, workspace, generation, 1000,
                                          &conversation) == H2_PAL_OK &&
                 conversation != NULL,
             "conversation opens with a logical Event lease");
  return conversation;
}

static int test_raw_opus(h2_gizclaw_client_t *client,
                         test_audio_state_t *test) {
  int fails = 0;
  h2_gizclaw_conversation_t *conversation =
      open_conversation(client, 1u, &fails);
  const uint8_t first[] = {0x11u, 0x22u};
  const uint8_t second[] = {0x33u, 0x44u, 0x55u};
  fails += expect(
      h2_gizclaw_conversation_write_opus(conversation, first, sizeof(first),
                                         10u) == H2_PAL_OK &&
          h2_gizclaw_conversation_write_opus(
              conversation, second, sizeof(second), 20u) == H2_PAL_OK &&
          test->packet_calls == 2u &&
          test->packet_lengths[0] == sizeof(first) &&
          test->packet_lengths[1] == sizeof(second) &&
          test->packet_first_bytes[0] == first[0] &&
          test->packet_first_bytes[1] == second[0],
      "caller-encoded Opus packets are forwarded unchanged and in order");
  fails +=
      expect(h2_gizclaw_conversation_commit(conversation, 30u) == H2_PAL_OK &&
                 h2_gizclaw_conversation_commit(conversation, 30u) == H2_PAL_OK,
             "commit is idempotent after the EOS boundary is accepted");
  fails += expect(h2_gizclaw_conversation_write_opus(conversation, first,
                                                     sizeof(first), 40u) ==
                      H2_PAL_ERR_INVALID_STATE,
                  "committed conversation rejects further Opus input");
  h2_gizclaw_conversation_deinit(conversation);
  return fails;
}

static int test_backpressure(h2_gizclaw_client_t *client,
                             test_audio_state_t *test) {
  int fails = 0;
  h2_gizclaw_conversation_t *conversation =
      open_conversation(client, 2u, &fails);
  const uint8_t packet[] = {0x71u, 0x72u, 0x73u};
  const size_t first_call = test->packet_calls;
  test->packet_results[first_call] = GZC_ERR_WOULD_BLOCK;
  test->packet_result_count = first_call + 1u;
  fails += expect(h2_gizclaw_conversation_write_opus(conversation, packet,
                                                     sizeof(packet), 0u) ==
                      H2_PAL_ERR_WOULD_BLOCK,
                  "transport backpressure rejects rather than retains input");
  fails += expect(h2_gizclaw_conversation_write_opus(
                      conversation, packet, sizeof(packet), 0u) == H2_PAL_OK &&
                      test->packet_calls == first_call + 2u &&
                      test->packet_lengths[first_call] == sizeof(packet) &&
                      test->packet_lengths[first_call + 1u] == sizeof(packet) &&
                      test->packet_first_bytes[first_call] == packet[0] &&
                      test->packet_first_bytes[first_call + 1u] == packet[0],
                  "caller can retry the identical packet after transport "
                  "progress");
  h2_gizclaw_conversation_deinit(conversation);
  return fails;
}

static int test_validation(h2_gizclaw_client_t *client) {
  int fails = 0;
  h2_gizclaw_conversation_t *conversation =
      open_conversation(client, 3u, &fails);
  uint8_t packet[H2_GIZCLAW_CONVERSATION_OPUS_MAX_BYTES + 1u] = {0x55u};
  fails += expect(
      h2_gizclaw_conversation_write_opus(NULL, packet, 1u, 0u) ==
              H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_conversation_write_opus(conversation, NULL, 1u, 0u) ==
              H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_conversation_write_opus(conversation, packet, 0u, 0u) ==
              H2_PAL_ERR_INVALID_ARG &&
          h2_gizclaw_conversation_write_opus(conversation, packet,
                                             sizeof(packet),
                                             0u) == H2_PAL_ERR_INVALID_ARG,
      "raw Opus input validates pointer and packet bounds");
  h2_gizclaw_conversation_cancel(conversation);
  fails += expect(h2_gizclaw_conversation_write_opus(
                      conversation, packet, 1u, 0u) == H2_PAL_ERR_INVALID_STATE,
                  "canceled conversation rejects further Opus input");
  h2_gizclaw_conversation_deinit(conversation);
  return fails;
}

int h2_gizclaw_conversation_audio_tests(void) {
  int fails = 0;
  fails += expect(h2_gizclaw_test_audio_rings(),
                  "SPSC PCM and packet rings preserve wrap order and bounds");
  test_audio_state_t test = {0};
  const h2_pal_mem_vtable_t mem_vtable = {
      .alloc = test_alloc,
      .realloc = test_realloc,
      .free = test_free,
  };
  const h2_pal_mem_api_t mem = {.user = &test, .vtable = &mem_vtable};
  const h2_pal_time_vtable_t time_vtable = {
      .get_monotonic_ms = test_time,
  };
  const h2_pal_time_api_t time = {.user = NULL, .vtable = &time_vtable};
  const h2_pal_http_api_t http = {0};
  const h2_pal_webrtc_api_t webrtc = {0};
  const h2_pal_crypto_api_t crypto = {0};
  const h2_gizclaw_config_t config = {
      .server_endpoint = {.data = "127.0.0.1:19820", .len = 15u},
      .private_key = {.data = "test-private-key", .len = 16u},
      .cipher_mode = H2_GIZCLAW_CIPHER_CHACHA20_POLY1305,
      .connect_timeout_ms = 1000,
      .allocator = &mem,
      .http = &http,
      .webrtc = &webrtc,
      .crypto = &crypto,
      .time = &time,
  };
  h2_gizclaw_client_t *client = NULL;
  fails += expect(h2_gizclaw_client_init(&config, &client) == H2_PAL_OK &&
                      client != NULL,
                  "audio tests initialize a client");
  h2_gizclaw_test_set_event_ops(test_event_send, NULL, test_event_close, &test);
  h2_gizclaw_test_replace_event_stream(client,
                                       (gzc_event_stream_t *)(uintptr_t)0x585u);
  h2_gizclaw_test_set_conversation_ops(test_packet_send, &test);

  fails += test_raw_opus(client, &test);
  fails += test_backpressure(client, &test);
  fails += test_validation(client);

  h2_gizclaw_client_deinit(client);
  h2_gizclaw_test_set_conversation_ops(NULL, NULL);
  h2_gizclaw_test_set_event_ops(NULL, NULL, NULL, NULL);
  fails += expect(test.live_allocations == 0u,
                  "conversation and client teardown release all PAL memory");
  if (fails == 0)
    printf("PASS h2_gizclaw_conversation_audio\n");
  return fails;
}
