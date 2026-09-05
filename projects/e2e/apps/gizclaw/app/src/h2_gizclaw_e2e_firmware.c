#include "h2_gizclaw_e2e_firmware.h"

#include "h2/pal/application/h2_pal_http.h"

#include "h2_gizclaw_firmware.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
  H2_GIZCLAW_E2E_FIRMWARE_CHUNK_SIZE = 32 * 1024,
  H2_GIZCLAW_E2E_SHA256_HEX_LENGTH = 64,
};

typedef struct firmware_sha256 {
  uint32_t state[8];
  uint64_t bit_length;
  uint8_t block[64];
  size_t block_length;
} firmware_sha256_t;

typedef struct firmware_download {
  firmware_sha256_t sha256;
  uint64_t expected_size;
  uint64_t received_size;
} firmware_download_t;

static uint32_t rotate_right(uint32_t value, unsigned bits) {
  return (value >> bits) | (value << (32u - bits));
}

static void sha256_transform(firmware_sha256_t *context,
                             const uint8_t block[64]) {
  static const uint32_t constants[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
      0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
      0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
      0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
      0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
      0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
      0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
      0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
      0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
      0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
  };
  uint32_t words[64];
  for (size_t index = 0u; index < 16u; ++index) {
    words[index] = ((uint32_t)block[index * 4u] << 24u) |
                   ((uint32_t)block[index * 4u + 1u] << 16u) |
                   ((uint32_t)block[index * 4u + 2u] << 8u) |
                   (uint32_t)block[index * 4u + 3u];
  }
  for (size_t index = 16u; index < 64u; ++index) {
    const uint32_t s0 = rotate_right(words[index - 15u], 7u) ^
                        rotate_right(words[index - 15u], 18u) ^
                        (words[index - 15u] >> 3u);
    const uint32_t s1 = rotate_right(words[index - 2u], 17u) ^
                        rotate_right(words[index - 2u], 19u) ^
                        (words[index - 2u] >> 10u);
    words[index] = words[index - 16u] + s0 + words[index - 7u] + s1;
  }
  uint32_t a = context->state[0];
  uint32_t b = context->state[1];
  uint32_t c = context->state[2];
  uint32_t d = context->state[3];
  uint32_t e = context->state[4];
  uint32_t f = context->state[5];
  uint32_t g = context->state[6];
  uint32_t h = context->state[7];
  for (size_t index = 0u; index < 64u; ++index) {
    const uint32_t sum1 =
        rotate_right(e, 6u) ^ rotate_right(e, 11u) ^ rotate_right(e, 25u);
    const uint32_t choice = (e & f) ^ ((~e) & g);
    const uint32_t temp1 = h + sum1 + choice + constants[index] + words[index];
    const uint32_t sum0 =
        rotate_right(a, 2u) ^ rotate_right(a, 13u) ^ rotate_right(a, 22u);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  context->state[0] += a;
  context->state[1] += b;
  context->state[2] += c;
  context->state[3] += d;
  context->state[4] += e;
  context->state[5] += f;
  context->state[6] += g;
  context->state[7] += h;
}

static void sha256_init(firmware_sha256_t *context) {
  *context = (firmware_sha256_t){
      .state =
          {
              0x6a09e667u,
              0xbb67ae85u,
              0x3c6ef372u,
              0xa54ff53au,
              0x510e527fu,
              0x9b05688cu,
              0x1f83d9abu,
              0x5be0cd19u,
          },
  };
}

static void sha256_update(firmware_sha256_t *context, const uint8_t *data,
                          size_t len) {
  for (size_t index = 0u; index < len; ++index) {
    context->block[context->block_length++] = data[index];
    if (context->block_length == sizeof(context->block)) {
      sha256_transform(context, context->block);
      context->bit_length += 512u;
      context->block_length = 0u;
    }
  }
}

static void sha256_finish(firmware_sha256_t *context, uint8_t digest[32]) {
  size_t index = context->block_length;
  context->block[index++] = 0x80u;
  if (index > 56u) {
    memset(&context->block[index], 0, sizeof(context->block) - index);
    sha256_transform(context, context->block);
    index = 0u;
  }
  memset(&context->block[index], 0, 56u - index);
  context->bit_length += (uint64_t)context->block_length * 8u;
  for (size_t byte = 0u; byte < 8u; ++byte) {
    context->block[63u - byte] = (uint8_t)(context->bit_length >> (byte * 8u));
  }
  sha256_transform(context, context->block);
  for (size_t word = 0u; word < 8u; ++word) {
    digest[word * 4u] = (uint8_t)(context->state[word] >> 24u);
    digest[word * 4u + 1u] = (uint8_t)(context->state[word] >> 16u);
    digest[word * 4u + 2u] = (uint8_t)(context->state[word] >> 8u);
    digest[word * 4u + 3u] = (uint8_t)context->state[word];
  }
}

static void sha256_hex(const uint8_t digest[32], char out[65]) {
  static const char hex[] = "0123456789abcdef";
  for (size_t index = 0u; index < 32u; ++index) {
    out[index * 2u] = hex[digest[index] >> 4u];
    out[index * 2u + 1u] = hex[digest[index] & 0x0fu];
  }
  out[64] = '\0';
}

static bool valid_sha256(const char *value) {
  if (value == NULL ||
      memchr(value, '\0', H2_GIZCLAW_E2E_SHA256_HEX_LENGTH) != NULL ||
      value[H2_GIZCLAW_E2E_SHA256_HEX_LENGTH] != '\0') {
    return false;
  }
  for (size_t index = 0u; index < H2_GIZCLAW_E2E_SHA256_HEX_LENGTH; ++index) {
    const char ch = value[index];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
          (ch >= 'A' && ch <= 'F'))) {
      return false;
    }
  }
  return true;
}

