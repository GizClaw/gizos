"""Verify JieLi trial evidence: only an attempted, unconfirmed App rolls back."""
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
#define H2_LOADER_IMAGE_ROLE_H2LOADER 1
#define H2_LOADER_PREF_NAMESPACE "h2loader"
#define H2_JIELI_TRIAL_ATTEMPT_KEY "jieli_trial_attempt"
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
 int (*set_string)(ns_t *, const char *, const char *);
 int (*remove)(ns_t *, const char *);
 int (*commit)(ns_t *);
 int (*close)(ns_t *);
};
typedef ns_t h2_pal_pref_namespace_t;
#define H2_PAL_PREF_OPEN_READ_WRITE 1
static h2_loader_status_t status_value;
static const char *stored_attempt;
static char written_attempt[65];
static struct {
 unsigned running_partition_id; int app_trial_rolled_back;
 const int *pref; const int *allocator;
} state;
static int removed, freed, commits;
static int get_string(ns_t *n, const int *a, const char *k, char **out) {
 assert(strcmp(k, H2_JIELI_TRIAL_ATTEMPT_KEY) == 0);
 if (!stored_attempt) return H2_PAL_ERR_NOT_FOUND;
 *out = malloc(strlen(stored_attempt)+1); strcpy(*out,stored_attempt); return 0;
}
static int set_string(ns_t *n, const char *k, const char *v) {
 assert(strcmp(k, H2_JIELI_TRIAL_ATTEMPT_KEY) == 0);
 strcpy(written_attempt, v); return 0;
}
static int get_u32(ns_t *n, const char *k, uint32_t *v) { return -4; }
static int remove_key(ns_t *n, const char *k) { ++removed; return 0; }
static int noop(ns_t *n) { return 0; }
static int commit(ns_t *n) { ++commits; return 0; }
static ns_t ns = {get_string,get_u32,set_string,remove_key,commit,noop};
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
 stored_attempt="checksum-A"; written_attempt[0]=0;
 state.running_partition_id=1;
 state.app_trial_rolled_back=removed=freed=commits=0;
}
'''

MAIN = r'''
int main(void) {
 h2_pal_power_boot_partition_t p;
 /* The Loader recorded this App attempt and it was never confirmed. */
 reset(); reconcile_trial_state(NULL,NULL);
 assert(state.app_trial_rolled_back && freed==1 && removed==0);
 fill_partition(&p,2,8); assert(!(p.flags&1) && (p.flags&8));
 fill_partition(&p,1,4); assert((p.flags&1) && (p.flags&4));
 /* Stage merely equals old P2 metadata: a restage, not a boot attempt. */
 reset(); stored_attempt=NULL; reconcile_trial_state(NULL,NULL);
 assert(!state.app_trial_rolled_back && removed==0);
 fill_partition(&p,2,8); assert(p.flags&1);
 reset(); stored_attempt="older-image"; reconcile_trial_state(NULL,NULL);
 assert(!state.app_trial_rolled_back && freed==1);
 reset(); strcpy(status_value.stage.image_checksum,"new-image");
 reconcile_trial_state(NULL,NULL);
 /* A new download does not erase evidence of the installed App's failure. */
 assert(state.app_trial_rolled_back && removed==0 && commits==0);
 reset(); stored_attempt="older-image";
 strcpy(status_value.stage.image_checksum,"new-image");
 reconcile_trial_state(NULL,NULL);
 assert(!state.app_trial_rolled_back && removed==3 && commits==1);
 reset(); state.running_partition_id=2; reconcile_trial_state(NULL,NULL);
 assert(!state.app_trial_rolled_back);
 reset(); status_value.stage.valid=0; reconcile_trial_state(NULL,NULL);
 /* Installed-App warm trials have no Stage, but still require confirmation. */
 assert(state.app_trial_rolled_back && removed==0);
 reset(); status_value.stage.valid=0; stored_attempt=NULL;
 reconcile_trial_state(NULL,NULL);
 assert(!state.app_trial_rolled_back && removed==3);
 /* Both App and Loader candidates require an attempted/confirmed lifecycle. */
 reset(); assert(set_trial_attempt(1)==0);
 assert(strcmp(written_attempt,"checksum-A")==0 && commits==1);
 reset(); status_value.partition_2.role=1;
 assert(set_trial_attempt(1)==0 && strcmp(written_attempt,"checksum-A")==0 && commits==1);
 reconcile_trial_state(NULL,NULL); assert(state.app_trial_rolled_back);
 reset(); status_value.partition_2.role=1;state.running_partition_id=2;
 reconcile_trial_state(NULL,NULL); assert(!state.app_trial_rolled_back);
 reset(); status_value.partition_2.role=0;
 assert(set_trial_attempt(1)==0 && written_attempt[0]==0 && commits==0);
 reset(); assert(set_trial_attempt(0)==0 && removed==1 && commits==1);
 return 0;
}
'''


class TrialRollbackTest(unittest.TestCase):
    def test_copy_back_to_p1_does_not_recreate_p2_attempt(self):
        source = SOURCE.read_text()
        self.assertIn("set_trial_attempt(partition_id == H2_JIELI_PARTITION_APP)", source)
        self.assertNotIn("int rc = set_trial_attempt(1);", source)

    def test_reboot_does_not_clear_trial_evidence(self):
        source = SOURCE.read_text()
        reboot = source[source.index("static int power_reboot("):
                        source.index("int h2_jieli_loader_platform_init(")]
        self.assertNotIn("set_trial_attempt(0)", reboot)
        self.assertNotIn("probe_request", reboot)

    def test_warm_app_return_preserves_image_and_installation(self):
        source = (ROOT / "projects/example/targets/h2loader_tar_zlib/display/jieli_ac791n_devkit/src/color_bar_pal.c").read_text()
        self.assertNotIn("flash_update_clr_boot_info", source)
        self.assertNotIn("prepare_destructive_app_return", source)
        reboot = source[source.index("static int app_power_reboot("):
                        source.index("static const h2_pal_power_api_t *app_power_api(")]
        self.assertIn("system_reset();", reboot)
        self.assertNotIn("h2_pal_fs_remove", reboot)
        self.assertIn("h2_jieli_app_loader_prepare_reboot", reboot)

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

    def test_burn_semaphore_outlives_every_wait(self):
        """A burn callback after a timed-out wait must find a live semaphore."""
        source = SOURCE.read_text()
        self.assertNotIn("os_sem_del(&state.update_sem", source)
        self.assertEqual(source.count("os_sem_create(&state.update_sem"), 1)
        init = source[source.index("int h2_jieli_loader_platform_init("):]
        self.assertIn("os_sem_create(&state.update_sem", init)
        callback = source[source.index("static int update_burn_complete("):
                          source.index("static int image_writer_write(")]
        self.assertLess(callback.index("burn_waiting"),
                        callback.index("state.update_result ="))


if __name__ == "__main__":
    unittest.main()
