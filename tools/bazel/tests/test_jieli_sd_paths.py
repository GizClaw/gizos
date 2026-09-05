"""Run the board's native SD path mapping without accessing a card."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class SdPathTest(unittest.TestCase):
    def test_mapping_rejects_traversal(self):
        source = (ROOT / "boards/jieli_ac791n_devkit/ac791n/src/"
                  "h2_jieli_ac791n_devkit_sd_fs.c").read_text()
        begin = source.index("static int translate_path(")
        end = source.index("\n}\n", begin) + 3
        stub = r'''
#include <assert.h>
#include <stddef.h>
#include <string.h>
#define H2_JIELI_SD_ROOT "storage/sd0/C/"
#define H2_JIELI_SD_PATH_MAX 192u
enum { H2_PAL_OK=0, H2_PAL_ERR_INVALID_ARG=-1, H2_PAL_ERR_NO_SPACE=-2 };
'''
        main = r'''
int main(void) {
 char out[H2_JIELI_SD_PATH_MAX];
 const char *bad[]={NULL,"","/","/other","/dl/../data/file",
  "/data/a/../../other","/dl/.","/dl/..","/data//file","/dl/",
  "/data/file/","/data/a/./file","/data/a//file","/dl/..\\other",
  "/dlx/file","dl/file"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
  memset(out,0x5a,sizeof(out));
  assert(translate_path(bad[i],out)==H2_PAL_ERR_INVALID_ARG);
  for(unsigned j=0;j<sizeof(out);++j) assert(out[j]==0x5a);
 }
 const char *paths[]={"/dl","/data","/dl/update.tar.zlib.tmp",
  "/dl/update.tar.zlib.prev","/dl/update.tar.zlib",
  "/data/.h2loader-image.tmp","/data/.h2loader-image-1",
  "/data/.h2loader-image-2","/data/.checksum","/data/nested/video.mp4"};
 const char *mapped[]={"dl","data","dl/H2STAGE.TMP","dl/H2PREV.BIN",
  "dl/H2STAGE.BIN","data/H2IMG.TMP","data/H2IMG1.BIN",
  "data/H2IMG2.BIN","data/H2CHECK.SUM","data/nested/video.mp4"};
 for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);++i) {
  assert(translate_path(paths[i],out)==0);
  assert(strncmp(out,H2_JIELI_SD_ROOT,strlen(H2_JIELI_SD_ROOT))==0);
  assert(strcmp(out+strlen(H2_JIELI_SD_ROOT),mapped[i])==0);
 }
 char long_path[300]; memset(long_path,'a',sizeof(long_path));
 memcpy(long_path,"/data/",6); long_path[299]=0;
 assert(translate_path(long_path,out)==H2_PAL_ERR_NO_SPACE);
 assert(translate_path("/dl",NULL)==H2_PAL_ERR_INVALID_ARG);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-sd-paths-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(stub + source[begin:end] + main)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(test), "-o", str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