static bool digest_equal(const char *left, const char *right) {
  for (size_t index = 0u; index < H2_GIZCLAW_E2E_SHA256_HEX_LENGTH; ++index) {
    char a = left[index];
    char b = right[index];
    if (a >= 'A' && a <= 'F')
      a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'F')
      b = (char)(b - 'A' + 'a');
    if (a != b)
      return false;
  }
  return true;
}

static int cancel_download(void *user) {
  const h2_gizclaw_e2e_fixture_t *fixture = user;
  if (fixture == NULL || fixture->cancel_requested ||
      (fixture->config->should_stop != NULL &&
       fixture->config->should_stop(fixture->config->should_stop_user))) {
    return 1;
  }
  uint64_t now_ms = 0u;
  return fixture->time == NULL ||
         h2_pal_time_get_monotonic_ms(fixture->time, &now_ms) != H2_PAL_OK ||
         now_ms >= fixture->deadline_ms;
}

static int receive_firmware(void *user, const h2_pal_http_request_t *request,
                            const uint8_t *chunk, size_t chunk_len,
                            size_t total_read, size_t remaining) {
  (void)request;
  (void)total_read;
  (void)remaining;
  firmware_download_t *download = user;
  if (download == NULL || (chunk == NULL && chunk_len != 0u) ||
      UINT64_MAX - download->received_size < chunk_len ||
      download->received_size + chunk_len > download->expected_size) {
    return H2_PAL_ERR_FORMAT;
  }
  sha256_update(&download->sha256, chunk, chunk_len);
  download->received_size += chunk_len;
  return H2_PAL_OK;
}

static int firmware_evidence(const char *symbol, const char *stage, int rc) {
  h2_gizclaw_e2e_evidence(symbol, stage, rc);
  return rc;
}

static int validate_metadata(const h2_gizclaw_firmware_t *metadata) {
  return metadata->channel == H2_GIZCLAW_FIRMWARE_CHANNEL_DEVELOP &&
                 metadata->size > 0 &&
                 strncmp(metadata->url, "https://", 8) == 0 &&
                 memchr(metadata->url, 0, sizeof(metadata->url)) != NULL &&
                 valid_sha256(metadata->sha256)
             ? H2_PAL_OK
             : H2_PAL_ERR_FORMAT;
}

