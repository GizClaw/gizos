"""Board watchdog feeding and SDK boot-mode regression checks."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[3]
class WatchdogTest(unittest.TestCase):
    def test_progress(self):
        source = (ROOT/'boards/jieli_ac791n_devkit/ac791n/layouts/h2loader/src/watchdog.c').read_text()
        source = '\n'.join(x for x in source.splitlines() if not x.startswith('#include'))
        patch = (ROOT/'boards/jieli_ac791n_devkit/ac791n/layouts/h2loader/sdk_patches/early_app_boot.patch').read_text()
        part = patch[patch.index(' void setup_arch()'):]
        part = part[:part.index('@@', 1)]
        boot = '\n'.join(x[1:] for x in part.splitlines() if x.startswith((' ', '+')) and not x.startswith('+++'))
        boot = boot[:boot.index('    clk_early_init')]+ '}\n'
        fixture = (ROOT/'tools/bazel/tests/fixtures/jieli_watchdog.c').read_text()
        with tempfile.TemporaryDirectory() as directory:
            unit=Path(directory)/'test.c'
            binary=Path(directory)/'test'
            unit.write_text(fixture.replace('/* PROVIDER */',source).replace('/* BOOT */',boot))
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-pthread',*shlex.split(os.environ.get('JIELI_TEST_CFLAGS','')),'-I',str(ROOT/'native_component_src/jieli/wl82/h2_pal_core/include'),str(unit),'-I', str(ROOT / 'libs/atomic/include'), str(ROOT / 'libs/atomic/providers/c11/src/h2_atomic_c11.c'), '-o',str(binary)],check=True)
            for case in ['stopped_0','stopped_1','healthy','boot','threads']:
                with self.subTest(case=case):
                    result=subprocess.run([str(binary),case],capture_output=True,text=True,timeout=10)
                    self.assertNotIn('WARNING: ThreadSanitizer',result.stderr)
                    self.assertEqual(result.returncode,0,result.stderr)
if __name__=='__main__':
    unittest.main()
