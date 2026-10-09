"""Optional diagnostics must not relabel old physical Audio observations."""

import hashlib
import json
from pathlib import Path
import unittest
from unittest.mock import patch

import aec_compatibility as compatibility


class Compatibility(unittest.TestCase):
    def test_exact_additions_restore_every_historical_source(self):
        audit = json.loads(compatibility.RECORD.read_bytes())
        for name, record in audit["files"].items():
            source = Path(name).read_bytes()
            restored = compatibility.source_content(Path(name), source)
            self.assertEqual(hashlib.sha256(restored).hexdigest(), record["previous_sha256"])
            self.assertEqual(compatibility.source_content(Path(name), restored), restored)

    def test_unrelated_provider_change_cannot_inherit_qualification(self):
        for name in compatibility.PATHS:
            source = Path(name).read_bytes() + b"\nunknown ordinary Audio change\n"
            with self.assertRaises(AssertionError):
                compatibility.source_content(Path(name), source)

    def test_unsupported_stub_cannot_be_changed_to_success(self):
        name = "libs/pal/providers/ios/pal_core/src/h2_ios_audio.m"
        source = Path(name).read_bytes().replace(
            b"    return H2_AUDIO_ERR_UNSUPPORTED;", b"    return H2_AUDIO_OK;", 1)
        with self.assertRaises(AssertionError):
            compatibility.source_content(Path(name), source)

    def test_fixture_edit_cannot_rebind_provider_bytes(self):
        original = Path.read_bytes
        def read(path):
            content = original(path)
            return content + b" " if path == compatibility.RECORD else content
        with patch.object(Path, "read_bytes", new=read), self.assertRaises(AssertionError):
            name = next(iter(compatibility.PATHS))
            compatibility.source_content(name, original(Path(name)))

    def test_scope_cannot_exempt_another_capability(self):
        content = b"unrelated Display provider bytes"
        self.assertEqual(compatibility.source_content("display.c", content), content)


if __name__ == "__main__":
    unittest.main()
