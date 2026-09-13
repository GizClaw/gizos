"""The shared reboot transaction must be able to read the App's next bank."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
TARGET = ROOT / "projects/example/targets/h2loader_tar_zlib/display/jieli_ac791n_devkit/src"
APP_SUPPORT = ROOT / "projects/h2loader/native_component_src/jieli/wl82/h2loader_app/src"
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
    def test_shared_reboot_commits_attempt_before_request(self):
        source = (APP_SUPPORT / "jieli_h2loader_app_support.c").read_text()
        begin = source.index("int h2_jieli_app_loader_prepare_reboot(")
        end = source.index("static int digest_start(", begin)
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define H2_PAL_OK 0
#define H2_PAL_ERR_INVALID_ARG -1
#define H2_JIELI_PARTITION_LOADER 1u
#define H2_JIELI_PARTITION_APP 2u
#define H2_JIELI_BANK_2_SFC_BASE 0x37c020u
#define H2_LOADER_PREF_NAMESPACE "h2loader"
#define H2_JIELI_TRIAL_ATTEMPT_KEY "jieli_trial_attempt"
#define H2_PAL_PREF_OPEN_READ_WRITE 1
typedef struct ns ns_t;
struct ns {
 int (*set_string)(ns_t *,const char *,const char *);
 int (*commit)(ns_t *);
 int (*close)(ns_t *);
};
typedef ns_t h2_pal_pref_namespace_t;
typedef struct {
 const void *pref;
 struct { char image_sha256[65]; } active_identity;
} h2_loader_app_client_config_t;
static int open_rc,set_rc,commit_rc,close_rc,opened,closed,committed,requested;
static int set(ns_t *n,const char *k,const char *v) {
 assert(strcmp(k,H2_JIELI_TRIAL_ATTEMPT_KEY)==0);
 assert(strcmp(v,"current-image")==0); return set_rc;
}
static int commit(ns_t *n) { ++committed; return commit_rc; }
static int close(ns_t *n) { ++closed; return close_rc; }
static ns_t ns={set,commit,close};
static int h2_pal_pref_open(const void *p,const char *n,int flags,ns_t **out) {
 ++opened; *out=&ns; return open_rc;
}
static void h2_jieli_warm_boot_request(uint32_t base) {
 assert(base==H2_JIELI_BANK_2_SFC_BASE);
 assert(committed && closed); ++requested;
}
static void reset(void) {
 open_rc=set_rc=commit_rc=close_rc=opened=closed=committed=requested=0;
}
'''
        main = r'''
int main(void) {
 h2_loader_app_client_config_t c={.pref=&ns};
 strcpy(c.active_identity.image_sha256,"current-image");
 reset(); assert(h2_jieli_app_loader_prepare_reboot(NULL,1)==0);
 assert(!opened && !requested);
 assert(h2_jieli_app_loader_prepare_reboot(NULL,2)==-1);
 assert(h2_jieli_app_loader_prepare_reboot(&c,3)==-1);
 reset(); assert(h2_jieli_app_loader_prepare_reboot(&c,2)==0);
 assert(requested==1 && committed==1 && closed==1);
 reset(); open_rc=-4; assert(h2_jieli_app_loader_prepare_reboot(&c,2)==-4);
 assert(!requested && !closed);
 reset(); set_rc=-5; assert(h2_jieli_app_loader_prepare_reboot(&c,2)==-5);
 assert(!requested && !committed && closed==1);
 reset(); commit_rc=-6; assert(h2_jieli_app_loader_prepare_reboot(&c,2)==-6);
 assert(!requested && closed==1);
 reset(); close_rc=-7; assert(h2_jieli_app_loader_prepare_reboot(&c,2)==-7);
 assert(!requested && closed==1);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-app-reboot-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(fixture + source[begin:end] + main)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", str(test), "-o", str(binary)],
                           check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)
        for name in ("color_bar_pal.c", "mp4_player_small_pal.c"):
            app = (TARGET / name).read_text()
            self.assertIn("h2_jieli_app_loader_prepare_reboot", app)
            self.assertNotIn("flash_update_clr_boot_info", app)

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
