from pathlib import Path
import re
import unittest

ROOT = Path("projects/e2e/apps/pal-audio-decoder/app")


class DecoderInventory(unittest.TestCase):
    def test_public_operations_have_real_calls(self):
        header = Path("libs/pal/include/h2/pal/hal/h2_pal_audio_decoder.h").read_text()
        vtable = header.split("typedef struct h2_pal_audio_decoder_vtable {")[1].split("} h2_pal_audio_decoder_vtable_t;")[0]
        operations = re.findall(r"h2_pal_result_t\s*\(\*(\w+)\)\s*\(", vtable)
        self.assertEqual(len(operations), 8)
        source = (ROOT / "src/h2_pal_audio_decoder_e2e.c").read_text()
        for operation in operations:
            self.assertIn("api->vtable->" + operation, source)
            self.assertRegex(source, r"h2_pal_audio_decoder_" + operation + r"\s*\(")
        cases = re.findall(r'H2_PAL_ADEC_CASE\("([^"]+)",\s*(\w+)\)',
                           (ROOT / "include/h2_pal_audio_decoder_cases.inc").read_text())
        self.assertEqual(len(cases), 29)
        self.assertEqual(len({name for name, _ in cases}), len(cases))
        for _, function in cases:
            self.assertIn("static h2_pal_result_t " + function + "(", source)
        self.assertNotIn("assert(", source)


if __name__ == "__main__":
    unittest.main()
