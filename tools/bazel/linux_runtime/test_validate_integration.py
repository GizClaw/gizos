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
    def validate_binary(self, relative_path: str):
        runfiles = pathlib.Path(os.environ["TEST_SRCDIR"])
        workspace = os.environ["TEST_WORKSPACE"]
        binary = runfiles / workspace / relative_path
        allowlist = MODULE_PATH.with_name(
            "ubuntu_24_04_x86_64_allowlist.json"
        )
        return validate.validate(binary, allowlist)

    def test_representative_desktop_binary_closure(self):
        sonames = self.validate_binary(
            "projects/showcase/targets/cc_binary/showcase/showcase"
        )
        self.assertIn("libc.so.6", sonames)

    def test_recording_binary_closure(self):
        sonames = self.validate_binary(
            "projects/example/targets/cc_binary/recording/recording-smoke"
        )
        self.assertIn("libavformat.so.62", sonames)


if __name__ == "__main__":
    unittest.main()
