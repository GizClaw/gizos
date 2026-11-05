"""Actual producer policies feed the current host oracle; no hardware qualification."""
from pathlib import Path
import re
import subprocess
import sys
import unittest

from verify_device import boot_ledger

probe=Path(sys.argv.pop(1)).resolve()
registry=Path(sys.argv.pop(1))


class ProducerAdmission(unittest.TestCase):
    def test_current_bk_and_devkit_producers_deliver_required_confirmation(self):
        ids=re.findall(r'H2_PAL_MQTT_CASE\(\w+, "([^"]+)"\)',registry.read_text())
        for board in ['bk7258','devkit']:
            with self.subTest(board=board):
                result=subprocess.run([str(probe),'--emit-ledger',board],capture_output=True,check=True)
                text=result.stdout.decode()
                admitted=boot_ledger(text,ids,'test-version')
                self.assertEqual(admitted['ready']['board'],board)
                self.assertEqual(admitted['ready']['confirm'],'pending')
                self.assertEqual(admitted['confirmation'],{'board':board,'rc':'0'})
                incomplete=text.split('H2_PAL_MQTT_CONFIRMED ',1)[0]
                with self.assertRaisesRegex(AssertionError,'missing post-READY'):
                    boot_ledger(incomplete,ids,'test-version')
                with self.assertRaisesRegex(AssertionError,'current admission'):
                    boot_ledger(incomplete.replace('confirm=pending','confirm=0'),ids,'test-version')


if __name__=='__main__':unittest.main()
