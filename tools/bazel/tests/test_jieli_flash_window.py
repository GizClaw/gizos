"""Real disk/Pref/updater protection windows, with deterministic pthread overlap."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
BOARD = ROOT / "boards/jieli_ac791n_devkit/ac791n"
BASELINE = os.environ.get("JIELI_WINDOW_BASELINE")


def source(path):
    if BASELINE:
        return subprocess.check_output(
            ["git", "show", f"{BASELINE}:{path.relative_to(ROOT)}"], text=True)
    return path.read_text()


class FlashWindowTest(unittest.TestCase):
    def test_providers_restore_and_share_protection(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            for d in ("asm", "device", "os"):
                (p / d).mkdir()
            (p / "asm/sfc_norflash_api.h").write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint32_t u32;
typedef uint8_t u8;
struct device;
int norflash_ioctl(struct device *,u32,u32);
int norflash_write(struct device *,void *,u32,u32);
int norflash_read(struct device *,void *,u32,u32);
int norflash_origin_read(u8 *,u32,u32);
int norflash_protect_suspend(void);
int norflash_protect_resume(void);
''')
            (p / "device/ioctl_cmds.h").write_text('''#pragma once
#define IOCTL_SET_WRITE_PROTECT 13u
#define IOCTL_GET_WRITE_PROTECT_VALUE 47u
#define IOCTL_ERASE_SECTOR 102u
#define IOCTL_ERASE_BLOCK 101u
#define IOCTL_ERASE_PAGE 103u
''')
            (p / "os/os_api.h").write_text('''#pragma once
#include <pthread.h>
#include <sched.h>
typedef pthread_mutex_t OS_MUTEX;
#define OS_NO_ERR 0
static inline int os_mutex_create(OS_MUTEX *m) { return pthread_mutex_init(m,0); }
static inline int os_mutex_pend(OS_MUTEX *m,unsigned t) {(void)t;return pthread_mutex_lock(m);}
static inline int os_mutex_post(OS_MUTEX *m) {return pthread_mutex_unlock(m);}
static inline void os_time_dly(unsigned t) {(void)t;sched_yield();}
''')
            (p / "app_config.h").write_text("")
            (p / "disk.c").write_text(source(BOARD / "src/h2_jieli_ac791n_devkit_disk.c"))
            pref = source(BOARD / "src/h2_jieli_ac791n_devkit_pref.c")
            (p / "pref.c").write_text(pref[pref.index("static int pref_flash_read("):
                                               pref.index("static int pref_flash_sync(")])
            (p / "adapter.c").write_text(source(BOARD / "layouts/h2loader/src/jieli_upgrade_io.c"))
            # Always compile the new helper; before runs use the old consumers.
            (p / "window.c").write_text((BOARD / "src/h2_jieli_ac791n_devkit_flash_window.c").read_text())
            (p / "test.c").write_text(r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "h2_jieli_ac791n_devkit_flash_window.h"
#include "window.c"
#include "disk.c"
typedef uint32_t lfs_block_t,lfs_off_t,lfs_size_t;
struct lfs_config {uint32_t block_size;};
#define LFS_ERR_IO -5
#define LFS_ERR_OK 0
#include "pref.c"
#include "adapter.c"
static pthread_mutex_t fake_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static u32 protection=0x9e00u, sdk_saved;
static int fault, overlap, inflight, arrived, first_done;
static _Thread_local int worker_id;
static u32 base=H2_JIELI_BANK_1_SFC_BASE;
u32 boot_info_get_sfc_base_addr(void) {return base;}
int norflash_ioctl(struct device *dev,u32 command,u32 arg) {
  (void)dev;assert(!pthread_mutex_lock(&fake_mutex));int rc=0;
  if(command==IOCTL_GET_WRITE_PROTECT_VALUE) {
    /* Map the SDK's 32-bit pointer ABI to this host's actual object. */
    assert(arg==(uint32_t)(uintptr_t)&window_saved);
    if(fault==1) rc=-1;else window_saved=protection;
  } else if(command==IOCTL_SET_WRITE_PROTECT) {
    if(arg && arg!=0x10000u) assert(inflight==0);
    if(!(arg && arg!=0x10000u && fault==6))
      protection=arg ? arg : 0x10000u;
    if(!arg && fault==2) rc=-1; /* failed SET may already have mutated SR */
    if(arg && arg!=0x10000u && (fault==5 || fault==6)) rc=-1;
  } else {
    assert(protection==0x10000u);
    if(fault==3)rc=-1;
  }
  assert(!pthread_mutex_unlock(&fake_mutex));return rc;
}
int norflash_protect_suspend(void) {
  sdk_saved=protection;
  return norflash_ioctl(NULL,IOCTL_SET_WRITE_PROTECT,0);
}
int norflash_protect_resume(void) {
  return norflash_ioctl(NULL,IOCTL_SET_WRITE_PROTECT,sdk_saved);
}
int norflash_read(struct device *dev,void *buf,u32 len,u32 addr) {
  (void)dev;(void)addr;memset(buf,0xff,len);return (int)len;
}
int norflash_origin_read(u8 *buf,u32 addr,u32 len) {
  return norflash_read(NULL,buf,len,addr);
}
int norflash_write(struct device *dev,void *buf,u32 len,u32 addr) {
  (void)dev;(void)buf;(void)addr;
  assert(!pthread_mutex_lock(&fake_mutex));assert(protection==0x10000u);
  ++inflight;
  if(overlap) {
    ++arrived;pthread_cond_broadcast(&changed);
    while(arrived<2 || (worker_id==2 && !first_done))
      pthread_cond_wait(&changed,&fake_mutex);
    assert(protection==0x10000u);
  }
  --inflight;
  assert(!pthread_mutex_unlock(&fake_mutex));
  return fault==3 ? -1 : fault==4 ? (int)len-1 : (int)len;
}
static int operation(int which) {
  char data[16]={0};struct lfs_config cfg={4096};
  switch(which) {
    case 0:return write_partition(NULL,H2_JIELI_PARTITION_PREF,0,data,sizeof(data));
    case 1:return erase_partition(NULL,H2_JIELI_PARTITION_PREF,0,4096);
    case 2:return pref_flash_program(&cfg,0,0,data,sizeof(data));
    case 3:return pref_flash_erase(&cfg,0);
    default:return -99;
  }
}
static void *worker(void *arg) {
  worker_id=(int)(uintptr_t)arg;
  assert(operation(worker_id==1 ? 0 : 2)==0);
  if(worker_id==1) {
    pthread_mutex_lock(&fake_mutex);first_done=1;
    pthread_cond_broadcast(&changed);pthread_mutex_unlock(&fake_mutex);
  }
  return NULL;
}
int main(int argc,char **argv) {
  assert(argc==3);int which=atoi(argv[1]);fault=atoi(argv[2]);
  if(which<4) {
    int rc=operation(which);
    assert(protection==(fault==6 ? 0x10000u : 0x9e00u));
    assert(rc==(fault ? (which<2 ? H2_PAL_ERR_IO : LFS_ERR_IO) : 0));
    if(fault==5 || fault==6) {
      h2_jieli_flash_window_t denied={0};
      assert(h2_jieli_flash_window_open(&denied)==-1 && !denied.active);
    }
  } else {
    /* The first close must not restore while the second writer is blocked. */
    overlap=1;pthread_t a,b;
    assert(!pthread_create(&a,NULL,worker,(void *)1));
    assert(!pthread_create(&b,NULL,worker,(void *)2));
    assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));
    assert(protection==0x9e00u);
    /* The SDK outer lease can contain provider and adapter inner leases. */
    dev_upgrade_protect_suspend();
    arrived=first_done=0;
    assert(!pthread_create(&a,NULL,worker,(void *)1));
    assert(!pthread_create(&b,NULL,worker,(void *)2));
    assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));
    overlap=0;assert(operation(0)==0 && operation(2)==0);
    assert(protection==0x10000u);
    char data[16]={0};
    assert(dev_upgrade_write((u8 *)data,H2_JIELI_BANK_2_SFC_BASE,16)==16);
    assert(dev_upgrade_erase(2,H2_JIELI_BANK_2_SFC_BASE)==1);
    dev_upgrade_protect_resume();assert(protection==0x9e00u);
  }
  /* Make the extracted read entry point part of the fixture too. */
  char data[16];struct lfs_config cfg={4096};
  assert(pref_flash_read(&cfg,0,0,data,sizeof(data))==0);
  return 0;
}
''')
            binary = p / "test"
            flags = ["-fsanitize=thread", "-g"] if os.environ.get("JIELI_TSAN") else []
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                            *flags, "-I", str(p), "-I", str(BOARD / "include"),
                            "-I", str(BOARD / "layouts/h2loader/include"),
                            "-I", str(ROOT / "libs/pal/include"),
                            "-I", str(ROOT / "native_component_src/jieli/wl82/h2_pal_core/include"),
                            str(p / "test.c"), "-o", str(binary)], check=True)
            for which in range(5):
                for fault in ((0, 1, 2, 3, 4, 5, 6) if which in (0, 2) else
                              (0, 1, 2, 3, 5, 6) if which < 4 else (0,)):
                    with self.subTest(provider=which, fault=fault):
                        subprocess.run([str(binary), str(which), str(fault)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
