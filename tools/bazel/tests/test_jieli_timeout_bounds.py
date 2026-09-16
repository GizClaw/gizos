"""Execute the actual SDK/audio timeout conversions at uint32 boundaries."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class TimeoutBoundsTest(unittest.TestCase):
    def test_ceiling_conversion_never_wraps(self):
        audio = (ROOT / "boards/jieli_ac791n_devkit/ac791n/src/"
                 "h2_jieli_ac791n_devkit_audio.c").read_text()
        sdk = (ROOT / "native_component_src/jieli/wl82/h2_pal_core/src/"
               "h2_jieli_wl82_sdk_port.c").read_text()
        source = "#include <stdint.h>\n#include <assert.h>\n"
        source += "#define H2_JIELI_WL82_TICK_MS 10u\n"
        source += function(audio, "static uint32_t timeout_ticks(")
        source += "\n" + function(sdk, "static int ms_to_ticks(")
        source += r'''
int main(void) {
  const uint32_t values[] = {0,1,9,10,11,100,UINT32_MAX-10,
                            UINT32_MAX-9,UINT32_MAX-1,UINT32_MAX};
  for (unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i) {
    uint32_t expected=(uint32_t)(((uint64_t)values[i]+9)/10);
    assert(timeout_ticks(values[i])==expected);
    assert(ms_to_ticks(values[i])==(int)expected);
  }
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.c"
            path.write_text(source)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Werror",
                            str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)
