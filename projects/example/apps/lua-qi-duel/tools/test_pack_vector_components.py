import os
from pathlib import Path
import re
import struct
import unittest
import zlib

from pack_vector_components import (pack_components, pack_frame,
                                    selected_frame_indices, unpack_frame)


class VectorPackingTest(unittest.TestCase):
    def test_invalid_source_slices_and_compression(self):
        directory = '["example"]={w=8,h=8,sheet=false,frames={{0,%d}}},'
        for source in [struct.pack("<I", 524289) + b"x",
                       struct.pack("<I", 13) + zlib.compress(b"x" * 14),
                       struct.pack("<I", 13) + zlib.compress(b"x" * 13) + b"tail"]:
            with self.assertRaises(ValueError):
                pack_components(source, directory % len(source), "all")
        with self.assertRaises(ValueError):
            pack_components(b"", directory % 10, "all")

    def test_signed_coordinates_and_quarter_pixels(self):
        for coordinates in [(-32768, 32767, 32767, -32768, 1, -1),
                            (0, 0, 4, 0, 4, 4)]:
            raw = b"H2VG" + struct.pack("<HHHHBBH", 8, 8, 0, 1, 4, 13, 3)
            raw += struct.pack("<6h", *coordinates) + b"\0"
            packed = pack_frame(raw)
            self.assertEqual(unpack_frame(packed), raw)
            for length in range(len(packed)):
                with self.assertRaises((ValueError, IndexError, struct.error)):
                    unpack_frame(packed[:length])

    def test_all_approved_commands_survive_screen_filtering(self):
        root = Path(os.environ.get("TEST_SRCDIR", ".")) / os.environ.get("TEST_WORKSPACE", "")
        app = root / "projects/example/apps/lua-qi-duel"
        bank = (app / "assets/vector/components/components.h2vp").read_bytes()
        directory = (app / "data/skills/qi-duel/scripts/component_paths.lua").read_text()

        def frames(blob, text):
            result = {}
            for line in text.splitlines():
                name = re.match(r'\["([^"]+)"\]', line)
                if name:
                    result[name[1]] = []
                    for offset, size in re.findall(r"\{(\d+),(\d+)\}", line):
                        chunk = blob[int(offset):int(offset) + int(size)]
                        raw = zlib.decompress(chunk[4:])
                        self.assertEqual(len(raw), struct.unpack_from("<I", chunk)[0])
                        result[name[1]].append(raw)
            return result

        original = frames(bank, directory)
        for screen in ("all", "amoled", "h106"):
            packed, metadata = pack_components(bank, directory, screen)
            selected = frames(packed, metadata)
            expected = {
                k: [v[index] for index in selected_frame_indices(k, len(v))]
                for k, v in original.items()
                if ((screen == "all" or
                     not k.endswith("-" + ("h106" if screen == "amoled" else "amoled"))) and
                    selected_frame_indices(k, len(v)))
            }
            self.assertEqual(selected.keys(), expected.keys())
            for name, values in selected.items():
                self.assertEqual([unpack_frame(v) for v in values], expected[name])
            self.assertLess(len(packed), len(bank) // 2)

    def test_runtime_keyframe_contract(self):
        self.assertEqual(selected_frame_indices("action-hands", 16),
                         (1, 3, 5, 7, 9, 11, 13, 15))
        self.assertEqual(selected_frame_indices("beam-clash-amoled", 12),
                         tuple(range(12)))
        self.assertEqual(selected_frame_indices("charge-cells", 70),
                         (0, 10, 20, 30, 40, 50, 60))
        self.assertEqual(selected_frame_indices("beam-clash-fade-amoled", 4), ())


if __name__ == "__main__":
    unittest.main()
