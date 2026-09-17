"""Run the board's native SD path mapping without accessing a card."""
from pathlib import Path
import os
import shlex
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
 char out[H2_JIELI_SD_PATH_MAX + 1u]; /* One extra canary at the capacity boundary. */
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
  "/dl/.h2loader-image.tmp","/dl/.h2loader-image-1",
  "/dl/.h2loader-image-2","/data/.checksum","/data/nested/video.mp4","/dl/x","/data/a/b"};
 const char *mapped[]={"dl","data","dl/H2STAGE.TMP","dl/H2PREV.BIN",
  "dl/H2STAGE.BIN","dl/H2IMG.TMP","dl/H2IMG1.BIN",
  "dl/H2IMG2.BIN","data/H2CHECK.SUM","data/nested/video.mp4","dl/x","data/a/b"};
 for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);++i) {
  memset(out,0x5a,sizeof(out));
  assert(translate_path(paths[i],out)==0);
  size_t length = strlen(H2_JIELI_SD_ROOT) + strlen(mapped[i]);
  assert(strlen(out)==length);
  assert(out[length + 1u]==0x5a);
  assert(strncmp(out,H2_JIELI_SD_ROOT,strlen(H2_JIELI_SD_ROOT))==0);
  assert(strcmp(out+strlen(H2_JIELI_SD_ROOT),mapped[i])==0);
 }

 /* Count UTF-16 units, not UTF-8 bytes, before entering the SDK. */
 char component_path[512];
 strcpy(component_path, "/data/h2-probe/");
 size_t prefix = strlen(component_path);
 memset(component_path + prefix, 'c', 130u);
 component_path[prefix + 130u] = 0;
 assert(translate_path(component_path, out) == H2_PAL_OK);
 component_path[prefix + 130u] = 'c';
 component_path[prefix + 131u] = 0;
 memset(out, 0x5a, sizeof(out));
 assert(translate_path(component_path, out) == H2_PAL_ERR_NO_SPACE);
 for (size_t i = 0; i < sizeof(out); ++i) assert(out[i] == 0x5a);
 for (size_t i = 0; i < 44u; ++i)
   memcpy(component_path + prefix + i * 3u, "\xe6\x97\xa5", 3u);
 component_path[prefix + 132u] = 0;
 assert(translate_path(component_path, out) == H2_PAL_OK);
 for (size_t i = 0; i < 131u; ++i)
   memcpy(component_path + prefix + i * 3u, "\xe6\x97\xa5", 3u);
 component_path[prefix + 393u] = 0;
 assert(translate_path(component_path, out) == H2_PAL_ERR_NO_SPACE);
 /* Mixed ASCII/emoji fits the full path, isolating the surrogate-pair count. */
 memset(component_path + prefix, 'e', 128u);
 memcpy(component_path + prefix + 128u, "\xf0\x9f\x98\x80", 5u);
 assert(translate_path(component_path, out) == H2_PAL_OK);
 component_path[prefix + 128u] = 'e';
 memcpy(component_path + prefix + 129u, "\xf0\x9f\x98\x80", 5u);
 assert(translate_path(component_path, out) == H2_PAL_ERR_NO_SPACE);
 /* The maximum translated length is capacity minus its terminator. */
 size_t suffix_length = H2_JIELI_SD_PATH_MAX - strlen(H2_JIELI_SD_ROOT);
 char boundary[H2_JIELI_SD_PATH_MAX + 1u];
 memset(boundary,'a',sizeof(boundary));
 memcpy(boundary,"/data/",6u);
 boundary[86] = '/'; /* Each component stays below 130 units. */
 boundary[suffix_length]='\0';
 memset(out,0x5a,sizeof(out));
 assert(translate_path(boundary,out)==H2_PAL_OK);
 assert(strlen(out)==H2_JIELI_SD_PATH_MAX - 1u);
 assert(out[H2_JIELI_SD_PATH_MAX]==0x5a);
 boundary[suffix_length]='a';
 boundary[suffix_length + 1u]='\0';
 memset(out,0x5a,sizeof(out));
 assert(translate_path(boundary,out)==H2_PAL_ERR_NO_SPACE);
 for(size_t i=0;i<sizeof(out);++i) assert(out[i]==0x5a);
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
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            *shlex.split(os.environ.get("JIELI_TEST_CFLAGS", "")),
                            "-fsanitize=address", "-fno-omit-frame-pointer",
                            str(test), "-o", str(binary)], check=True, timeout=60)
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
