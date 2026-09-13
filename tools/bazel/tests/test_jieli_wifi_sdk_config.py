"""Exercise AP credential boundaries in the pinned vendor implementation."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
PATCH = ROOT / "boards/jieli_ac791n_devkit/ac791n/layouts/h2loader/sdk_patches/wifi_ap_credentials.patch"


class WifiSdkConfigTest(unittest.TestCase):
    def test_ap_options_are_transactional(self):
        sdk = os.environ.get("JIELI_AC791N_SDK_PATH")
        if not sdk:
            self.skipTest("JIELI_AC791N_SDK_PATH is required")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            copy = root / "apps/common/net/wifi_conf.c"
            copy.parent.mkdir(parents=True)
            copy.write_text((Path(sdk) / "apps/common/net/wifi_conf.c").read_text())
            header = root / "include_lib/net/wifi/wifi_connect.h"
            header.parent.mkdir(parents=True)
            header.write_text((Path(sdk) / "include_lib/net/wifi/wifi_connect.h").read_text())
            subprocess.run(["git", "apply", str(PATCH.with_name("wifi_ap_config.patch"))],
                           cwd=root, check=True)
            source = copy.read_text()
            begin = source.index("static char WL_AP_DAT[]")
            fields = source[begin:source.index("};", begin) + 2]
            begin = source.index("int h2_jieli_wifi_configure_ap(")
            function = source[begin:source.index("const char *GET_WL_AP_DAT", begin)]
            fixture = '#include <assert.h>\n#include <string.h>\nstatic int on;\nint wifi_is_on(void) { return on; }\n'
            main = r'''
int main(void) {
    char before[sizeof(WL_AP_DAT)];
    assert(h2_jieli_wifi_configure_ap(6, 4, 1) == 0);
    assert(strstr(WL_AP_DAT, "\nChannel=6\n#\n"));
    assert(strstr(WL_AP_DAT, "\nMaxStaNum=4\n"));
    assert(strstr(WL_AP_DAT, "\nHideSSID=1\n"));
    memcpy(before, WL_AP_DAT, sizeof(before));
    assert(h2_jieli_wifi_configure_ap(0, 4, 0) == -1);
    assert(h2_jieli_wifi_configure_ap(15, 4, 0) == -1);
    assert(h2_jieli_wifi_configure_ap(1, 6, 0) == -1);
    assert(h2_jieli_wifi_configure_ap(1, 0, 0) == -1);
    assert(h2_jieli_wifi_configure_ap(1, 1, 2) == -1);
    on = 1;
    assert(h2_jieli_wifi_configure_ap(1, 1, 0) == -1);
    assert(memcmp(before, WL_AP_DAT, sizeof(before)) == 0);
    on = 0;
    assert(h2_jieli_wifi_configure_ap(14, 5, 0) == 0);
    assert(strstr(WL_AP_DAT, "\nChannel=14#\n"));
    assert(strstr(WL_AP_DAT, "\nMaxStaNum=5\n"));
    assert(strstr(WL_AP_DAT, "\nHideSSID=0\n"));
    assert(strstr(WL_AP_DAT, "\nWirelessMode=9\n"));
    return 0;
}
'''
            unit = root / "options.c"
            binary = root / "options-test"
            unit.write_text(fixture + fields + function + main)
            subprocess.run(["cc", str(unit), "-o", str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=10)

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
