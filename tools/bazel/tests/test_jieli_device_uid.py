"""Verify Loader identity uses the controller address in display byte order."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class DeviceUidTest(unittest.TestCase):
    def test_controller_identity(self):
        source = (ROOT / "boards/jieli_ac791n_devkit/ac791n/src/"
                  "h2_jieli_ac791n_devkit_ble.c").read_text()
        start = source.index("const char *h2_jieli_ac791n_devkit_device_uid(void)")
        end = source.index("static int h2_uuid_equal", start)
        stub = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
static uint8_t base[] = {1, 2, 3, 4, 5, 6};
static const uint8_t *h2_ble_base_mac(void) { return base; }
void lib_make_ble_address(uint8_t *out, uint8_t *in) {
  assert(in == base);
  const uint8_t identity[] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc};
  memcpy(out, identity, 6);
}
'''
        main = r'''
int main(void) {
  assert(strcmp(h2_jieli_ac791n_devkit_device_uid(), "bc9a78563412") == 0);
  assert(strcmp(h2_jieli_ac791n_devkit_device_uid(), "bc9a78563412") == 0);
  return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="jieli-uid-test-") as directory:
            path = Path(directory)
            (path / "test.c").write_text(stub + source[start:end] + main)
            subprocess.run(["cc", "-Wall", "-Werror", str(path / "test.c"),
                            "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
