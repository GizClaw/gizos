"""Run actual SD init/deinit against borrowed and owned mount failures."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[3]

class MountTest(unittest.TestCase):
    def test_mount_ownership(self):
        text = (ROOT / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_sd_fs.c").read_text()
        code = text[text.index("h2_pal_result_t h2_jieli_ac791n_devkit_sd_fs_init("):]
        stub = r'''
#include <assert.h>
#include <stdlib.h>
#include "h2/pal/os/h2_pal_fs.h"
#define H2_JIELI_SD_MOUNT "mount"
#define H2_JIELI_SD_ROOT "root/"
#define H2_JIELI_SD_FS_TYPE "fat"
#define H2_JIELI_SD_CACHE_COUNT 4
#define fs_mkdir NULL
#define fs_open NULL
#define fs_read NULL
#define fs_seek NULL
#define fs_write NULL
#define fs_sync NULL
#define fs_close NULL
#define fs_stat NULL
#define fs_clear NULL
#define fs_remove NULL
#define fs_rename NULL
static int sd_mounted, sd_mount_owned;
static const char *sd_last_stage;
static int sd_heap_probe_32k, sd_heap_probe_16k;
static int existing, unmounts, mounts, fail_directory, fail_unmount;
struct imount { int dummy; };
static struct imount volume;
static int wait_sd_online(void) { return 0; }
static int fmount_exist(const char *path) { (void)path; return existing; }
static struct imount *mount(const char *d,const char *p,const char *t,int c,void *u) {
 (void)d;
 (void)p;
 (void)t;
 (void)c;
 (void)u;
 ++mounts;
 existing=1;
 return &volume;
}
static int unmount(const char *path) {
 (void)path;
 ++unmounts;
 if (fail_unmount) return -1;
 existing=0;
 return 0;
}
static int ensure_directory(const char *path) { (void)path; return fail_directory ? -1 : 0; }
static void capture_sd_diagnostic(void) {}
static int map_error(int rc) { return rc==0 ? H2_PAL_OK : H2_PAL_ERR_IO; }
'''
        main = r'''
int main(void) {
 h2_pal_fs_api_t api={0};
 (void)sd_mount_owned;
 existing=1;
 fail_directory=1;
 assert(h2_jieli_ac791n_devkit_sd_fs_init(&api)==H2_PAL_ERR_IO);
 assert(unmounts==0 && existing && !sd_mounted && api.vtable==NULL);
 fail_directory=0;
 assert(h2_jieli_ac791n_devkit_sd_fs_init(&api)==H2_PAL_OK);
 assert(h2_jieli_ac791n_devkit_sd_fs_deinit()==H2_PAL_OK);
 assert(unmounts==0 && existing);
 existing=0;
 fail_directory=1;
 fail_unmount=1;
 assert(h2_jieli_ac791n_devkit_sd_fs_init(&api)==H2_PAL_ERR_IO);
 assert(mounts==1 && unmounts==1 && existing && !sd_mounted);
 fail_unmount=0;
 assert(h2_jieli_ac791n_devkit_sd_fs_deinit()==H2_PAL_OK);
 assert(unmounts==2 && !existing);
 fail_directory=0;
 assert(h2_jieli_ac791n_devkit_sd_fs_init(&api)==H2_PAL_OK);
 assert(mounts==2);
 assert(h2_jieli_ac791n_devkit_sd_fs_deinit()==H2_PAL_OK);
 assert(unmounts==3 && !existing);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(stub + code + main)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "libs/pal/include"), str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True, timeout=10)

if __name__ == "__main__":
    unittest.main()
