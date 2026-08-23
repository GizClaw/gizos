from __future__ import annotations

import importlib.util
import os
import pathlib
import sys
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("validate.py")
SPEC = importlib.util.spec_from_file_location("linux_runtime_validate", MODULE_PATH)
assert SPEC and SPEC.loader
validate = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = validate
SPEC.loader.exec_module(validate)


class ValidateIntegrationTest(unittest.TestCase):
    def test_representative_desktop_binary_closure(self):
        runfiles = pathlib.Path(os.environ["TEST_SRCDIR"])
        workspace = os.environ["TEST_WORKSPACE"]
        binary = runfiles / workspace / "projects/showcase/targets/cc_binary/showcase/showcase"
        allowlist = MODULE_PATH.with_name(
            "ubuntu_24_04_x86_64_allowlist.json"
        )
        sonames = validate.validate(binary, allowlist)
        self.assertIn("libc.so.6", sonames)


if __name__ == "__main__":
    unittest.main()
