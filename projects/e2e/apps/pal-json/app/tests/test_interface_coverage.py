from pathlib import Path
import re
import unittest

ROOT = Path("projects/e2e/apps/pal-json/app")
HEADER = Path("libs/pal/include/h2/pal/os/h2_pal_json.h")


class JsonInventory(unittest.TestCase):
    def test_public_vtable_and_mandatory_registry(self):
        header = HEADER.read_text()
        vtable = header.split("typedef struct h2_pal_json_vtable {")[1].split("} h2_pal_json_vtable_t;")[0]
        operations = re.findall(r"h2_pal_result_t\s*\(\*(\w+)\)\s*\(", vtable)
        self.assertEqual(len(operations), 24)
        self.assertEqual(len(set(operations)), len(operations))
        source = (ROOT / "src/h2_pal_json_e2e.c").read_text()
        for name in operations:
            self.assertIn("api->vtable->" + name, source)
            self.assertRegex(source, r"h2_pal_json_" + name + r"\s*\(")
        cases = re.findall(r'H2_PAL_JSON_CASE\("([^"]+)",\s*(\w+)\)',
                           (ROOT / "include/h2_pal_json_cases.inc").read_text())
        self.assertEqual(len(cases), 15)
        self.assertEqual(len({name for name, _ in cases}), len(cases))
        for _, function in cases:
            self.assertRegex(source, r"static h2_pal_result_t " + function + r"\(")
        self.assertIn('#include "h2_pal_json_cases.inc"', source)
        self.assertNotIn("assert(", source)


if __name__ == "__main__":
    unittest.main()
