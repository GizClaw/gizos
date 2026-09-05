"""The shared reboot transaction must be able to read the App's next bank."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
TARGET = ROOT / "projects/example/targets/jieli_firmware/display/jieli_ac791n_devkit/src"
STUB = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define H2_PAL_OK 0
#define H2_PAL_ERR_INVALID_ARG -1
#define H2_JIELI_PARTITION_LOADER 1u
#define H2_JIELI_PARTITION_APP 2u
#define H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE 1u
#define H2_PAL_POWER_BOOT_PARTITION_FLAG_NEXT 2u
#define H2_PAL_POWER_BOOT_PARTITION_FLAG_RECOVERY 4u
#define H2_PAL_POWER_BOOT_PARTITION_FLAG_APP 8u
typedef struct { uint32_t id, flags; char name[16]; } h2_pal_power_boot_partition_t;
static uint32_t next_boot_partition=2;
'''


class AppPowerTest(unittest.TestCase):
    def test_both_app_targets_expose_next_partition(self):
        for name, prefix in (("color_bar_pal.c", "app_power"),
                             ("mp4_player_small_pal.c", "power")):
            with self.subTest(target=name):
                source = (TARGET / name).read_text()
                self.assertIn(f".get_next_boot_partition = {prefix}_get_next", source)
                start = source.index(f"static int {prefix}_get_next(")
                end = source.index(f"static int {prefix}_set_next(", start)
                main = r'''
int main(void) {
 h2_pal_power_boot_partition_t p;
 assert(GET_NEXT(NULL,NULL)==-1);
 memset(&p,0xff,sizeof(p));
 assert(GET_NEXT(NULL,&p)==0 && p.id==2 && p.flags==11);
 assert(strcmp(p.name,"app")==0);
 next_boot_partition=1;
 assert(GET_NEXT(NULL,&p)==0 && p.id==1 && p.flags==7);
 assert(strcmp(p.name,"h2loader")==0);
 return 0;
}
'''.replace("GET_NEXT", f"{prefix}_get_next")
                with tempfile.TemporaryDirectory(prefix="h2-app-power-") as directory:
                    test = Path(directory) / "test.c"
                    test.write_text(STUB + source[start:end] + main)
                    binary = Path(directory) / "test"
                    subprocess.run(["cc", "-std=c11", str(test), "-o", str(binary)],
                                   check=True, timeout=60)
                    subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    unittest.main()
