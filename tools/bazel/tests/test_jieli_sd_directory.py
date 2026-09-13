"""Verify SD mkdir does not turn native errors into success."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_sd_fs.c"


class SdDirectoryTest(unittest.TestCase):
    def test_existing_empty_directory_and_create_failures(self):
        source = SOURCE.read_text()
        begin = source.index("static int ensure_directory(")
        end = source.index("static int fs_mkdir(", begin)
        fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <string.h>
#define H2_JIELI_SD_PATH_MAX 192u
#define H2_JIELI_SD_ROOT "storage/sd0/C/"
#define H2_PAL_OK 0
#define H2_PAL_ERR_INVALID_ARG -1
#define H2_PAL_ERR_IO -2
static int exists, create_rc, concurrent_create, creates;
static int fdir_exist(const char *path) {
    assert(strcmp(path, H2_JIELI_SD_ROOT "dl") == 0);
    return exists;
}
static int fmk_dir(const char *root, char *folder, unsigned mode) {
    assert(strcmp(root, H2_JIELI_SD_ROOT) == 0);
    assert(strcmp(folder, "/dl") == 0 && mode == 0);
    ++creates;
    if (concurrent_create) exists = 1;
    return create_rc;
}
'''
        main = r'''
int main(void) {
    exists = 1; create_rc = -1;
    assert(ensure_directory(H2_JIELI_SD_ROOT "dl") == 0 && creates == 0);
    exists = 0; create_rc = 0;
    assert(ensure_directory(H2_JIELI_SD_ROOT "dl") == 0 && creates == 1);
    create_rc = -1;
    assert(ensure_directory(H2_JIELI_SD_ROOT "dl") == H2_PAL_ERR_IO);
    concurrent_create = 1;
    assert(ensure_directory(H2_JIELI_SD_ROOT "dl") == 0);
    assert(ensure_directory("outside/dl") == H2_PAL_ERR_INVALID_ARG);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-sd-directory-") as directory:
            root = Path(directory)
            unit = root / "directory.c"
            unit.write_text(fixture + source[begin:end] + main)
            binary = root / "directory-test"
            subprocess.run(["cc", "-std=c11", str(unit), "-o", str(binary)],
                           check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
