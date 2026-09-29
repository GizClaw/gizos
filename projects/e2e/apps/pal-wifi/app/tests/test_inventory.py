import json
from pathlib import Path
import re
import unittest
BASE = Path("projects/e2e/apps/pal-wifi/app")
class Inventory(unittest.TestCase):
    def test_all_slots_and_cases(self):
        m=json.loads((BASE/"api_coverage.json").read_text())
        actual=set()
        for domain in {x["domain"] for x in m["operations"]}:
            op=next(x for x in m["operations"] if x["domain"]==domain)
            text=(Path("libs/pal/include/h2/pal")/op["header"]).read_text()
            body=text.split("typedef struct "+op["vtable"]+" {")[1].split("} "+op["vtable"]+"_t;")[0]
            fields=re.findall(r"\(\*(\w+)\)",body) if domain=="netif" else re.findall(r"\w+_fn\s+(\w+)\s*;",body)
            actual.update((domain,f) for f in fields)
        self.assertEqual(actual,{(x["domain"],x["name"]) for x in m["operations"]})
        self.assertEqual(len(actual),m["operation_count"])
        source=(BASE/"src/h2_pal_wifi_e2e.c").read_text()
        for op in m["operations"]:self.assertRegex(source,re.escape(op["wrapper"])+r"\s*\(")
        cases=re.findall(r'H2_WIFI_CASE\("([^\"]+)",\s*(\w+)\)',(BASE/"include/h2_pal_wifi_cases.inc").read_text())
        self.assertEqual(len(cases),m["case_count"])
        self.assertEqual(len({c[0] for c in cases}),len(cases))
        for _,fn in cases:self.assertRegex(source,r"static int "+fn+r"\(")
        self.assertNotIn("assert(",source)
        self.assertIn('cases[i].run != restore',source)
if __name__=="__main__":unittest.main()
