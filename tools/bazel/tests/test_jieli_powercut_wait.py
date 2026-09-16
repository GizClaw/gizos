"""Check the actual burn-wait branch without changing production timeouts."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

class PowercutWaitTest(unittest.TestCase):
    def test_timeout_and_diagnostic_hold(self):
        source = (ROOT / "projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit/src/jieli_loader_platform.c").read_text()
        branch = source[source.index("  int pend_rc = OS_NO_ERR;"):source.index("  __atomic_store_n(&state.burn_waiting, 0", source.index("  int pend_rc = OS_NO_ERR;"))]
        stub = r'''
#include <assert.h>
#include <setjmp.h>
#define OS_NO_ERR 0
#define H2_PAL_ERR_IO -5
#define H2_PAL_ERR_TIMEOUT -6
#define H2_JIELI_UPDATE_WAIT_TICKS 500
static struct { int update_sem; } state;
static int held, waits, sleeps, timed_out;
static jmp_buf paused;
static int os_sem_pend(int *sem, int ticks) {
 assert(sem==&state.update_sem && ticks==500); waits++; return timed_out;
}
static int h2_jieli_loader_powercut_paused(void) { return held; }
static void os_time_dly(unsigned ticks) {
 assert(ticks==100); if(++sleeps==3)longjmp(paused,1);
}
static int run(unsigned burn_rc) {
 int rc=0;
'''
        main = r'''
 return rc;
}
int main(void) {
 assert(run(1)==H2_PAL_ERR_IO && waits==0);
 assert(run(0)==0 && waits==1 && sleeps==0);
 timed_out=1;
 assert(run(0)==H2_PAL_ERR_TIMEOUT && waits==2 && sleeps==0);
 held=1;
 if(!setjmp(paused)){run(0);assert(0);}
 assert(waits==3 && sleeps==3);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(stub + branch + main)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(path / "test.c"), "-o", str(path / "test")], check=True, timeout=60)
            subprocess.run([str(path / "test")], check=True, timeout=10)
