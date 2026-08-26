#include "h2_gizclaw_client.h"
#include "h2_gizclaw_conversation.h"
#include "h2_gizclaw_internal.h"

#include "gzc_common.h"
#include "opus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_MAX_ENCODE_CALLS 32u
#define TEST_MAX_FRAME_VALUES 640u
#define TEST_MAX_PACKET_CALLS 64u

typedef struct test_audio_state {
  size_t allocation_calls;
  size_t fail_allocation_call;
  size_t live_allocations;
  size_t event_send_calls;
  size_t encode_calls;
  size_t encode_fail_call;
  size_t encoded_value_counts[TEST_MAX_ENCODE_CALLS];
  int16_t encoded_pcm[TEST_MAX_ENCODE_CALLS][TEST_MAX_FRAME_VALUES];
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
  ++test->allocation_calls;
  if (test->fail_allocation_call == test->allocation_calls)
    return NULL;
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
  ++test->allocation_calls;
  if (test->fail_allocation_call == test->allocation_calls)
    return NULL;
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

static int test_encode(void *user, void *encoder, const int16_t *pcm,
                       int frame_samples, uint8_t *opus, int opus_capacity) {
  test_audio_state_t *test = user;
  (void)encoder;
  const size_t call = test->encode_calls++;
  if (test->encode_fail_call != 0u &&
      test->encode_calls == test->encode_fail_call) {
    return OPUS_INTERNAL_ERROR;
  }
  if (call >= TEST_MAX_ENCODE_CALLS || frame_samples <= 0 ||
      (size_t)frame_samples > TEST_MAX_FRAME_VALUES || opus_capacity < 1) {
    return OPUS_BAD_ARG;
  }
  test->encoded_value_counts[call] = (size_t)frame_samples;
  memcpy(test->encoded_pcm[call], pcm, (size_t)frame_samples * sizeof(int16_t));
  opus[0] = (uint8_t)(call + 1u);
  return 1;
}

static void reset_audio_calls(test_audio_state_t *test) {
  test->encode_calls = 0u;
  test->encode_fail_call = 0u;
  memset(test->encoded_value_counts, 0, sizeof(test->encoded_value_counts));
  memset(test->encoded_pcm, 0, sizeof(test->encoded_pcm));
  test->packet_calls = 0u;
  test->packet_result_count = 0u;
  memset(test->packet_results, 0, sizeof(test->packet_results));
  memset(test->packet_lengths, 0, sizeof(test->packet_lengths));
  memset(test->packet_first_bytes, 0, sizeof(test->packet_first_bytes));
}

static h2_audio_frame_t mono_frame(int16_t *samples, uint16_t count,
                                   h2_audio_pcm_format_t format) {
  h2_audio_frame_t frame = h2_audio_frame_for_buffer(
      samples, (size_t)count * sizeof(*samples), format);
  frame.bytes = frame.capacity;
  frame.samples_per_channel = count;
  return frame;
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

static int test_arbitrary_chunks(h2_gizclaw_client_t *client,
                                 test_audio_state_t *test,
                                 h2_audio_pcm_format_t format) {
  int fails = 0;
  reset_audio_calls(test);
  h2_gizclaw_test_set_conversation_ops(test_packet_send, test_encode, test);
  h2_gizclaw_conversation_t *conversation =
      open_conversation(client, 1u, &fails);
  fails += expect(h2_gizclaw_conversation_configure_pcm(conversation, &format,
                                                        0) == H2_PAL_OK,
                  "PCM mode accepts a 512-sample provider format");

  int16_t input[2560];
  for (size_t index = 0u; index < 2560u; ++index)
    input[index] = (int16_t)index;
  for (size_t chunk = 0u; chunk < 5u; ++chunk) {
    h2_audio_frame_t frame = mono_frame(input + chunk * 512u, 512u, format);
    fails += expect(h2_gizclaw_conversation_write_pcm(conversation, &frame) ==
                        H2_PAL_OK,
                    "a complete 512-sample provider chunk is consumed");
  }
  fails += expect(test->encode_calls == 8u && test->packet_calls == 8u,
                  "five 512-sample chunks emit eight 320-sample packets");
  for (size_t packet = 0u; packet < 8u; ++packet) {
    fails += expect(test->encoded_value_counts[packet] == 320u &&
                        memcmp(test->encoded_pcm[packet], input + packet * 320u,
                               320u * sizeof(int16_t)) == 0,
                    "PCM ordering is preserved across provider boundaries");
  }
  fails +=
      expect(h2_gizclaw_conversation_commit(conversation, 0u) == H2_PAL_OK &&
                 test->encode_calls == 8u && test->packet_calls == 8u,
             "an aligned commit adds no padded packet");
  fails += expect(h2_gizclaw_conversation_commit(conversation, 0u) == H2_PAL_OK,
                  "a completed commit is idempotent");
  h2_gizclaw_conversation_deinit(conversation);
  return fails;
}

static int test_padding_and_modes(h2_gizclaw_client_t *client,
                                  test_audio_state_t *test,
                                  h2_audio_pcm_format_t format) {
  int fails = 0;
  reset_audio_calls(test);
  h2_gizclaw_conversation_t *conversation =
      open_conversation(client, 2u, &fails);
  h2_audio_pcm_format_t invalid_format = format;
  invalid_format.sample_rate_hz = 44100u;
  fails +=
      expect(h2_gizclaw_conversation_configure_pcm(
                 conversation, &invalid_format, 0) == H2_PAL_ERR_UNSUPPORTED,
             "PCM configuration rejects an unsupported sample rate");
  invalid_format = format;
  invalid_format.channels = 3u;
  fails +=
      expect(h2_gizclaw_conversation_configure_pcm(
                 conversation, &invalid_format, 0) == H2_PAL_ERR_UNSUPPORTED,
             "PCM configuration rejects unsupported channels");
  invalid_format = format;
  invalid_format.sample_format = (h2_audio_sample_format_t)99;
  fails +=
      expect(h2_gizclaw_conversation_configure_pcm(
                 conversation, &invalid_format, 0) == H2_PAL_ERR_UNSUPPORTED,
             "PCM configuration rejects unsupported sample formats");
  fails += expect(h2_gizclaw_conversation_configure_pcm(
                      conversation, &format, -1) == H2_PAL_ERR_INVALID_ARG &&
                      h2_gizclaw_conversation_configure_pcm(
                          conversation, &format, 11) == H2_PAL_ERR_INVALID_ARG,
                  "PCM configuration rejects Opus complexity outside 0-10");
  fails += expect(h2_gizclaw_conversation_configure_pcm(conversation, &format,
                                                        0) == H2_PAL_OK,
                  "failed configuration leaves the conversation configurable");
  int16_t input[100];
  for (size_t index = 0u; index < 100u; ++index)
    input[index] = (int16_t)(index + 1u);
  h2_audio_frame_t frame = mono_frame(input, 100u, format);
  fails += expect(h2_gizclaw_conversation_write_pcm(conversation, &frame) ==
                      H2_PAL_OK,
                  "a partial Opus interval is retained");
  const uint8_t raw_opus[] = {0xf8u};
  fails += expect(h2_gizclaw_conversation_write_opus(conversation, raw_opus,
                                                     sizeof(raw_opus), 0u) ==
                      H2_PAL_ERR_INVALID_STATE,
                  "PCM mode rejects caller-supplied raw Opus");
  fails +=
      expect(h2_gizclaw_conversation_commit(conversation, 0u) == H2_PAL_OK &&
                 test->encode_calls == 1u && test->packet_calls == 1u,
             "commit pads one non-empty residual interval");
  fails += expect(memcmp(test->encoded_pcm[0], input, sizeof(input)) == 0,
                  "final padding preserves residual samples");
  for (size_t index = 100u; index < 320u; ++index)
    fails += expect(test->encoded_pcm[0][index] == 0,
                    "final padding contains only zero samples");
  h2_gizclaw_conversation_deinit(conversation);

  reset_audio_calls(test);
  conversation = open_conversation(client, 3u, &fails);
  fails += expect(
      h2_gizclaw_conversation_configure_pcm(conversation, &format, 0) ==
              H2_PAL_OK &&
          h2_gizclaw_conversation_commit(conversation, 0u) == H2_PAL_OK &&
          test->encode_calls == 0u && test->packet_calls == 0u,
      "empty PCM commit manufactures no packet");
  h2_gizclaw_conversation_deinit(conversation);

  reset_audio_calls(test);
  conversation = open_conversation(client, 4u, &fails);
  fails += expect(
      h2_gizclaw_conversation_write_opus(conversation, raw_opus,
                                         sizeof(raw_opus), 0u) == H2_PAL_OK &&
          h2_gizclaw_conversation_configure_pcm(conversation, &format, 0) ==
              H2_PAL_ERR_INVALID_STATE,
      "raw Opus mode rejects a later PCM configuration");
  h2_gizclaw_conversation_deinit(conversation);
  return fails;
}

static int test_validation(h2_gizclaw_client_t *client,
                           test_audio_state_t *test,
                           h2_audio_pcm_format_t format) {
  int fails = 0;
  reset_audio_calls(test);
  h2_gizclaw_conversation_t *conversation =
      open_conversation(client, 5u, &fails);
  fails += expect(h2_gizclaw_conversation_configure_pcm(conversation, &format,
                                                        0) == H2_PAL_OK,
                  "validation conversation configures PCM");
  int16_t samples[513] = {0};
  h2_audio_frame_t frame = mono_frame(samples, 320u, format);
  frame.sample_rate_hz = 8000u;
  fails += expect(h2_gizclaw_conversation_write_pcm(conversation, &frame) ==
                      H2_PAL_ERR_FORMAT,
                  "PCM write rejects format drift");
  frame = mono_frame(samples, 320u, format);
  --frame.bytes;
  fails += expect(h2_gizclaw_conversation_write_pcm(conversation, &frame) ==
                      H2_PAL_ERR_FORMAT,
                  "PCM write rejects malformed byte counts");
  frame = mono_frame(samples, 513u, format);
  fails += expect(h2_gizclaw_conversation_write_pcm(conversation, &frame) ==
                      H2_PAL_ERR_INVALID_ARG,
                  "PCM write rejects an oversized provider chunk");
  frame = mono_frame(samples, 320u, format);
  fails += expect(h2_gizclaw_conversation_write_pcm(conversation, &frame) ==
                          H2_PAL_OK &&
                      test->encode_calls == 1u,
                  "a 320-sample provider chunk emits one packet");
  h2_gizclaw_conversation_cancel(conversation);
  fails += expect(h2_gizclaw_conversation_write_pcm(conversation, &frame) ==
                      H2_PAL_ERR_INVALID_STATE,
                  "canceled conversation rejects further PCM");
  h2_gizclaw_conversation_deinit(conversation);
  return fails;
}

static int test_backpressure(h2_gizclaw_client_t *client,
                             test_audio_state_t *test,
                             h2_audio_pcm_format_t format) {
  int fails = 0;
  reset_audio_calls(test);
  test->packet_result_count = TEST_MAX_PACKET_CALLS;
  for (size_t call = 0u; call < test->packet_result_count; ++call)
    test->packet_results[call] = GZC_ERR_WOULD_BLOCK;
  h2_gizclaw_conversation_t *conversation =
      open_conversation(client, 6u, &fails);
  fails += expect(h2_gizclaw_conversation_configure_pcm(conversation, &format,
                                                        0) == H2_PAL_OK,
                  "backpressure conversation configures PCM");
  int16_t input[2560];
  for (size_t index = 0u; index < 2560u; ++index)
    input[index] = (int16_t)index;
  for (size_t chunk = 0u; chunk < 5u; ++chunk) {
    h2_audio_frame_t frame = mono_frame(input + chunk * 512u, 512u, format);
    fails += expect(h2_gizclaw_conversation_write_pcm(conversation, &frame) ==
                        H2_PAL_OK,
                    "a full Opus ring continues consuming provider PCM");
  }
  fails += expect(test->encode_calls == 8u,
                  "blocked transport still encodes every complete interval");
  int16_t encoded[2560];
  for (size_t packet = 0u; packet < 8u; ++packet) {
    memcpy(encoded + packet * 320u, test->encoded_pcm[packet],
           320u * sizeof(int16_t));
  }
  fails += expect(memcmp(encoded, input, sizeof(input)) == 0,
                  "backpressure preserves PCM encoding order");
  const size_t recovery_call = test->packet_calls;
  test->packet_result_count = recovery_call;
  fails += expect(h2_gizclaw_conversation_commit(conversation, 0u) == H2_PAL_OK,
                  "commit drains the retained packets after recovery");
  for (size_t packet = 0u; packet < 4u; ++packet) {
    fails +=
        expect(test->packet_first_bytes[recovery_call + packet] == packet + 5u,
               "transport recovery sends the newest retained packets");
  }
  h2_gizclaw_conversation_deinit(conversation);
  return fails;
}

static int test_terminal_failures(h2_gizclaw_client_t *client,
                                  test_audio_state_t *test,
                                  h2_audio_pcm_format_t format) {
  int fails = 0;
  int16_t samples[320] = {0};
  h2_audio_frame_t frame = mono_frame(samples, 320u, format);

  reset_audio_calls(test);
  test->packet_results[0] = GZC_ERR_RPC;
  test->packet_result_count = 1u;
  h2_gizclaw_conversation_t *conversation =
      open_conversation(client, 7u, &fails);
  fails += expect(h2_gizclaw_conversation_configure_pcm(conversation, &format,
                                                        0) == H2_PAL_OK,
                  "transport-failure conversation configures PCM");
  fails += expect(h2_gizclaw_conversation_write_pcm(conversation, &frame) ==
                          H2_PAL_ERR_IO &&
                      h2_gizclaw_conversation_write_pcm(conversation, &frame) ==
                          H2_PAL_ERR_IO &&
                      h2_gizclaw_conversation_commit(conversation, 0u) ==
                          H2_PAL_ERR_IO,
                  "terminal transport failure cannot be retried as unconsumed");
  h2_gizclaw_conversation_deinit(conversation);

  reset_audio_calls(test);
  test->encode_fail_call = 1u;
  conversation = open_conversation(client, 8u, &fails);
  fails += expect(h2_gizclaw_conversation_configure_pcm(conversation, &format,
                                                        0) == H2_PAL_OK,
                  "encode-failure conversation configures PCM");
  fails += expect(h2_gizclaw_conversation_write_pcm(conversation, &frame) ==
                          H2_PAL_ERR_FORMAT &&
                      h2_gizclaw_conversation_write_pcm(conversation, &frame) ==
                          H2_PAL_ERR_FORMAT &&
                      h2_gizclaw_conversation_commit(conversation, 0u) ==
                          H2_PAL_ERR_FORMAT,
                  "terminal encode failure cannot be retried as unconsumed");
  h2_gizclaw_conversation_deinit(conversation);
  return fails;
}

static int test_commit_backpressure(h2_gizclaw_client_t *client,
                                    test_audio_state_t *test,
                                    h2_audio_pcm_format_t format) {
  int fails = 0;
  reset_audio_calls(test);
  test->packet_results[0] = GZC_ERR_WOULD_BLOCK;
  test->packet_result_count = 1u;
  h2_gizclaw_conversation_t *conversation =
      open_conversation(client, 9u, &fails);
  fails += expect(h2_gizclaw_conversation_configure_pcm(conversation, &format,
                                                        0) == H2_PAL_OK,
                  "commit-backpressure conversation configures PCM");
  int16_t samples[100];
  for (size_t index = 0u; index < 100u; ++index)
    samples[index] = (int16_t)(index + 1u);
  h2_audio_frame_t frame = mono_frame(samples, 100u, format);
  fails += expect(h2_gizclaw_conversation_write_pcm(conversation, &frame) ==
                          H2_PAL_OK &&
                      h2_gizclaw_conversation_commit(conversation, 0u) ==
                          H2_PAL_ERR_WOULD_BLOCK &&
                      test->encode_calls == 1u && test->packet_calls == 1u,
                  "blocked commit preserves one padded pending packet");
  fails += expect(
      h2_gizclaw_conversation_commit(conversation, 0u) == H2_PAL_OK &&
          h2_gizclaw_conversation_commit(conversation, 0u) == H2_PAL_OK &&
          test->encode_calls == 1u && test->packet_calls == 2u,
      "commit retry sends no duplicate encode or padding");
  h2_gizclaw_conversation_deinit(conversation);
  return fails;
}

static int test_allocation_and_real_encoder(h2_gizclaw_client_t *client,
                                            test_audio_state_t *test,
                                            h2_audio_pcm_format_t format) {
  int fails = 0;
  reset_audio_calls(test);
  h2_gizclaw_conversation_t *conversation =
      open_conversation(client, 10u, &fails);
  const size_t live_before_configure = test->live_allocations;
  test->fail_allocation_call = test->allocation_calls + 2u;
  fails += expect(h2_gizclaw_conversation_configure_pcm(
                      conversation, &format, 7) == H2_PAL_ERR_NO_MEMORY &&
                      test->live_allocations == live_before_configure,
                  "partial PCM initialization releases successful allocations");
  test->fail_allocation_call = 0u;
  fails += expect(
      h2_gizclaw_conversation_configure_pcm(conversation, &format, 7) ==
              H2_PAL_OK &&
          test->live_allocations == live_before_configure + 2u &&
          h2_gizclaw_test_conversation_opus_complexity(conversation) == 7,
      "PCM state uses PAL memory and the requested Opus complexity");
  h2_gizclaw_conversation_cancel(conversation);
  fails += expect(test->live_allocations == live_before_configure,
                  "cancel releases encoder and accumulator exactly once");
  h2_gizclaw_conversation_deinit(conversation);

  reset_audio_calls(test);
  h2_gizclaw_test_set_conversation_ops(test_packet_send, NULL, test);
  conversation = open_conversation(client, 11u, &fails);
  int16_t silence[320] = {0};
  h2_audio_frame_t frame = mono_frame(silence, 320u, format);
  fails += expect(h2_gizclaw_conversation_configure_pcm(conversation, &format,
                                                        0) == H2_PAL_OK &&
                      h2_gizclaw_conversation_write_pcm(conversation, &frame) ==
                          H2_PAL_OK &&
                      test->packet_calls == 1u && test->packet_lengths[0] > 0u,
                  "the production libopus encoder emits a raw Opus packet");
  h2_gizclaw_conversation_deinit(conversation);
  return fails;
}

int h2_gizclaw_conversation_audio_tests(void) {
  int fails = 0;
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
  const h2_audio_pcm_format_t format = {
      .sample_rate_hz = 16000u,
      .frame_samples_per_channel = 512u,
      .channels = 1u,
      .sample_format = H2_AUDIO_SAMPLE_S16LE,
  };

  fails += test_arbitrary_chunks(client, &test, format);
  fails += test_padding_and_modes(client, &test, format);
  fails += test_validation(client, &test, format);
  fails += test_backpressure(client, &test, format);
  fails += test_terminal_failures(client, &test, format);
  fails += test_commit_backpressure(client, &test, format);
  fails += test_allocation_and_real_encoder(client, &test, format);

  h2_gizclaw_client_deinit(client);
  h2_gizclaw_test_set_conversation_ops(NULL, NULL, NULL);
  h2_gizclaw_test_set_event_ops(NULL, NULL, NULL, NULL);
  fails += expect(test.live_allocations == 0u,
                  "conversation and client teardown release all PAL memory");
  if (fails == 0)
    printf("PASS h2_gizclaw_conversation_audio\n");
  return fails;
}
