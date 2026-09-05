"""Compile the board's actual preference commit path against injected I/O failures."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_pref.c"


class PreferenceCommitTest(unittest.TestCase):
    def test_concurrent_first_use_and_failed_create_retry(self):
        source = SOURCE.read_text()
        functions = source[source.index("static int pref_lock(void)"):
                           source.index("static int pref_region_erased(void)")]
        stub = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
enum { OS_NO_ERR=0, H2_PAL_OK=0, H2_PAL_ERR_IO=-1 };
static pthread_mutex_t pref_mutex;
static int pref_mutex_ready;
static atomic_int started, go, creates, inside;
static int protected_count;
static void pref_trace(const char *text) { (void)text; }
static void os_time_dly(unsigned ticks) {
  struct timespec delay={0,(long)ticks*1000000L};
  while (nanosleep(&delay,&delay)!=0) assert(errno==EINTR);
}
static int os_mutex_create(pthread_mutex_t *mutex) {
  int attempt=atomic_fetch_add(&creates,1);
  os_time_dly(5);
  if (attempt==0) return -1;
  /* Any attempt beyond this is a duplicate initialization of a live mutex. */
  assert(attempt==1);
  return pthread_mutex_init(mutex,NULL);
}
static int os_mutex_pend(pthread_mutex_t *mutex, unsigned timeout) {
  (void)timeout; return pthread_mutex_lock(mutex);
}
static int os_mutex_post(pthread_mutex_t *mutex) { return pthread_mutex_unlock(mutex); }
'''
        main = r'''
static void *worker(void *unused) {
  (void)unused;
  atomic_fetch_add(&started,1);
  while (!atomic_load(&go)) os_time_dly(1);
  for (int i=0;i<100;++i) {
    int rc;
    do { rc=pref_lock(); } while (rc==H2_PAL_ERR_IO);
    assert(rc==0 && atomic_fetch_add(&inside,1)==0);
    ++protected_count;
    assert(atomic_fetch_sub(&inside,1)==1);
    pref_unlock();
  }
  return NULL;
}
int main(void) {
  pthread_t threads[8];
  for (int i=0;i<8;++i) assert(pthread_create(&threads[i],NULL,worker,NULL)==0);
  while (atomic_load(&started)!=8) os_time_dly(1);
  atomic_store(&go,1);
  for (int i=0;i<8;++i) assert(pthread_join(threads[i],NULL)==0);
  assert(atomic_load(&creates)==2 && protected_count==800);
  assert(pthread_mutex_destroy(&pref_mutex)==0);
  return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-pref-init-") as directory:
            test = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            test.write_text(stub + functions + main)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-pthread", str(test), "-o", str(binary)],
                           check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=60)

    def test_iteration_and_clear_failures(self):
        source = SOURCE.read_text()
        types = source[source.index("enum {"):source.index("static lfs_t pref_lfs;")]
        helpers = source[source.index("static int require_writable("):
                         source.index("static int read_record(")]
        operations = source[source.index("static int pref_clear("):
                            source.index("static void initialize_namespace(")]
        prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
typedef struct { void *user; } h2_pal_pref_namespace_t;
typedef int h2_pal_pref_open_mode_t;
typedef int h2_pal_pref_entry_type_t;
typedef int lfs_dir_t;
typedef int lfs_file_t;
typedef int lfs_ssize_t;
typedef struct h2_pal_pref_cursor h2_pal_pref_cursor_t;
typedef struct { const char *key; int type; size_t value_size; } h2_pal_pref_entry_t;
struct lfs_info { char name[256]; };
enum { H2_PAL_PREF_OPEN_READ_WRITE=1, H2_PAL_OK=0, H2_PAL_ERR_IO=-1,
       H2_PAL_ERR_INVALID_ARG=-2, H2_PAL_ERR_INVALID_STATE=-3,
       H2_PAL_ERR_NO_MEMORY=-4, H2_PAL_ERR_NOT_FOUND=-5,
       LFS_ERR_OK=0, LFS_O_RDONLY=1 };
'''
        stub = r'''
static int pref_lfs, scenario, locked, dir_closes, file_opens, file_closes, opened, reads;
static int pref_lock(void) { assert(!locked); locked=1; return 0; }
static void pref_unlock(void) { assert(locked); locked=0; }
static int pref_prepare_locked(void) { return 0; }
static int map_lfs_error(int rc) { return rc<0 ? H2_PAL_ERR_IO : 0; }
static int lfs_dir_open(int *fs, lfs_dir_t *dir, const char *path) {
  (void)fs; (void)path; assert(locked); *dir=1; return 0;
}
static int lfs_dir_read(int *fs, lfs_dir_t *dir, struct lfs_info *info) {
  (void)fs; assert(locked && *dir==1);
  if (scenario==3 || scenario==6) return -1;
  if (scenario==7) return 0;
  if (scenario==2) { memset(info->name,'a',200); info->name[200]=0; }
  else strcpy(info->name,"6b6579");
  return 1;
}
static int lfs_dir_close(int *fs, lfs_dir_t *dir) {
  (void)fs; assert(locked && *dir==1); ++dir_closes;
  return scenario==7 || scenario==9 ? -1 : 0;
}
static int lfs_remove(int *fs, const char *path) {
  (void)fs; (void)path; assert(!"clear must stop at read failure"); return 0;
}
static int lfs_file_open(int *fs, lfs_file_t *file, const char *path, int flags) {
  (void)fs; (void)path; (void)flags; assert(locked); ++file_opens;
  if (scenario==1) return -1;
  opened=1; *file=1; return 0;
}
static int lfs_file_read(int *fs, lfs_file_t *file, void *p, size_t n) {
  (void)fs; assert(locked && opened && *file==1); ++reads;
  if (reads==1) {
    jieli_pref_header_t h = {.magic=H2_PREF_MAGIC, .key_size=3, .type=1, .value_size=4};
    if (scenario==8) h.key_size=H2_PREF_KEY_MAX+1;
    assert(n==sizeof(h)); memcpy(p,&h,n);
    return scenario==4 ? (int)n-1 : (int)n;
  }
  assert(n==3); memcpy(p,"key",3); return 3;
}
static int lfs_file_close(int *fs, lfs_file_t *file) {
  (void)fs; (void)file; assert(locked && opened);
  ++file_closes; opened=0;
  return scenario==5 ? -1 : 0;
}
'''
        main = r'''
int main(void) {
  for (scenario=0; scenario<=9; ++scenario) {
    jieli_pref_namespace_t ns = {.path="/abcd", .mode=H2_PAL_PREF_OPEN_READ_WRITE};
    ns.pal.user=&ns;
    locked=dir_closes=file_opens=file_closes=opened=reads=0;
    if (scenario==6 || scenario==7) {
      assert(pref_clear(&ns.pal)==H2_PAL_ERR_IO);
      assert(dir_closes==1 && !locked); continue;
    }
    if (scenario==2) { memset(ns.path,'a',sizeof(ns.path)-1); ns.path[sizeof(ns.path)-1]=0; }
    h2_pal_pref_cursor_t *cursor=NULL;
    h2_pal_pref_entry_t entry={.value_size=123};
    int rc=pref_iterate(&ns.pal,&cursor,&entry);
    assert(rc==((scenario==0 || scenario==9) ? 0 : H2_PAL_ERR_IO));
    assert(!locked && !opened);
    assert(file_opens==((scenario==2 || scenario==3) ? 0 : 1));
    assert(file_closes==((scenario==1 || scenario==2 || scenario==3) ? 0 : 1));
    if (rc==0) assert(strcmp(entry.key,"key")==0 && entry.value_size==4);
    else assert(entry.value_size==123);
    assert(pref_iterate_close(&ns.pal,&cursor)==(scenario==9 ? H2_PAL_ERR_IO : 0));
    assert(cursor==NULL && dir_closes==1 && !locked);
    assert(pref_commit(&ns.pal)==0);
  }
  return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-pref-iterate-") as directory:
            test = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            test.write_text(prefix + types + stub + helpers + operations + main)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(test), "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=60)

    def test_replace_keeps_old_record_until_rename(self):
        source = SOURCE.read_text()
        types = source[source.index("enum {"):source.index("struct h2_pal_pref_cursor")]
        paths = source[source.index("static size_t bounded_length("):
                       source.index("static jieli_pref_namespace_t *to_namespace(")]
        write = source[source.index("static int write_record("):
                       source.index("static int pref_close(")]
        stub = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int h2_pal_pref_namespace_t;
typedef int h2_pal_pref_open_mode_t;
typedef int h2_pal_pref_entry_type_t;
typedef int lfs_file_t;
typedef int lfs_ssize_t;
enum { H2_PAL_PREF_OPEN_READ_WRITE=1, H2_PAL_OK=0,
       H2_PAL_ERR_INVALID_ARG=-1, H2_PAL_ERR_INVALID_STATE=-2,
       H2_PAL_ERR_IO=-3, LFS_O_WRONLY=1, LFS_O_CREAT=2, LFS_O_TRUNC=4 };
static int pref_lfs, locked, failure, writes, synced, closed, renamed;
static int old_value, old_removes, temp_exists;
static int pref_lock(void) { assert(!locked); locked=1; return 0; }
static void pref_unlock(void) { assert(locked); locked=0; }
static int pref_prepare_locked(void) { assert(locked); return 0; }
static int map_lfs_error(int rc) { return rc < 0 ? H2_PAL_ERR_IO : 0; }
static int is_temp(const char *path) {
  size_t length=strlen(path);
  return length>=4 && strcmp(path+length-4,".tmp")==0;
}
static int lfs_file_open(int *fs, lfs_file_t *file, const char *path, int flags) {
  (void)fs; (void)file;
  assert(locked && is_temp(path) && flags==7);
  if (failure==1) return -1;
  temp_exists=1; return 0;
}
static lfs_ssize_t lfs_file_write(int *fs, lfs_file_t *file, const void *p, size_t n) {
  (void)fs; (void)file; (void)p;
  assert(locked && temp_exists && !closed);
  ++writes;
  return failure==writes+1 ? -1 : (lfs_ssize_t)n;
}
static int lfs_file_sync(int *fs, lfs_file_t *file) {
  (void)fs; (void)file;
  assert(locked && writes==3); synced=1;
  return failure==5 ? -1 : 0;
}
static int lfs_file_close(int *fs, lfs_file_t *file) {
  (void)fs; (void)file;
  assert(locked && !closed); closed=1;
  return failure==6 ? -1 : 0;
}
static int lfs_remove(int *fs, const char *path) {
  (void)fs; assert(locked);
  if (is_temp(path)) temp_exists=0;
  else { ++old_removes; old_value=0; }
  return 0;
}
static int lfs_rename(int *fs, const char *oldpath, const char *newpath) {
  (void)fs;
  assert(locked && synced && closed && temp_exists);
  assert(is_temp(oldpath) && !is_temp(newpath));
  ++renamed;
  if (failure==7) return -1;
  old_value=99; temp_exists=0; return 0;
}
'''
        main = r'''
int main(void) {
  jieli_pref_namespace_t ns = {.path="/abcd", .mode=H2_PAL_PREF_OPEN_READ_WRITE};
  const char value[]="new value";
  for (failure=0; failure<=7; ++failure) {
    old_value=42; old_removes=temp_exists=locked=writes=synced=closed=renamed=0;
    int rc=write_record(&ns,"key",1,value,sizeof(value));
    assert(rc==(failure==0 ? H2_PAL_OK : H2_PAL_ERR_IO));
    assert(!locked && old_removes==0);
    assert(old_value==(failure==0 ? 99 : 42));
    assert(renamed==((failure==0 || failure==7) ? 1 : 0));
    assert(closed==(failure==1 ? 0 : 1));
  }
  /* Both individually advertised maxima must also work together, including
   * the temporary-file suffix. One extra key byte remains invalid. */
  failure=0; old_value=42;
  old_removes=temp_exists=locked=writes=synced=closed=renamed=0;
  memset(ns.path,'a',sizeof(ns.path)-1); ns.path[0]='/'; ns.path[sizeof(ns.path)-1]=0;
  char key[H2_PREF_KEY_MAX+2];
  memset(key,'k',H2_PREF_KEY_MAX); key[H2_PREF_KEY_MAX]=0;
  assert(write_record(&ns,key,1,value,sizeof(value))==H2_PAL_OK);
  assert(old_value==99 && !locked && old_removes==0);
  key[H2_PREF_KEY_MAX]='k'; key[H2_PREF_KEY_MAX+1]=0;
  assert(write_record(&ns,key,1,value,sizeof(value))==H2_PAL_ERR_INVALID_ARG);
  return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-pref-commit-") as directory:
            test = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            test.write_text(stub + types + paths + write + main)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(test), "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    unittest.main()
