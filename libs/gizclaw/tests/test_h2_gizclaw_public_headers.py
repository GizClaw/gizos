"""Check the whole installed header surface, including conditional declarations."""

import collections
import os
from pathlib import Path
import re
import unittest


def without_comments(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


class PublicHeadersTest(unittest.TestCase):
    def test_removed_client_compatibility_does_not_return_privately(self):
        if "TEST_SRCDIR" in os.environ:
            root = (Path(os.environ["TEST_SRCDIR"]) /
                    os.environ["TEST_WORKSPACE"] / "libs/gizclaw")
        else:
            root = Path(__file__).absolute().parents[1]
        forbidden = (
            r"\bh2_gizclaw_client_(?:ping(?:_measure)?|"
            r"speedtest(?:_measure|_download)?|delete_peer|rpc_call(?:_stream)?|"
            r"set_rpc_interceptor_internal)\b|"
            r"\bh2_gizclaw_(?:encode_pb_message|decode_pb_message|"
            r"test_set_rpc_call(?:_stream)?|test_set_speed_test|rpc_response_deinit)\b")
        sources = sorted((root / "src").glob("*.[ch]"))
        self.assertGreater(len(sources), 0)
        for source in sources:
            self.assertNotRegex(without_comments(source.read_text()), forbidden,
                                str(source))

    def test_exact_application_surface(self):
        if "TEST_SRCDIR" in os.environ:
            root = (Path(os.environ["TEST_SRCDIR"]) /
                    os.environ["TEST_WORKSPACE"] / "libs/gizclaw")
        else:
            root = Path(__file__).absolute().parents[1]
        catalog = without_comments((root / "tests/public_api.inc").read_text())
        expected = re.findall(r"H2_GIZCLAW_API\((h2_gizclaw_\w+)\)", catalog)
        self.assertEqual(len(expected), 181)
        self.assertEqual(len(set(expected)), 181)
        actual = []
        headers = sorted((root / "include").glob("*.h"))
        self.assertGreater(len(headers), 0)
        for header in headers:
            text = without_comments(header.read_text())
            self.assertNotIn("H2_GIZCLAW_TESTING", text, str(header))
            self.assertNotRegex(text, r'#\s*include\s*["<][^">]*_internal\.h')
            self.assertNotRegex(text, r"#\s*define\s+h2_gizclaw_\w+")
            self.assertNotRegex(text, r"\bh2_gizclaw_track_(?:vtable|read_fn|write_fn)\b")
            self.assertNotRegex(text, r"\bstruct\s+h2_gizclaw_track\s*\{")
            actual.extend(re.findall(r"\b(h2_gizclaw_\w+)\s*\(", text))
        self.assertEqual(collections.Counter(actual), collections.Counter(expected))


if __name__ == "__main__":
    unittest.main()
