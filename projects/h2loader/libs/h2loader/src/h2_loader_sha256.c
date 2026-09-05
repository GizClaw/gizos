#include "h2_loader_sha256.h"

#include <string.h>

static const uint32_t constants[64] = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
    UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
    UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
    UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
    UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
    UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
    UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
    UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
    UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
    UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
    UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
    UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
    UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
    UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
    UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
    UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
    UINT32_C(0xc67178f2),
};

static uint32_t rotate_right(uint32_t value, uint32_t amount) {
  return (value >> amount) | (value << (32u - amount));
}

static uint32_t read_be32(const uint8_t *data) {
  return ((uint32_t)data[0] << 24u) | ((uint32_t)data[1] << 16u) |
         ((uint32_t)data[2] << 8u) | (uint32_t)data[3];
}

static void write_be32(uint8_t *out, uint32_t value) {
  out[0] = (uint8_t)(value >> 24u);
  out[1] = (uint8_t)(value >> 16u);
  out[2] = (uint8_t)(value >> 8u);
  out[3] = (uint8_t)value;
}

static void transform(h2_loader_sha256_t *sha, const uint8_t block[64]) {
  uint32_t schedule[64];
  uint32_t a = sha->state[0], b = sha->state[1], c = sha->state[2];
  uint32_t d = sha->state[3], e = sha->state[4], f = sha->state[5];
  uint32_t g = sha->state[6], h = sha->state[7];

  for (size_t i = 0; i < 16u; ++i) schedule[i] = read_be32(&block[i * 4u]);
  for (size_t i = 16u; i < 64u; ++i) {
    uint32_t s0 = rotate_right(schedule[i - 15u], 7u) ^
                  rotate_right(schedule[i - 15u], 18u) ^
                  (schedule[i - 15u] >> 3u);
    uint32_t s1 = rotate_right(schedule[i - 2u], 17u) ^
                  rotate_right(schedule[i - 2u], 19u) ^
                  (schedule[i - 2u] >> 10u);
    schedule[i] = schedule[i - 16u] + s0 + schedule[i - 7u] + s1;
  }
  for (size_t i = 0; i < 64u; ++i) {
    uint32_t s1 = rotate_right(e, 6u) ^ rotate_right(e, 11u) ^
                  rotate_right(e, 25u);
    uint32_t choice = (e & f) ^ ((~e) & g);
    uint32_t temp1 = h + s1 + choice + constants[i] + schedule[i];
    uint32_t s0 = rotate_right(a, 2u) ^ rotate_right(a, 13u) ^
                  rotate_right(a, 22u);
    uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    uint32_t temp2 = s0 + majority;
    h = g; g = f; f = e; e = d + temp1;
    d = c; c = b; b = a; a = temp1 + temp2;
  }
  sha->state[0] += a; sha->state[1] += b; sha->state[2] += c;
  sha->state[3] += d; sha->state[4] += e; sha->state[5] += f;
  sha->state[6] += g; sha->state[7] += h;
}

void h2_loader_sha256_init(h2_loader_sha256_t *sha) {
  static const uint32_t initial[8] = {
      UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85),
      UINT32_C(0x3c6ef372), UINT32_C(0xa54ff53a),
      UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
      UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19),
  };
  memset(sha, 0, sizeof(*sha));
  memcpy(sha->state, initial, sizeof(initial));
}

void h2_loader_sha256_update(
    h2_loader_sha256_t *sha, const uint8_t *data, size_t len) {
  if (sha == NULL || (len != 0u && data == NULL)) return;
  sha->total_bytes += len;
  while (len != 0u) {
    size_t available = sizeof(sha->block) - sha->block_len;
    size_t take = len < available ? len : available;
    memcpy(&sha->block[sha->block_len], data, take);
    sha->block_len += take;
    data += take;
    len -= take;
    if (sha->block_len == sizeof(sha->block)) {
      transform(sha, sha->block);
      sha->block_len = 0u;
    }
  }
}

void h2_loader_sha256_finish(h2_loader_sha256_t *sha, uint8_t out[32]) {
  uint64_t total_bits = sha->total_bytes * UINT64_C(8);
  sha->block[sha->block_len++] = 0x80u;
  if (sha->block_len > 56u) {
    memset(&sha->block[sha->block_len], 0, sizeof(sha->block) - sha->block_len);
    transform(sha, sha->block);
    sha->block_len = 0u;
  }
  memset(&sha->block[sha->block_len], 0, 56u - sha->block_len);
  for (size_t i = 0; i < 8u; ++i) {
    sha->block[63u - i] = (uint8_t)(total_bits >> (i * 8u));
  }
  transform(sha, sha->block);
  for (size_t i = 0; i < 8u; ++i) write_be32(&out[i * 4u], sha->state[i]);
  memset(sha, 0, sizeof(*sha));
}

void h2_loader_sha256_hex(const uint8_t digest[32], char out[65]) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < 32u; ++i) {
    out[i * 2u] = hex[digest[i] >> 4u];
    out[i * 2u + 1u] = hex[digest[i] & 0x0fu];
  }
  out[64] = '\0';
}
