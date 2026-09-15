"""Run the shared launcher audio probe with a failing information provider."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class AudioProbeTest(unittest.TestCase):
    def test_info_failure_skips_devices(self):
        source = (ROOT / 'projects/example/native_component_src/jieli/wl82/devkit_app/src/color_bar_pal.c').read_text()
        probe = source[source.index('static void probe_audio(void)'):source.index('static void probe_pref(void)')]
        fixture = r'''
#include "h2/pal/hal/h2_pal_audio.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define H2_AUDIO_PROBE_SAMPLES 32
static int16_t audio_probe_pcm[H2_AUDIO_PROBE_SAMPLES];
static int audio_info_result, info_result, starts, logs;
static char log_text[256];
static int get_info(void *user, h2_audio_info_t *info) {
    (void)user;
    if (info_result != H2_AUDIO_OK) return info_result; /* no output written */
    *info = (h2_audio_info_t){0};
    return H2_AUDIO_OK;
}
static int start(void *user) {
    (void)user;
    ++starts;
    return H2_AUDIO_ERR_UNAVAILABLE;
}
static const h2_pal_audio_vtable_t vtable = {
    .get_info = get_info, .start_mic = start, .start_speaker = start,
};
static const h2_pal_audio_api_t api = {.vtable = &vtable};
static const h2_pal_audio_api_t *h2_jieli_ac791n_devkit_audio_api(void) { return &api; }
static void usb_write_status(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(log_text, sizeof(log_text), format, args);
    va_end(args);
    ++logs;
}
/* REAL_PROBE */
int main(void) {
    const int failures[] = {H2_AUDIO_ERR_UNAVAILABLE, H2_AUDIO_ERR_INVALID_ARG};
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        info_result = failures[i]; starts = logs = 0;
        probe_audio();
        assert(audio_info_result == info_result);
        assert(starts == 0);
        assert(logs == 1 && strstr(log_text, "info=") && strstr(log_text, "skip"));
    }
    info_result = H2_AUDIO_OK; starts = logs = 0;
    probe_audio();
    assert(starts == 2 && logs == 1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / 'probe.c'
            binary = Path(directory) / 'probe'
            unit.write_text(fixture.replace('/* REAL_PROBE */', probe))
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'libs/pal/include'), str(unit), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
