#include "h2_sm3.h"

#include <string.h>

static uint32_t rotate_left(uint32_t value, unsigned count) {
  count &= 31u;
  return count == 0u ? value : (value << count) | (value >> (32u - count));
}

static uint32_t read_word(const uint8_t* data) {
  return ((uint32_t)data[0] << 24u) | ((uint32_t)data[1] << 16u) |
         ((uint32_t)data[2] << 8u) | data[3];
}

static void clear(void* data, size_t length) {
  volatile uint8_t* bytes = data;
  while (length-- != 0u) *bytes++ = 0u;
}

static void compress(h2_sm3_t* context) {
  uint32_t words[68];
  for (size_t index = 0u; index < 16u; ++index)
    words[index] = read_word(context->block + index * 4u);
  for (size_t index = 16u; index < 68u; ++index) {
    uint32_t value = words[index - 16u] ^ words[index - 9u] ^
                     rotate_left(words[index - 3u], 15u);
    words[index] = value ^ rotate_left(value, 15u) ^ rotate_left(value, 23u) ^
                   rotate_left(words[index - 13u], 7u) ^ words[index - 6u];
  }
  uint32_t state_a = context->state[0], state_b = context->state[1];
  uint32_t state_c = context->state[2], state_d = context->state[3];
  uint32_t state_e = context->state[4], state_f = context->state[5];
  uint32_t state_g = context->state[6], state_h = context->state[7];
  for (unsigned round = 0u; round < 64u; ++round) {
    uint32_t constant = round < 16u ? 0x79cc4519u : 0x7a879d8au;
    uint32_t rotated_a = rotate_left(state_a, 12u);
    uint32_t sum =
        rotate_left(rotated_a + state_e + rotate_left(constant, round), 7u);
    uint32_t boolean_a =
        round < 16u
            ? state_a ^ state_b ^ state_c
            : (state_a & state_b) | (state_a & state_c) | (state_b & state_c);
    uint32_t boolean_e = round < 16u
                             ? state_e ^ state_f ^ state_g
                             : (state_e & state_f) | (~state_e & state_g);
    uint32_t next_a = boolean_a + state_d + (sum ^ rotated_a) +
                      (words[round] ^ words[round + 4u]);
    uint32_t next_e = boolean_e + state_h + sum + words[round];
    state_d = state_c;
    state_c = rotate_left(state_b, 9u);
    state_b = state_a;
    state_a = next_a;
    state_h = state_g;
    state_g = rotate_left(state_f, 19u);
    state_f = state_e;
    state_e = next_e ^ rotate_left(next_e, 9u) ^ rotate_left(next_e, 17u);
  }
  context->state[0] ^= state_a;
  context->state[1] ^= state_b;
  context->state[2] ^= state_c;
  context->state[3] ^= state_d;
  context->state[4] ^= state_e;
  context->state[5] ^= state_f;
  context->state[6] ^= state_g;
  context->state[7] ^= state_h;
  clear(words, sizeof(words));
}

void h2_sm3_init(h2_sm3_t* context) {
  *context = (h2_sm3_t){
      .state = {0x7380166fu, 0x4914b2b9u, 0x172442d7u, 0xda8a0600u, 0xa96f30bcu,
                0x163138aau, 0xe38dee4du, 0xb0fb0e4eu},
  };
}

void h2_sm3_update(h2_sm3_t* context, const uint8_t* data, size_t length) {
  context->total_bytes += length;
  while (length != 0u) {
    size_t available = sizeof(context->block) - context->block_len;
    size_t count = length < available ? length : available;
    memcpy(context->block + context->block_len, data, count);
    context->block_len += count;
    data += count;
    length -= count;
    if (context->block_len == sizeof(context->block)) {
      compress(context);
      context->block_len = 0u;
    }
  }
}

void h2_sm3_finish(h2_sm3_t* context, uint8_t out[32]) {
  uint64_t bits = context->total_bytes * 8u;
  uint8_t padding[72] = {0x80u};
  size_t length = context->block_len < 56u ? 56u - context->block_len
                                           : 120u - context->block_len;
  for (size_t index = 0u; index < 8u; ++index)
    padding[length + index] = (uint8_t)(bits >> ((7u - index) * 8u));
  h2_sm3_update(context, padding, length + 8u);
  for (size_t index = 0u; index < 32u; ++index)
    out[index] =
        (uint8_t)(context->state[index / 4u] >> ((3u - index % 4u) * 8u));
  clear(context, sizeof(*context));
}

void h2_sm3_hmac(const uint8_t* key, size_t key_length, const uint8_t* data,
                 size_t length, uint8_t out[32]) {
  uint8_t pad[64] = {0};
  uint8_t inner[32];
  h2_sm3_t context;
  if (key_length > sizeof(pad)) {
    h2_sm3_init(&context);
    h2_sm3_update(&context, key, key_length);
    h2_sm3_finish(&context, pad);
  } else if (key_length != 0u) {
    memcpy(pad, key, key_length);
  }
  for (size_t index = 0u; index < sizeof(pad); ++index) pad[index] ^= 0x36u;
  h2_sm3_init(&context);
  h2_sm3_update(&context, pad, sizeof(pad));
  h2_sm3_update(&context, data, length);
  h2_sm3_finish(&context, inner);
  for (size_t index = 0u; index < sizeof(pad); ++index)
    pad[index] ^= 0x36u ^ 0x5cu;
  h2_sm3_init(&context);
  h2_sm3_update(&context, pad, sizeof(pad));
  h2_sm3_update(&context, inner, sizeof(inner));
  h2_sm3_finish(&context, out);
  clear(pad, sizeof(pad));
  clear(inner, sizeof(inner));
}
