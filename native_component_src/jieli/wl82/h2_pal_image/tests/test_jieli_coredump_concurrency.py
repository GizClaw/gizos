"""Real retained coredump producer, concurrent capture and reset-origin checks."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[5]
class CoredumpTest(unittest.TestCase):
    def test_capture(self):
        source = (ROOT / 'native_component_src/jieli/wl82/h2_pal_image/src/coredump.c').read_text()
        source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
        fixture = (ROOT / 'native_component_src/jieli/wl82/h2_pal_image/tests/fixtures/jieli_coredump_concurrency.c').read_text()
        runner = (ROOT / 'projects/h2loader/targets/h2_loader_tar_zlib/loader/jieli/jieli_ac791n_devkit/src/loader_launcher.c').read_text()
        self.assertIn('h2_jieli_wl82_take_loader_crash_pending() != 0', runner)
        self.assertIn('.force_command_mode = crash_recovery_active', runner)
        self.assertRegex(runner, r'h2_device_loader_run\(&loader,\s*runtime,\s*&service,\s*NULL,\s*!crash_recovery_active\)')
        fixture = fixture.replace('/* RECOVERY_POLICY */', 'static int recovery_policy(uint32_t reason, int pending) { (void)reason; return pending != 0; }')
        source = source.replace('h2_jieli_wl82_boot_geometry()->coredump_flash_address', 'H2_JIELI_COREDUMP_ADDRESS')
        warm = (ROOT / 'native_component_src/jieli/wl82/h2_pal_image/src/jieli_warm_boot.c').read_text()
        warm_types = warm[warm.index('typedef struct warm_snapshot'):warm.index('#define SNAPSHOT (')]
        warm_report = warm[warm.index('void h2_jieli_warm_boot_report'):warm.index('void h2_jieli_warm_boot_snapshot_previous(')]
        warm_constants = '\n'.join(line for line in warm.splitlines() if line.startswith(('#define RAM_MARKER_ADDR ', '#define RETAINED_LOG_ADDR ', '#define RETAINED_LOG_CAPACITY ', '#define SNAPSHOT_MAGIC ')))
        fixture = fixture.replace('/* WARM_REPORT */', warm_constants+'\n'+warm_types+'\nstatic warm_snapshot_t warm_test;\n#define SNAPSHOT (&warm_test)\n'+warm_report)
        modern = 'static volatile uint8_t log_lock;' in source
        for tag, statement in {
            'RESET_READY': 'h2_jieli_atomic_store_u32(&capture_ready,0u);',
            'HOLD_LOG': 'assert(h2_jieli_sdk_try_lock_byte(&log_lock));',
            'RELEASE_LOG': 'h2_jieli_sdk_unlock_byte(&log_lock);',
            'HOLD_CAPTURE': 'assert(h2_jieli_sdk_try_lock_byte(&capture_lock));',
            'RELEASE_CAPTURE': 'h2_jieli_sdk_unlock_byte(&capture_lock);',
            'WAIT_WRITER': 'while (!h2_atomic_load(&writer_entered)) {}',
        }.items():
            fixture = fixture.replace('/* '+tag+' */', statement if modern else ('while (!h2_atomic_load(&writer_finished)) {}' if tag == 'WAIT_WRITER' else ''))
        if not modern:
            fixture = fixture.replace('static uint32_t h2_jieli_atomic_load_u32', 'static __attribute__((unused)) uint32_t h2_jieli_atomic_load_u32').replace('static void h2_jieli_atomic_store_u32', 'static __attribute__((unused)) void h2_jieli_atomic_store_u32')
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory)/'test.c'
            binary = Path(directory)/'test'
            unit.write_text(fixture.replace('/* PROVIDER */',source))
            for role in ['app','loader']:
                subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-pthread',*shlex.split(os.environ.get('JIELI_TEST_CFLAGS','')), *(['-DTEST_LOADER'] if role == 'loader' else []),str(unit),'-I', str(ROOT / 'libs/sync/atomic/include'), str(ROOT / 'libs/sync/atomic/providers/c11/src/h2_atomic_c11.c'), '-o',str(binary)],check=True)
                for case in ['watchdog_loader','watchdog_app','early_role','warm_dirty','warm_layout','invalid_pending','recovery_policy','preboot','flush_capture','counter','dirty','busy_log','busy_capture','threads','torn']:
                    with self.subTest(role=role,case=case):
                        result=subprocess.run([str(binary),case],capture_output=True,text=True,timeout=20)
                        self.assertNotIn('WARNING: ThreadSanitizer',result.stderr)
                        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
if __name__=='__main__':
    unittest.main()
