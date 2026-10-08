import struct
import unittest

from tools.esp_sr_model.pack import pack


class PackTest(unittest.TestCase):
    def test_offsets_order_alignment_and_opaque_bytes(self):
        files = {"wn9_index": b"index", "wn9_data": b"\x00\xff\x17", "_MODEL_INFO_": b"model-info"}
        blob = pack("wn9_fixture", files)
        self.assertEqual(blob, pack("wn9_fixture", dict(reversed(list(files.items())))))
        self.assertEqual(struct.unpack_from("<I", blob)[0], 1)
        self.assertEqual(blob[4:36].rstrip(b"\0"), b"wn9_fixture")
        self.assertEqual(struct.unpack_from("<I", blob, 36)[0], 3)
        for index, name in enumerate(sorted(files)):
            entry = 40 + index * 40
            self.assertEqual(blob[entry:entry + 32].rstrip(b"\0").decode(), name)
            offset, size = struct.unpack_from("<II", blob, entry + 32)
            self.assertEqual(offset % 16, 0)
            self.assertGreaterEqual(offset, 160)
            self.assertEqual(blob[offset:offset + size], files[name])

    def test_rejects_bad_inputs_before_publishing(self):
        for model, files in [
            ("../model", {"_MODEL_INFO_": b"x"}),
            ("x" * 32, {"_MODEL_INFO_": b"x"}),
            ("fixture", {}),
            ("fixture", {"_MODEL_INFO_": b""}),
            ("fixture", {"_MODEL_INFO_": b"x", "wn9_data": b"version https://git-lfs.github.com/spec/v1\n"}),
            ("fixture", {"_MODEL_INFO_": b"x", "wn9_data": b"x" * (1024 * 1024)}),
        ]:
            with self.subTest(model=model, files=list(files)):
                with self.assertRaises(ValueError):
                    pack(model, files)


if __name__ == "__main__":
    unittest.main()
