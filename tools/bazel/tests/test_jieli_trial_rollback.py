"""Verify that JieLi reports a returned, unconfirmed trial as non-bootable."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit/src/jieli_loader_platform.c"

STUB = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define H2_PAL_OK 0
#define H2_PAL_ERR_NOT_FOUND -4
#define H2_JIELI_PARTITION_LOADER 1
#define H2_JIELI_PARTITION_APP 2
#define H2_LOADER_IMAGE_ROLE_APP 2
#define H2_LOADER_PREF_NAMESPACE "h2loader"
#define H2_JIELI_TRIAL_CHECKSUM_KEY "jieli_trial_checksum"
#define H2_JIELI_TRIAL_RESET_REASON_KEY "jieli_trial_reset_reason"
#define H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE 1u
typedef int h2_pal_pref_api_t;
typedef int h2_pal_mem_api_t;
typedef struct { int valid, role; char image_checksum[65]; } metadata_t;
typedef struct { metadata_t stage, partition_2; } h2_loader_status_t;
typedef struct { uint32_t id, flags; char name[16]; } h2_pal_power_boot_partition_t;
typedef struct ns ns_t;
struct ns {
 int (*get_string)(ns_t *, const int *, const char *, char **);
 int (*get_u32)(ns_t *, const char *, uint32_t *);
 int (*remove)(ns_t *, const char *);
 int (*commit)(ns_t *);
 int (*close)(ns_t *);
};
typedef ns_t h2_pal_pref_namespace_t;
#define H2_PAL_PREF_OPEN_READ_WRITE 1
static h2_loader_status_t status_value;
static const char *stored_checksum;
static struct { unsigned running_partition_id; int app_trial_rolled_back; } state;
static int removed, freed;
static int get_string(ns_t *n, const int *a, const char *k, char **out) {
 if (!stored_checksum) return H2_PAL_ERR_NOT_FOUND;
 *out = malloc(strlen(stored_checksum)+1); strcpy(*out,stored_checksum); return 0;
}
static int get_u32(ns_t *n, const char *k, uint32_t *v) { return -4; }
static int remove_key(ns_t *n, const char *k) { ++removed; return 0; }
static int noop(ns_t *n) { return 0; }
static ns_t ns = {get_string,get_u32,remove_key,noop,noop};
static int h2_loader_read_pref_status(const int *p, const int *a, h2_loader_status_t *s) {
 *s=status_value; return 0;
}
static int h2_pal_pref_open(const int *p,const char *name,int mode,ns_t **out) {
 *out=&ns; return 0;
}
static int h2_loader_metadata_image_equal(const metadata_t *a,const metadata_t *b) {
 return strcmp(a->image_checksum,b->image_checksum)==0;
}
static void h2_pal_mem_free(const int *a,void *p) { ++freed; free(p); }
static void h2_jieli_loader_diag_write(const char *s) {}
static void reset(void) {
 memset(&status_value,0,sizeof(status_value));
 status_value.stage.valid=status_value.partition_2.valid=1;
 status_value.stage.role=status_value.partition_2.role=2;
 strcpy(status_value.stage.image_checksum,"checksum-A");
 strcpy(status_value.partition_2.image_checksum,"checksum-A");
 stored_checksum="checksum-A"; state.running_partition_id=1;
 state.app_trial_rolled_back=removed=freed=0;
}
'''

MAIN = r'''
int main(void) {
 h2_pal_power_boot_partition_t p;
 reset(); reconcile_trial_state(NULL,NULL);
 assert(state.app_trial_rolled_back && freed==0);
 fill_partition(&p,2,8); assert(!(p.flags&1) && (p.flags&8));
 fill_partition(&p,1,4); assert((p.flags&1) && (p.flags&4));
 reset(); stored_checksum=NULL; reconcile_trial_state(NULL,NULL);
 assert(state.app_trial_rolled_back);
 reset(); stored_checksum="older-image"; reconcile_trial_state(NULL,NULL);
 assert(state.app_trial_rolled_back && freed==0);
 reset(); strcpy(status_value.stage.image_checksum,"new-image");
 reconcile_trial_state(NULL,NULL); assert(!state.app_trial_rolled_back);
 reset(); state.running_partition_id=2; reconcile_trial_state(NULL,NULL);
 assert(!state.app_trial_rolled_back);
 reset(); status_value.stage.valid=0; reconcile_trial_state(NULL,NULL);
 assert(!state.app_trial_rolled_back && removed==2);
 fill_partition(&p,2,8); assert(p.flags&1);
 return 0;
}
'''


class TrialRollbackTest(unittest.TestCase):
    def test_actual_pal_trial_reconciliation_and_partition_flags(self):
        source = SOURCE.read_text()
        reconcile = source[source.index("static void reconcile_trial_state("):
                           source.index("static int image_path(")]
        fill = source[source.index("static void fill_partition("):
                      source.index("static int power_list(")]
        with tempfile.TemporaryDirectory(prefix="h2-trial-test-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(STUB + reconcile + fill + MAIN)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", str(test), "-o", str(binary)],
                           check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    unittest.main()
