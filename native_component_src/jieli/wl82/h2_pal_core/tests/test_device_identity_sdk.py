"""Exercise the production WL82 identity binding with persistent SDK fakes."""
from pathlib import Path
import subprocess
import tempfile
import unittest

CORE = Path(__file__).resolve().parents[1]
ROOT = CORE.parents[3]
SOURCE = CORE / "src/h2_jieli_wl82_sdk_port.c"

STUB = r'''
#include "core/h2_pal_errors.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#ifdef H2_JIELI_BLE_ENABLE
#define CFG_BT_MAC_ADDR 42
static uint8_t persisted[6];
static int persisted_length = -1, fail_write;
static unsigned writes, random_calls, derivations;
static const uint8_t flash_uid[16] = {1, 2, 3, 4};
static int syscfg_read(int key, void *out, unsigned size) {
  assert(key == CFG_BT_MAC_ADDR && size == 6u);
  memcpy(out, persisted, size);
  return persisted_length;
}
static int syscfg_write(int key, const void *data, unsigned size) {
  assert(key == CFG_BT_MAC_ADDR && size == 6u);
  ++writes;
  if (fail_write) return -1;
  memcpy(persisted, data, size);
  persisted_length = (int)size;
  return (int)size;
}
static const uint8_t *get_norflash_uuid(void) { return flash_uid; }
static uint32_t rand32(void) { ++random_calls; return UINT32_C(0x10203040); }
static uint32_t CRC32(const void *data, unsigned size) {
  assert(size == sizeof(flash_uid) && memcmp(data, flash_uid, size) == 0);
  return UINT32_C(0x01020304);
}
static uint16_t CRC16(const void *data, unsigned size) {
  assert(size == sizeof(flash_uid) && memcmp(data, flash_uid, size) == 0);
  return UINT16_C(0x1234);
}
static int bytecmp(const void *data, int value, unsigned size) {
  const uint8_t *bytes = data;
  for (unsigned i = 0u; i < size; ++i)
    if (bytes[i] != value) return 1;
  return 0;
}
void lib_make_ble_address(uint8_t *out, uint8_t *base) {
  ++derivations;
  for (unsigned i = 0u; i < 6u; ++i) out[i] = base[i] ^ 0x5au;
}
static void check_matches_controller(const uint8_t identity[6]) {
  for (unsigned i = 0u; i < 6u; ++i)
    assert(identity[i] == (uint8_t)(persisted[5u-i] ^ 0x5au));
}
#endif
'''

MAIN = r'''
int main(void) {
  uint8_t identity[6], previous[6], empty[6] = {0};
  assert(h2_jieli_sdk_device_identity(NULL) == H2_PAL_ERR_INVALID_ARG);
#ifdef H2_JIELI_BLE_ENABLE
  /* A provisioned board uses exactly the controller derivation and byte order. */
  const uint8_t base[6] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab};
  memcpy(persisted, base, sizeof(base));
  persisted_length = sizeof(base);
  assert(h2_jieli_sdk_device_identity(identity) == H2_PAL_OK);
  check_matches_controller(identity);
  assert(writes == 0u && random_calls == 0u && derivations == 1u);
  /* First boot provisions the existing algorithm once, before BLE or Wi-Fi. */
  persisted_length = -1;
  assert(h2_jieli_sdk_device_identity(identity) == H2_PAL_OK);
  assert(writes == 1u && random_calls == 2u);
  uint8_t expected[6];
  uint32_t crc32 = UINT32_C(0x10203040) ^ UINT32_C(0x01020304);
  uint16_t crc16 = (uint16_t)(UINT32_C(0x10203040) ^ UINT16_C(0x1234));
  memcpy(expected, &crc32, sizeof(crc32));
  memcpy(expected + sizeof(crc32), &crc16, sizeof(crc16));
  expected[0] &= (uint8_t)~3u;
  assert(memcmp(persisted, expected, sizeof(expected)) == 0);
  check_matches_controller(identity);
  memcpy(previous, identity, sizeof(previous));
  assert(h2_jieli_sdk_device_identity(identity) == H2_PAL_OK);
  assert(memcmp(identity, previous, sizeof(identity)) == 0);
  assert(writes == 1u && random_calls == 2u);
  /* Erased and short records use the same provisioning path. */
  memset(persisted, 0xff, sizeof(persisted));
  assert(h2_jieli_sdk_device_identity(identity) == H2_PAL_OK);
  assert(writes == 2u && random_calls == 4u);
  persisted_length = 5;
  assert(h2_jieli_sdk_device_identity(identity) == H2_PAL_OK);
  assert(writes == 3u && random_calls == 6u);
  /* Never advertise an unstable identity when persistence failed. */
  persisted_length = -1;
  fail_write = 1;
  unsigned derived_before_failure = derivations;
  memset(identity, 0xa5, sizeof(identity));
  assert(h2_jieli_sdk_device_identity(identity) == H2_PAL_ERR_IO);
  assert(memcmp(identity, empty, sizeof(identity)) == 0);
  assert(derivations == derived_before_failure);
  fail_write = 0;
  assert(h2_jieli_sdk_device_identity(identity) == H2_PAL_OK);
  check_matches_controller(identity);
#else
  (void)previous;
  memset(identity, 0xa5, sizeof(identity));
  assert(h2_jieli_sdk_device_identity(identity) == H2_PAL_ERR_UNSUPPORTED);
  assert(memcmp(identity, empty, sizeof(identity)) == 0);
#endif
  return 0;
}
'''


class DeviceIdentitySdkTest(unittest.TestCase):
    def test_persisted_and_first_boot_identity(self):
        text = SOURCE.read_text()
        start = text.index("/* ---- Stable public identity")
        end = text.index("/* ---- Memory", start)
        for enabled in (True, False):
            with self.subTest(ble_enabled=enabled), tempfile.TemporaryDirectory(
                prefix="h2-jieli-device-identity-"
            ) as directory:
                root = Path(directory)
                unit = root / "identity.c"
                binary = root / "identity-test"
                unit.write_text(STUB + text[start:end] + MAIN)
                command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                           "-I", str(ROOT / "pal/include")]
                if enabled:
                    command.append("-DH2_JIELI_BLE_ENABLE=1")
                subprocess.run(command + [str(unit), "-o", str(binary)],
                               check=True, timeout=30)
                subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