static int get_firmware_metadata(h2_gizclaw_e2e_fixture_t *fixture,
                                 h2_gizclaw_firmware_t *out_metadata) {
  h2_gizclaw_service_t *service = fixture->actors[H2_GIZCLAW_E2E_OWNER].service;
  const uint32_t timeout_ms = 15000;
  for (unsigned mode = 0; mode < 2; ++mode) {
    if (!h2_gizclaw_e2e_fixture_has_time(fixture, timeout_ms))
      return H2_PAL_ERR_TIMEOUT;
    int rc;
    if (mode == 0) {
      h2_gizclaw_req_t *request = NULL;
      rc = firmware_evidence(
          "h2_gizclaw_req_create_firmware_get", "firmware-req",
          h2_gizclaw_req_create_firmware_get(
              service, 1, H2_GIZCLAW_FIRMWARE_CHANNEL_DEVELOP, timeout_ms,
              &request));
      if (rc == H2_PAL_OK)
        rc = firmware_evidence("h2_gizclaw_req_do", "firmware-req",
                               h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL));
      if (rc == H2_PAL_OK)
        rc = firmware_evidence("h2_gizclaw_req_wait", "firmware-req",
                               h2_gizclaw_req_wait(request, timeout_ms));
      if (rc == H2_PAL_OK)
        rc = firmware_evidence(
            "h2_gizclaw_resp_parse_firmware_get", "firmware-req",
            h2_gizclaw_resp_parse_firmware_get(request, out_metadata));
      if (rc != H2_PAL_OK && request != NULL)
        (void)h2_gizclaw_req_cancel(request);
      h2_gizclaw_req_release(request);
    } else {
      rc = firmware_evidence("h2_gizclaw_rpc_firmware_get", "firmware-rpc",
                             h2_gizclaw_rpc_firmware_get(
                                 service, H2_GIZCLAW_FIRMWARE_CHANNEL_DEVELOP,
                                 timeout_ms, out_metadata));
    }
    if (rc == H2_PAL_OK)
      rc = firmware_evidence(mode == 0 ? "h2_gizclaw_resp_parse_firmware_get"
                                       : "h2_gizclaw_rpc_firmware_get",
                             "firmware_get-assert",
                             validate_metadata(out_metadata));
    if (rc != H2_PAL_OK)
      return rc;
  }
  return H2_PAL_OK;
}

static int download_firmware(h2_gizclaw_e2e_fixture_t *fixture,
                             const h2_gizclaw_firmware_t *metadata,
                             uint64_t *out_received_size) {
  if (validate_metadata(metadata) != H2_PAL_OK) {
    return H2_PAL_ERR_FORMAT;
  }
  uint8_t *chunk =
      h2_pal_mem_alloc(fixture->allocator, H2_GIZCLAW_E2E_FIRMWARE_CHUNK_SIZE);
  if (chunk == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  firmware_download_t download = {
      .expected_size = (uint64_t)metadata->size,
  };
  sha256_init(&download.sha256);
  h2_pal_http_request_t request = {
      .method = H2_PAL_HTTP_GET,
      .url = {.data = metadata->url, .len = strlen(metadata->url)},
      .timeout_ms = 30000,
      .retry_count = 0,
      .chunk_buf = chunk,
      .chunk_buf_cap = H2_GIZCLAW_E2E_FIRMWARE_CHUNK_SIZE,
      .read_cb = receive_firmware,
      .user = &download,
      .cancel_cb = cancel_download,
      .cancel_user = fixture,
      .allocator = fixture->allocator,
  };
  h2_pal_http_response_t response;
  h2_pal_http_response_reset(&response);
  int result = h2_pal_http_request(fixture->http, &request, &response);
  h2_gizclaw_e2e_evidence("h2_pal_http_request", "firmware", result);
  if (result == H2_PAL_OK &&
      (response.status_code < 200 || response.status_code >= 300)) {
    result = response.status_code == 404 ? H2_PAL_ERR_NOT_FOUND : H2_PAL_ERR_IO;
  }
  if (result == H2_PAL_OK && response.content_length >= 0 &&
      (uint64_t)response.content_length != download.expected_size) {
    result = H2_PAL_ERR_FORMAT;
  }
  if (result == H2_PAL_OK && download.received_size != download.expected_size) {
    result = H2_PAL_ERR_FORMAT;
  }
  uint8_t digest[32];
  char digest_hex[65];
  sha256_finish(&download.sha256, digest);
  sha256_hex(digest, digest_hex);
  if (result == H2_PAL_OK && !digest_equal(digest_hex, metadata->sha256)) {
    result = H2_PAL_ERR_FORMAT;
  }
  h2_pal_http_response_free(fixture->http, &response);
  memset(chunk, 0, H2_GIZCLAW_E2E_FIRMWARE_CHUNK_SIZE);
  h2_pal_mem_free(fixture->allocator, chunk);
  *out_received_size = download.received_size;
  return result;
}

int h2_gizclaw_e2e_run_firmware(h2_gizclaw_e2e_fixture_t *fixture) {
  if (fixture == NULL || fixture->http == NULL ||
      fixture->actors[H2_GIZCLAW_E2E_OWNER].service == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_gizclaw_firmware_t metadata = {0};
  int result = get_firmware_metadata(fixture, &metadata);
  uint64_t received_size = 0u;
  if (result == H2_PAL_OK) {
    result = download_firmware(fixture, &metadata, &received_size);
  }
  printf("H2_GIZCLAW_E2E stage=firmware result=%s bytes=%" PRIu64
         " expected=%" PRId64 " digest_match=%s\n",
         result == H2_PAL_OK ? "PASS" : "FAIL", received_size, metadata.size,
         result == H2_PAL_OK ? "yes" : "no");
  return result;
}
