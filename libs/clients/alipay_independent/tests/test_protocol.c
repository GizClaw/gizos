#include "h2_alipay_protocol.h"
#include "h2_app_test_mem.h"

#include <assert.h>
#include <string.h>

static const h2_alipay_protocol_identity_t identity = {
    .sn = "FACTORY_SN_001",
    .imei = "FACTORY_SN_001",
    .mac = "12:34:56:78:9A:BC",
    .model = "H-106TG",
};

int main(void) {
  char timestamp[24];
  assert(h2_alipay_protocol_timestamp(1794449000123u, timestamp) == H2_PAL_OK);
  assert(strcmp(timestamp, "1794449000000") == 0);
  assert(h2_alipay_protocol_timestamp(0u, timestamp) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(timestamp[0] == '\0');
  assert(h2_alipay_protocol_timestamp(UINT64_MAX, timestamp) ==
         H2_PAL_ERR_INVALID_STATE);
  uint8_t key[32];
  static const uint8_t expected_key[32] = {
      0x0c, 0x84, 0x80, 0x21, 0x2d, 0x94, 0xfd, 0x05, 0x12, 0xb9, 0xd3,
      0x95, 0xcd, 0xd0, 0x50, 0xf0, 0x54, 0x2a, 0x63, 0xdb, 0x11, 0x1f,
      0xde, 0x4e, 0xf9, 0xc9, 0x2f, 0xed, 0x40, 0x9c, 0xbd, 0x5b,
  };
  assert(h2_alipay_protocol_signing_key(&identity, "SYNTHETIC_DEVICE_NAME_001",
                                        key) == H2_PAL_OK);
  assert(memcmp(key, expected_key, sizeof(key)) == 0);
  h2_alipay_protocol_identity_t invalid = identity;
  invalid.sn = "BAD#SN";
  assert(h2_alipay_protocol_signing_key(&invalid, "SYNTHETIC_DEVICE_NAME_001",
                                        key) == H2_PAL_ERR_INVALID_ARG);
  assert(memcmp(key, (uint8_t[32]){0}, sizeof(key)) == 0);
  char canonical[256];
  size_t length = 0u;
  assert(h2_alipay_protocol_canonical("operation", "1794449000", false,
                                      (const uint8_t*)"a", 1u, canonical,
                                      sizeof(canonical), &length) == H2_PAL_OK);
  assert(strcmp(canonical,
                "Operation-Type=operation&Request-Data=YQ==&Ts=1794449000") ==
         0);
  assert(length == strlen(canonical));
  assert(h2_alipay_protocol_canonical("op", "1", false, NULL, 0u, canonical,
                                      sizeof(canonical), &length) == H2_PAL_OK);
  assert(strcmp(canonical, "Operation-Type=op&Request-Data=[]&Ts=1") == 0);
  assert(h2_alipay_protocol_canonical("op", "1", true, NULL, 0u, canonical,
                                      sizeof(canonical), &length) == H2_PAL_OK);
  assert(strcmp(canonical, "Operation-Type=op&Request-Data=&Ts=1") == 0);
  memset(canonical, 0xa5, sizeof(canonical));
  assert(h2_alipay_protocol_canonical("op", "1", false, NULL, 0u, canonical, 1u,
                                      &length) == H2_PAL_ERR_NO_SPACE);
  assert((unsigned char)canonical[0] == 0xa5u);
  assert(h2_alipay_protocol_canonical("op", "1&x=2", false, NULL, 0u, canonical,
                                      sizeof(canonical),
                                      &length) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_alipay_protocol_canonical("op", "1", false, (const uint8_t*)"x",
                                      SIZE_MAX, canonical, sizeof(canonical),
                                      &length) == H2_PAL_ERR_NO_SPACE);
  uint8_t registration[512];
  invalid = identity;
  invalid.model = "MODEL#WITH_DELIMITER";
  assert(h2_alipay_protocol_registration(&invalid, registration,
                                         sizeof(registration),
                                         &length) == H2_PAL_ERR_INVALID_ARG);
  assert(length == 0u);
  assert(h2_alipay_protocol_registration(&identity, NULL, 0u, &length) ==
         H2_PAL_ERR_NO_SPACE);
  size_t required = length;
  assert(h2_alipay_protocol_registration(&identity, registration, required - 1u,
                                         &length) == H2_PAL_ERR_NO_SPACE);
  assert(h2_alipay_protocol_registration(&identity, registration, required,
                                         &length) == H2_PAL_OK);
  assert(length == required);
  static const uint8_t response[] = {0x0a, 0x04, 'N',  'A', 'M',
                                     'E',  0x10, 0x81, 0x01};
  char name[132];
  assert(h2_alipay_protocol_device_name(response, sizeof(response), name,
                                        sizeof(name)) == H2_PAL_OK);
  assert(strcmp(name, "NAME") == 0);
  assert(h2_alipay_protocol_device_name(response, sizeof(response), name, 4u) ==
         H2_PAL_ERR_NO_SPACE);
  assert(name[0] == '\0');
  const uint8_t malformed[][8] = {
      {0x0a, 0x06, 'N', 'A', 'M', 'E'},     {0x0a, 0x01, 'A', 0x0a, 0x01, 'B'},
      {0x08, 0x01, 0x0a, 0x02, 'A', 'B'},   {0x0a, 0x04, 'N', '\0', 'M', 'E'},
      {0x0a, 0x00, 0x10, 0x01, 0x18, 0x01},
  };
  for (size_t index = 0u; index < sizeof(malformed) / sizeof(*malformed);
       ++index)
    assert(h2_alipay_protocol_device_name(malformed[index], 6u, name,
                                          sizeof(name)) == H2_PAL_ERR_FORMAT);
  h2_app_test_mem_t memory;
  h2_app_test_mem_init(&memory, NULL);
  uint8_t public_key[64];
  for (size_t index = 0u; index < sizeof(public_key); ++index)
    public_key[index] = (uint8_t)index;
  char* body = NULL;
  assert(h2_alipay_protocol_prepare_bind(
             &memory.api, &identity, "HAIVIVI", "SYNTHETIC_BIZ_001", 16385u,
             1794449000u, public_key, &body, &length) == H2_PAL_OK);
  assert(length == strlen(body));
  assert(strstr(body, "\"clientKey\":\"FACTORY_SN_001\"") != NULL);
  assert(strstr(body, "\"ecdhFactor\":\"00010203") != NULL);
  h2_pal_mem_free(&memory.api, body);
  size_t calls = memory.calls;
  for (size_t index = 1u; index <= calls; ++index) {
    memory.fail_at = memory.calls + index;
    int rc = h2_alipay_protocol_prepare_bind(
        &memory.api, &identity, "HAIVIVI", "SYNTHETIC_BIZ_001", 16385u,
        1794449000u, public_key, &body, &length);
    assert(rc == H2_PAL_ERR_NO_MEMORY || rc == H2_PAL_OK);
    if (rc == H2_PAL_OK) h2_pal_mem_free(&memory.api, body);
    assert(memory.live_blocks == 0u);
  }
  assert(h2_alipay_protocol_binding_code(
             &identity, "HAIVIVI", "SYNTHETIC_BINDING_TOKEN", 16385u,
             1794449000u, canonical, sizeof(canonical), &length) == H2_PAL_OK);
  assert(strcmp(canonical,
                "ALIPAY_QZWT#0#SYNTHETIC_BINDING_TOKEN#"
                "QZ.SB.LY.HAIVIVI-H-106TG#4.612.16385.179444") == 0);
  return 0;
}
