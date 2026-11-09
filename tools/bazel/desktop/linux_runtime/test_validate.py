from __future__ import annotations

import importlib.util
import pathlib
import sys
import tempfile
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("validate.py")
SPEC = importlib.util.spec_from_file_location("linux_runtime_validate", MODULE_PATH)
assert SPEC and SPEC.loader
validate = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = validate
SPEC.loader.exec_module(validate)


class ValidateTest(unittest.TestCase):
    def test_parses_mapped_and_direct_entries(self):
        output = """
            linux-vdso.so.1 (0x00007fff)
            libSDL3.so.0 => /tmp/libSDL3.so.0 (0x00007fff)
            /lib64/ld-linux-x86-64.so.2 (0x00007fff)
        """
        self.assertEqual(
            validate.parse_ldd(output),
            {"linux-vdso.so.1", "libSDL3.so.0", "ld-linux-x86-64.so.2"},
        )

    def test_rejects_unresolved_library(self):
        with self.assertRaisesRegex(validate.ValidationError, "unresolved"):
            validate.parse_ldd("libmissing.so => not found\n")

    def test_rejects_unknown_and_forbidden_libraries(self):
        contract = {
            "allowed_sonames": ["libc.so.6", "libcurl.so.4"],
            "forbidden_substrings": ["curl"],
        }
        with self.assertRaisesRegex(validate.ValidationError, "forbidden"):
            validate.validate_sonames({"libcurl.so.4"}, contract)
        with self.assertRaisesRegex(validate.ValidationError, "allowlist"):
            validate.validate_sonames({"libsurprise.so.1"}, contract)

if __name__ == "__main__":
    unittest.main()
