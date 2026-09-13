"""Exercise track capacity preflight before the real provider allocates a slot."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class AudioCapacityTest(unittest.TestCase):
    def test_capacity_fits_native_pcm_buffer(self):
        source = (ROOT / 'boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_audio.c').read_text()
        begin = source.index('static int audio_create_track(')
        end = source.index('  jieli_audio_track_t *track = NULL;', begin)
        stub = r'''
#include <assert.h>
#include <stdint.h>
#include "h2/pal/hal/h2_pal_audio.h"
enum { H2_AUDIO_SAMPLE_RATE=16000, H2_AUDIO_FRAME_BYTES=640, H2_AUDIO_TRACK_QUEUE_FRAMES=4 };
static struct { int speaker_started; } audio_state={1};
'''
        main = r'''
int main(void) {
 h2_audio_track_config_t config={.format={.sample_rate_hz=16000,
  .channels=1,.sample_format=H2_AUDIO_SAMPLE_S16LE},.volume_factor_milli=1000};
 h2_pal_audio_track_t *track=NULL;
 const size_t invalid[]={SIZE_MAX, SIZE_MAX/640u+1u, UINT32_MAX/640u+1u};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
  config.buffer_frames=invalid[i];
  assert(audio_create_track(NULL,&config,&track)==H2_AUDIO_ERR_INVALID_ARG);
 }
 const size_t valid[]={0,1,4,UINT32_MAX/640u};
 for(unsigned i=0;i<sizeof(valid)/sizeof(valid[0]);++i) {
  config.buffer_frames=valid[i];
  assert(audio_create_track(NULL,&config,&track)==H2_AUDIO_OK);
 }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'test.c'
            test.write_text(stub + source[begin:end] + '(void)out_track; return H2_AUDIO_OK;\n}\n' + main)
            binary = Path(directory) / 'test'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'libs/pal/include'), str(test), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
