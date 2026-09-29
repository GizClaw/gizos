from pathlib import Path
import re
import unittest
class Inventory(unittest.TestCase):
    def test_operations_and_registry(self):
        header = Path('libs/pal/include/h2/pal/hal/h2_pal_display.h').read_text()
        body = header.split('typedef struct h2_pal_display_vtable {')[1].split('}')[0]
        operations = re.findall(r'\(\*(\w+)\)', body)
        self.assertEqual(len(operations), 6)
        source = Path('projects/e2e/apps/pal-display/app/src/h2_pal_display_e2e.c').read_text()
        for op in operations:
            self.assertIn('h2_pal_display_'+op+'(', source)
        cases = Path('projects/e2e/apps/pal-display/app/include/h2_pal_display_cases.inc').read_text()
        ids = re.findall(r'H2_PAL_DISPLAY_CASE\("([^"]+)"', cases)
        self.assertEqual(len(ids),24)
        self.assertEqual(len(set(ids)),len(ids))
        for fn in re.findall(r', (\w+)\)',cases):
            self.assertRegex(source,rf'static int {fn}\(')
if __name__ == '__main__': unittest.main()
