"""Exercise production reboot scheduling without touching retained hardware RAM."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class LoaderRebootTest(unittest.TestCase):
    def test_request_is_published_only_by_successfully_scheduled_reset(self):
        source = (ROOT / "projects/h2loader/targets/h2loader_tar_zlib/loader/"
                  "jieli_ac791n_devkit/src/jieli_loader_platform.c").read_text()
        functions = source[source.index("static void power_reboot_timer("):
                           source.index("int h2_jieli_loader_platform_init(")]
        stub = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define H2_PAL_OK 0
#define H2_PAL_ERR_INVALID_STATE -1
#define H2_PAL_ERR_NO_MEMORY -2
#define H2_JIELI_PARTITION_APP 2
#define H2_JIELI_BANK_2_SFC_BASE 0x37c020u
static struct { unsigned next_partition_id, running_partition_id;
  int update_committed; unsigned warm_partition_id, reboot_timer_id; } state;
static unsigned allocation, publications, resets;
static void (*callback)(void *);
static void *argument;
static void h2_jieli_loader_diag_write(const char *s) { (void)s; }
static void h2_jieli_warm_boot_request(uint32_t base) {
  assert(base == H2_JIELI_BANK_2_SFC_BASE); ++publications;
}
static void system_reset(void) { ++resets; }
static unsigned sys_timeout_add_to_task(const char *task, void *user,
    void (*fn)(void *), unsigned delay) {
  assert(strcmp(task,"sys_timer")==0 && delay==2000);
  callback=fn; argument=user; return allocation;
}
'''
        main = r'''
int main(void) {
  state.running_partition_id=1; state.next_partition_id=2;
  state.warm_partition_id=2;
  assert(power_reboot(NULL,0)==H2_PAL_ERR_NO_MEMORY);
  assert(publications==0 && resets==0 && state.reboot_timer_id==0);
  allocation=7;
  assert(power_reboot(NULL,0)==0 && publications==0 && resets==0);
  assert(power_reboot(NULL,0)==0 && publications==0);
  /* The callback uses the scheduled destination, not mutable later state. */
  state.warm_partition_id=0;
  callback(argument); assert(publications==1 && resets==1);
  state.reboot_timer_id=0; state.next_partition_id=1;
  assert(power_reboot(NULL,0)==0); callback(argument);
  assert(publications==1 && resets==2);
  state.reboot_timer_id=0; state.next_partition_id=2;
  assert(power_reboot(NULL,0)==H2_PAL_ERR_INVALID_STATE);
  assert(publications==1 && resets==2);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(stub + functions + main)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Werror",
                            str(path / "test.c"), "-o", str(path / "test")],
                           check=True, timeout=60)
            subprocess.run([str(path / "test")], check=True, timeout=10)
