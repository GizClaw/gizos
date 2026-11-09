"""Package identity wins; factory fallback hashes opaque native facts in old order."""
from pathlib import Path
import hashlib
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]
SOURCE = ROOT / "libs/h2loader/device/src/h2_device_boot_policy.c"


class IdentityTest(unittest.TestCase):
    def test_preserves_verified_package_identity(self):
        text = SOURCE.read_text()
        begin = text.index("int h2_device_boot_policy_current_identity(")
        end = text.index("{", begin) + 1
        depth = 1
        while depth:
            depth += (text[end] == "{") - (text[end] == "}")
            end += 1
        function = text[text.index("static void identity_from_metadata("):text.index("static int padded_identity(")] + text[begin:end]
        program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "h2_device_boot_policy.h"
static h2_loader_status_t stored;
static unsigned bootstrap_calls;
static int status_error, known=1;
static int read_status(h2_device_boot_policy_t* self,h2_loader_status_t* out) {
  (void)self;*out=stored;return status_error;
}
h2_pal_result_t h2_loader_pal_current_image_identity(h2_loader_pal_t* p,h2_loader_image_role_t role,
 const char* board,const char* target,const char* version,h2_loader_image_identity_t* out) {
  (void)p;(void)role;(void)board;(void)target;(void)version;(void)out;return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t info(void* user,uint32_t id,h2_pal_image_view_t view,h2_pal_image_info_t* out) {
  (void)user;assert((id==1 || id==2) && view==H2_PAL_IMAGE_VIEW_NATIVE);
  *out=(h2_pal_image_info_t){.capacity=0x37d000,.image_size=123,.flags=known?H2_PAL_IMAGE_FLAG_SIZE_KNOWN:0};return 0;
}
static h2_pal_result_t observe(void* user,const h2_pal_power_boot_request_t* request,h2_pal_power_native_evidence_t* out) {
  (void)user;assert(request->policy==H2_PAL_POWER_BOOT_POLICY_EXISTING);++bootstrap_calls;
  out->format=2;out->size=40;for(unsigned i=0;i<40;++i)out->bytes[i]=(uint8_t)i;return 0;
}
h2_pal_result_t h2_loader_pal_init(h2_loader_pal_t *out,const h2_loader_pal_config_t *config) {
 memset(out,0,sizeof(*out));out->config=*config;return 0;
}
int h2_loader_metadata_validate(h2_loader_metadata_slot_t slot,const h2_loader_metadata_t *metadata) {
 (void)slot;return metadata->valid?0:H2_PAL_ERR_INVALID_STATE;
}
static int padded_identity(h2_device_boot_policy_t *self,h2_loader_image_role_t role,const char *board,
 const char *target,const char *version,h2_loader_image_identity_t *out) {
 (void)self;(void)role;(void)board;(void)target;(void)version;(void)out;assert(0);return H2_PAL_ERR_UNSUPPORTED;
}
/* FUNCTION */
int main(void) {
  static const h2_pal_image_vtable_t iv={.get_info=info};
  static const h2_pal_power_vtable_t pv={.prepare_next_boot_partition=observe};
  const h2_pal_image_api_t image={.vtable=&iv};const h2_pal_power_api_t power={.vtable=&pv};
  h2_device_boot_policy_t service={.config={.loader_partition_id=1,.app_partition_id=2,.image=&image,.power=&power,.flags=H2_DEVICE_BOOT_FACTORY_IDENTITY,.loader_identity=H2_DEVICE_IDENTITY_COMMITTED}};
  const char *board="jieli_ac791n_devkit", *target="wl82";
  h2_loader_image_identity_t out;
  h2_loader_metadata_t m={.valid=1,.role=H2_LOADER_IMAGE_ROLE_H2LOADER,.image_size=915037};
  strcpy(m.board,board);strcpy(m.target,target);strcpy(m.version,"package-release");memset(m.image_checksum,'a',64);
  stored.partition_1=stored.partition_2=m;
  for(unsigned id=1;id<=2;++id) {
    service.running_partition_id=id;
    assert(h2_device_boot_policy_current_identity(&service,H2_LOADER_IMAGE_ROLE_H2LOADER,board,target,"build",&out)==0);
    assert(bootstrap_calls==0 && out.image_size==m.image_size);
    assert(strcmp(out.image_sha256,m.image_checksum)==0 && strcmp(out.version,m.version)==0);
  }
  stored.partition_2.valid=0;
  assert(h2_device_boot_policy_current_identity(&service,H2_LOADER_IMAGE_ROLE_H2LOADER,board,target,"build",&out)==0);
  assert(bootstrap_calls==1 && out.image_size==123 && strcmp(out.image_sha256,"EXPECTED")==0);
  stored.partition_2=m;strcpy(stored.partition_2.board,"another-board");
  assert(h2_device_boot_policy_current_identity(&service,H2_LOADER_IMAGE_ROLE_H2LOADER,board,target,"build",&out)==0 && bootstrap_calls==2);
  known=0;
  assert(h2_device_boot_policy_current_identity(&service,H2_LOADER_IMAGE_ROLE_H2LOADER,board,target,"build",&out)==H2_PAL_ERR_UNSUPPORTED && bootstrap_calls==2);
  status_error=H2_PAL_ERR_IO;
  assert(h2_device_boot_policy_current_identity(&service,H2_LOADER_IMAGE_ROLE_H2LOADER,board,target,"build",&out)==H2_PAL_ERR_IO && bootstrap_calls==2);
  return 0;
}
'''
        expected = hashlib.sha256(bytes(range(40)) + b"buildjieli_ac791n_devkitwl82").hexdigest()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            source.write_text(program.replace("/* FUNCTION */", function).replace("EXPECTED", expected))
            includes = [
                ROOT / "pal/include",
                ROOT / "libs/sync/atomic/include",
                ROOT / "libs/archive/bundle/include",
                ROOT / "libs/h2loader/device/include",
                ROOT / "libs/crypto/sha256/include",
            ]
            command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror"]
            for path in includes:
                command += ["-I", str(path)]
            command += [str(source), str(ROOT / "libs/h2loader/device/src/h2_loader_sha256.c"),
                        str(ROOT / "libs/crypto/sha256/src/h2_sha256.c"), "-o", str(binary)]
            subprocess.run(command, check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
