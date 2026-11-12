#include "h2_sm3.h"

#include <assert.h>
#include <string.h>

static void expect(const uint8_t digest[32], const char* expected) {
  static const char alphabet[] = "0123456789abcdef";
  for (size_t index = 0u; index < 32u; ++index) {
    assert(alphabet[digest[index] >> 4u] == expected[index * 2u]);
    assert(alphabet[digest[index] & 15u] == expected[index * 2u + 1u]);
  }
}

int main(void) {
  static const struct {
    size_t length;
    const char* digest;
  } fixtures[] = {
      {0, "1ab21d8355cfa17f8e61194831e81a8f22bec8c728fefb747ed035eb5082aa2b"},
      {55, "a79cf9dcee3404abf7f769698201647fd9d3ff61d629d0f58bb4b5579a427db8"},
      {56, "62f7363b15f4de76dd925c493b9d6d00d4ba0ef2a1f334c1d0f13b293aeb40d1"},
      {63, "6165e4cbb15cde01c6226e0015a47f710f8f8e1f2c296700033bb34d9212109c"},
      {64, "93566f236d157aae078d1ddb5cebdbba1520b5142e22a8915564345ba2ae1d63"},
      {65, "c886e6814be748285a10b28ae62ddacd85db830cd2cf3a2bfa2f729c15f63618"},
      {127, "bca3436d828517a6a6893a9e309e06e7b7b29c6e3f78b4814b23efe149962980"},
  };
  uint8_t data[127], digest[32];
  for (size_t index = 0u; index < sizeof(data); ++index)
    data[index] = (uint8_t)index;
  for (size_t index = 0u; index < sizeof(fixtures) / sizeof(*fixtures);
       ++index) {
    for (size_t chunk = 1u; chunk < 70u; ++chunk) {
      h2_sm3_t context;
      h2_sm3_init(&context);
      h2_sm3_update(&context, NULL, 0u);
      for (size_t offset = 0u; offset < fixtures[index].length;
           offset += chunk) {
        size_t remaining = fixtures[index].length - offset;
        h2_sm3_update(&context, data + offset,
                      remaining < chunk ? remaining : chunk);
      }
      h2_sm3_finish(&context, digest);
      expect(digest, fixtures[index].digest);
      const h2_sm3_t zero = {0};
      assert(memcmp(&context, &zero, sizeof(context)) == 0);
    }
  }
  h2_sm3_t context;
  h2_sm3_init(&context);
  h2_sm3_update(&context, (const uint8_t*)"abc", 3u);
  h2_sm3_finish(&context, digest);
  expect(digest,
         "66c7f0f462eeedd9d1f2d46bdc10e4e24167c4875cf2f7a2297da02b8f4ba8e0");
  uint8_t key[20];
  memset(key, 0x0b, sizeof(key));
  h2_sm3_hmac(key, sizeof(key), (const uint8_t*)"Hi There", 8u, digest);
  expect(digest,
         "51b00d1fb49832bfb01c3ce27848e59f871d9ba938dc563b338ca964755cce70");
  h2_sm3_hmac((const uint8_t*)"Jefe", 4u,
              (const uint8_t*)"what do ya want for nothing?", 28u, digest);
  expect(digest,
         "2e87f1d16862e6d964b50a5200bf2b10b764faa9680a296a2405f24bec39f882");
  h2_sm3_hmac(data, 65u, data, sizeof(data), digest);
  expect(digest,
         "c2f8a9badefcb95bc8c5e47e5a8ecb674d37a3d877ebc429bddbae726f8147f9");
  return 0;
}
