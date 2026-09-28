"""Independently enumerate FS callbacks and both Preferences dispatch shapes."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path("projects/e2e/apps/pal-storage/app")

class InventoryTest(unittest.TestCase):
    def test_every_operation_has_an_executed_owner(self):
        manifest = json.loads((ROOT / "api_coverage.json").read_text())
        fs = Path("libs/pal/include/h2/pal/os/h2_pal_fs.h").read_text()
        pref = Path("libs/pal/include/h2/pal/os/h2_pal_pref.h").read_text()
        actual = {"fs." + name for name in re.findall(r'\(\*(\w+)\)', fs.split("typedef struct h2_pal_fs_vtable {")[1].split("}")[0])}
        actual |= {"pref." + name for name in re.findall(r'h2_pal_pref_api_\w+_fn\s+(\w+)\s*;', pref.split("typedef struct h2_pal_pref_vtable {")[1].split("}")[0])}
        actual |= {"namespace." + name for name in re.findall(r'h2_pal_pref_namespace_\w+_fn\s+(\w+)\s*;', pref.split("struct h2_pal_pref_namespace {")[1].split("}")[0])}
        operations = manifest["operations"]
        self.assertEqual(len(operations), len(actual))
        self.assertEqual({op["operation"] for op in operations}, actual)
        self.assertEqual(len(actual), manifest["operation_count"])
        registry = re.findall(r'H2_PAL_STORAGE_CASE\("([^"]+)", ([12])\)', (ROOT / "include/h2_pal_storage_cases.inc").read_text())
        ids = {name for name, phase in registry}
        self.assertEqual(len(ids), manifest["case_count"])
        self.assertEqual(len(ids), len(registry))
        source = (ROOT / "src/h2_pal_storage_e2e.c").read_text()
        self.assertIn('#include "h2_pal_storage_cases.inc"', source)
        self.assertEqual({int(v) for v in re.findall(r'case (\d+):', source)}, set(range(len(ids))))
        for operation in operations:
            self.assertIn(operation["owner_case"], ids)
            for case in operation["additional_cases"]:
                self.assertIn(case, ids)
            capability, method = operation["operation"].split(".")
            pattern = (r'->' + method + r'\s*\(' if capability == "namespace" else
                       r'h2_pal_' + capability + '_' + method + r'\s*\(')
            self.assertRegex(source, pattern, operation["operation"])

if __name__ == "__main__":
    unittest.main()
