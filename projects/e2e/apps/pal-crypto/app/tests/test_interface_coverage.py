import json
from pathlib import Path
import re
import unittest

ROOT=Path("projects/e2e/apps/pal-crypto/app")

class CryptoInventory(unittest.TestCase):
    def test_contract_operations_and_cases(self):
        manifest=json.loads((ROOT/"api_coverage.json").read_text())
        header=Path("libs/pal/include/h2/pal/os/h2_pal_crypto.h").read_text()
        vtable=header.split("typedef struct h2_pal_crypto_vtable {")[1].split("} h2_pal_crypto_vtable_t;")[0]
        actual={name:re.sub(r"\s+"," ",signature).strip() for name,signature in re.findall(r'h2_pal_result_t\s*\(\*(\w+)\)\s*\((.*?)\);',vtable,re.S)}
        operations=manifest["operations"]
        self.assertEqual(len(operations),manifest["operation_count"])
        self.assertEqual(len(actual),len(operations))
        self.assertEqual({x["name"]:x["signature"] for x in operations},actual)
        source=(ROOT/"src/h2_pal_crypto_e2e.c").read_text()
        for operation in operations:
            self.assertEqual(operation["wrapper"],"h2_pal_crypto_"+operation["name"])
            self.assertRegex(source,operation["wrapper"]+r"\s*\(")
            self.assertRegex(source,r"api->vtable->"+operation["name"]+r"\s*==\s*NULL")
        cases=re.findall(r'H2_PAL_CRYPTO_CASE\("([^"]+)",\s*(\w+)\)',(ROOT/"include/h2_pal_crypto_cases.inc").read_text())
        self.assertEqual(len(cases),manifest["case_count"])
        self.assertEqual(len({x[0] for x in cases}),len(cases))
        self.assertIn('#include "h2_pal_crypto_cases.inc"',source)
        for _,function in cases:
            self.assertRegex(source,r"static int "+function+r"\(")
        self.assertNotIn('assert(',source)

if __name__=="__main__":unittest.main()
