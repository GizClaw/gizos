"""Exercise the production identity selector with a distinct package version."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit/src/loader_launcher.c"

STUB = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define H2_PAL_OK 0
#define H2_PAL_ERR_INVALID_ARG -1
#define H2_PAL_ERR_INVALID_STATE -2
#define H2_JIELI_PARTITION_LOADER 1
#define H2_LOADER_IMAGE_ROLE_H2LOADER 1
typedef struct { int valid, role; uint64_t image_size; char board[64], target[64], version[64], image_checksum[65]; } h2_loader_metadata_t;
typedef struct { h2_loader_metadata_t partition_1, partition_2; } h2_loader_status_t;
typedef struct { int format, role; uint64_t image_size; char board[64], target[64], version[64], image_sha256[65]; } h2_loader_image_identity_t;
typedef struct { unsigned id; } h2_pal_power_boot_partition_t;
typedef struct { void *power, *pref, *mem; const char *board, *target; } h2_runtime_t;
struct BootInfo { unsigned codeLength; };
typedef int h2_loader_sha256_t;
static h2_loader_status_t stored;
static unsigned running_id = 2, bootstrap_calls;
static int h2_pal_power_get_running_boot_partition(void *p, h2_pal_power_boot_partition_t *v) { v->id=running_id; return 0; }
static int h2_loader_read_pref_status(void *p, void *m, h2_loader_status_t *v) { *v=stored; return 0; }
static int get_current_boot_info(struct BootInfo *v) { ++bootstrap_calls; v->codeLength=123; return 0; }
static void h2_loader_sha256_init(h2_loader_sha256_t *s) {}
static void h2_loader_sha256_update(h2_loader_sha256_t *s, const void *p, size_t n) {}
static void h2_loader_sha256_finish(h2_loader_sha256_t *s, uint8_t *p) { memset(p,0,32); }
static void h2_loader_sha256_hex(const uint8_t *p, char *s) { memset(s,'0',64); s[64]=0; }
static void usb_diag_write(const char *s) {}
'''
MAIN = r'''
int main(void) {
 h2_runtime_t r={.board="jieli_ac791n_devkit",.target="wl82"};
 h2_loader_image_identity_t out;
 h2_loader_metadata_t m={.valid=1,.role=1,.image_size=915037};
 strcpy(m.board,r.board); strcpy(m.target,r.target);
 strcpy(m.version,"package-release"); memset(m.image_checksum,'a',64);
 stored.partition_1=stored.partition_2=m;
 for(running_id=1;running_id<=2;running_id++) {
  assert(current_loader_identity(&r,"bazel-native-artifacts",&out)==0);
  assert(bootstrap_calls==0);
  assert(out.image_size==m.image_size);
  assert(strcmp(out.image_sha256,m.image_checksum)==0);
  assert(strcmp(out.version,m.version)==0);
 }
 running_id=2; stored.partition_2.valid=0;
 assert(current_loader_identity(&r,"build",&out)==0);
 assert(bootstrap_calls==1 && out.image_size==123);
 stored.partition_2=m; strcpy(stored.partition_2.board,"another-board");
 assert(current_loader_identity(&r,"build",&out)==0);
 assert(bootstrap_calls==2);
 return 0;
}
'''

class IdentityTest(unittest.TestCase):
    def test_preserves_verified_package_identity(self):
        text = SOURCE.read_text()
        function = text[text.index("static int current_loader_identity("):text.index("static void loader_startup_event(")]
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            source.write_text(STUB + function + MAIN)
            subprocess.run(["cc", "-std=c11", str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=5)

if __name__ == "__main__":
    unittest.main()
