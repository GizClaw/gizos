"""Exercise AP credential boundaries in the pinned vendor implementation."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PATCH = ROOT / "boards/jieli_ac791n_devkit/ac791n/layouts/h2loader/sdk_patches/wifi_ap_credentials.patch"


class WifiSdkConfigTest(unittest.TestCase):
    def test_maximum_credentials(self):
        sdk = os.environ.get("JIELI_AC791N_SDK_PATH")
        if not sdk:
            self.skipTest("JIELI_AC791N_SDK_PATH is required")
        source = (Path(sdk) / "apps/common/net/wifi_conf.c").read_text()
        with tempfile.TemporaryDirectory() as directory:
            copy = Path(directory) / "apps/common/net/wifi_conf.c"
            copy.parent.mkdir(parents=True)
            copy.write_text(source)
            subprocess.run(["git", "apply", str(PATCH)], cwd=directory, check=True)
            source = copy.read_text()
        fields = source[source.index("static char WL_AP_DAT[]"):
                        source.index("};", source.index("static char WL_AP_DAT[]")) + 2]
        setters = source[source.index("static int wl_set_ssid("):
                         source.index("int wl_ap_init(")]
        fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void *checked_memset(void *p, int c, size_t n) {
    assert(n <= 63);
    return memset(p, c, n);
}
#undef memset
#define memset checked_memset
'''
        for length, setter in [(32, "wl_set_ssid"), (63, "wl_set_passphrase")]:
            with self.subTest(length=length), tempfile.TemporaryDirectory() as directory:
                unit = Path(directory) / "config.c"
                binary = Path(directory) / "config-test"
                unit.write_text(fixture + fields +
                                "\nconst char *GET_WL_AP_DAT(void) { return WL_AP_DAT; }\n" +
                                setters + f'\nint main(void) {{ assert({setter}("' +
                                "a" * length + '") == 0);\n' +
                                'assert(strstr(WL_AP_DAT, "\\nWirelessMode=9\\n"));\n' +
                                'assert(strstr(WL_AP_DAT, "\\nChannel=11#\\n"));\n' +
                                'assert(strstr(WL_AP_DAT, "\\nHideSSID=0\\n"));\n' +
                                'return 0; }\n')
                subprocess.run(["cc", str(unit), "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
