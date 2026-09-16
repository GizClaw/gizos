"""Require every board PAL vtable to initialize every function-pointer member."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]
BOARD = ROOT / "boards/jieli_ac791n_devkit/ac791n/src"


def without_comments(source):
    return re.sub(r"/\*.*?\*/|//[^\n]*", " ", source, flags=re.S)


def vtable_members(source):
    tables = {}
    for match in re.finditer(
            r"typedef\s+struct(?:\s+\w+)?\s*\{([^{}]*)\}\s*(\w+_vtable_t)\s*;",
            without_comments(source), re.S):
        members = set()
        for declaration in match[1].split(";"):
            if re.search(r"\btypedef\b", declaration):
                continue
            member = re.search(r"\b\w[\w\s*]*\(\s*\*\s*(\w+)\s*\)\s*\(",
                               declaration)
            if member:
                members.add(member[1])
        tables[match[2]] = members
    return tables


class VtableCompletenessTest(unittest.TestCase):
    def test_all_board_vtables_are_complete(self):
        tables = {}
        for header in sorted((ROOT / "libs/pal/include").rglob("*.h")):
            tables.update(vtable_members(header.read_text()))
        inspected = 0
        entries = 0
        for path in sorted(BOARD.glob("*.c")):
            for match in re.finditer(
                    r"\b(\w+_vtable_t)\s+(\w+)\s*=\s*\{([^{}]*)\}\s*;",
                    without_comments(path.read_text()), re.S):
                inspected += 1
                with self.subTest(file=path.name, type=match[1], name=match[2]):
                    self.assertIn(match[1], tables, "PAL vtable struct was not parsed")
                    members = tables[match[1]]
                    initializers = dict(re.findall(
                        r"\.(\w+)\s*=\s*([^,]+)", match[3]))
                    self.assertFalse(members - initializers.keys(),
                                     f"Missing entries: {sorted(members - initializers.keys())}")
                    for member in sorted(members):
                        # Board entries name functions directly, optionally with &.
                        # Requiring that form also rejects NULL, zero and casts of zero.
                        value = initializers[member].strip()
                        self.assertRegex(value, r"^&?\s*[A-Za-z_]\w*$", member)
                        self.assertNotEqual(value.lstrip("& "), "NULL", member)
                        entries += 1
        self.assertGreater(entries, 0, "No function-pointer members parsed")
        self.assertGreaterEqual(inspected, 13, "Vtable parser inspected too few literals")
        print(f"Inspected {inspected} vtable literals and {entries} function-pointer entries")


if __name__ == "__main__":
    unittest.main()
