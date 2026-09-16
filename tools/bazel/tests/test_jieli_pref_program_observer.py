"""Exercise the actual NOR program adapter with and without a diagnostic hook."""
from pathlib import Path
import subprocess
import os
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_pref.c'


class ObserverTest(unittest.TestCase):
    def test_program_observer(self):
        baseline = os.environ.get("JIELI_WINDOW_BASELINE")
        source = subprocess.check_output(
            ["git", "show", f"{baseline}:{SOURCE.relative_to(ROOT)}"], text=True
        ) if baseline else SOURCE.read_text()
        adapter = source[source.index('static int pref_flash_read('):
                         source.index('static int pref_flash_sync(')]
        stub = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef uint32_t lfs_block_t;
typedef uint32_t lfs_off_t;
typedef uint32_t lfs_size_t;
struct lfs_config { uint32_t block_size; };
#define H2_JIELI_PREF_SIZE 8192u
#define H2_JIELI_PREF_ADDRESS 0x10000u
#define LFS_ERR_IO -5
#define LFS_ERR_OK 0
#define IOCTL_SET_WRITE_PROTECT 1
#define IOCTL_ERASE_SECTOR 2
int writes, observed, fail, reads, erases, window_active;
/* This fixture isolates observer order; real protection is in flash_window. */
typedef struct { int active; } h2_jieli_flash_window_t;
int h2_jieli_flash_window_open(h2_jieli_flash_window_t *w) {w->active=1;window_active=1;return 0;}
int h2_jieli_flash_window_close(h2_jieli_flash_window_t *w) {w->active=0;window_active=0;return 0;}
static int norflash_ioctl(void *p, int cmd, unsigned arg) {
  (void)p;
  if (cmd == IOCTL_ERASE_SECTOR) {
    assert(window_active);
    assert(arg == 0x11000u); ++erases;
    return fail ? -1 : 0;
  }
  return 0;
}
static int norflash_read(void *p, void *buf, unsigned n, unsigned addr) {
  (void)p; assert(buf && n==256 && addr==0x11000);
  ++reads; return fail ? 0 : (int)n;
}
static int norflash_write(void *p, void *buf, unsigned n, unsigned addr) {
  assert(window_active);
  (void)p; assert(buf && n==256 && addr==0x11000);
  ++writes; return fail ? 0 : (int)n;
}
'''
        hook = r'''
#include <assert.h>
#include <stdint.h>
extern int writes, observed;
void h2_jieli_pref_program_observer(uint32_t addr, const void *buf, uint32_t n) {
  assert(writes==observed && addr==0x11000 && buf && n==256);
  ++observed;
}
'''
        main = r'''
int main(void) {
  struct lfs_config cfg={4096}; char data[256]={0};
  assert(pref_flash_program(&cfg,2,0,data,256)==LFS_ERR_IO);
  assert(pref_flash_program(&cfg,0x100001u,0,data,256)==LFS_ERR_IO);
  assert(pref_flash_program(&cfg,0,4097,data,256)==LFS_ERR_IO);
  assert(pref_flash_program(&cfg,0,4090,data,256)==LFS_ERR_IO);
  struct lfs_config invalid={0};
  assert(pref_flash_program(&invalid,0,0,data,256)==LFS_ERR_IO);
  assert(pref_flash_read(&cfg,2,0,data,256)==LFS_ERR_IO);
  assert(pref_flash_read(&cfg,0x100001u,0,data,256)==LFS_ERR_IO);
  assert(pref_flash_read(&cfg,0,4090,data,256)==LFS_ERR_IO);
  assert(pref_flash_read(&invalid,0,0,data,256)==LFS_ERR_IO);
  assert(pref_flash_erase(&cfg,2)==LFS_ERR_IO);
  assert(pref_flash_erase(&cfg,0x100001u)==LFS_ERR_IO);
  assert(pref_flash_erase(&invalid,0)==LFS_ERR_IO);
  assert(!reads && !erases);
  assert(pref_flash_read(&cfg,1,0,data,256)==0);
  assert(pref_flash_erase(&cfg,1)==0);
  assert(!writes && !observed);
  assert(pref_flash_program(&cfg,1,0,data,256)==0);
  fail=1;
  assert(pref_flash_read(&cfg,1,0,data,256)==LFS_ERR_IO);
  assert(pref_flash_erase(&cfg,1)==LFS_ERR_IO);
  assert(reads==2 && erases==2);
  assert(pref_flash_program(&cfg,1,0,data,256)==LFS_ERR_IO);
  assert(writes==2 && !window_active);
  assert(observed==EXPECTED);
}
'''
        with tempfile.TemporaryDirectory(prefix='jieli-pref-observer-') as directory:
            path = Path(directory)
            for enabled in (False, True):
                # Link a separate strong definition against the real weak default,
                # rather than substituting the production source in this test.
                (path / 'test.c').write_text(stub + adapter + main)
                (path / 'hook.c').write_text(hook)
                subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                f'-DEXPECTED={2 if enabled else 0}', str(path / 'test.c'),
                                *([str(path / 'hook.c')] if enabled else []),
                                '-o', str(path / 'test')], check=True)
                subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
