"""Exercise the compile-only layout with host GNU Make, without an SDK."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest


PROJECT = Path(__file__).absolute().parents[3] / "boards/ac791n_chip/ac791n/layouts/compile_only/project.mk"
VARIABLES = ("DEFINES", "INCLUDES", "c_SRC_FILES", "LFLAGS", "H2_JIELI_LAYOUT_ROOT")


class CompileOnlyProjectTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        rules = self.root / "empty.mk"
        rules.touch()
        (self.root / "Makefile").write_text(
            f"H2_JIELI_PROJECT_RULES := {rules}\ninclude {PROJECT}\n"
            ".PHONY: print\nprint:\n"
            + "".join(f"\t@printf '%s\\n' '{name}=$({name})'\n" for name in VARIABLES)
        )

    def make(self, *arguments):
        result = subprocess.run(["make", "--no-print-directory", *arguments],
                                cwd=self.root, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def values(self, *arguments):
        return dict(line.split("=", 1) for line in self.make("print", *arguments).splitlines())

    def test_sdram_defaults_and_switch(self):
        default = self.values()
        self.assertIn("-DCONFIG_NO_SDRAM_ENABLE", default["DEFINES"].split())
        self.assertNotIn("-DH2_JIELI_SDRAM_ENABLE=1", default["DEFINES"].split())
        self.assertEqual(default["H2_JIELI_LAYOUT_ROOT"], str(PROJECT.parent))
        enabled = self.values("H2_JIELI_SDRAM_ENABLE=1")["DEFINES"].split()
        self.assertNotIn("-DCONFIG_NO_SDRAM_ENABLE", enabled)
        self.assertIn("-DH2_JIELI_SDRAM_ENABLE=1", enabled)

    def test_caller_hooks_and_library_group(self):
        # Assign in the wrapper, rather than on the command line: ?= must keep it.
        wrapper = self.root / "Makefile"
        wrapper.write_text("H2_JIELI_LAYOUT_ROOT := /caller/layout\n" + wrapper.read_text())
        values = self.values("H2_JIELI_BOARD_DEFINES=-DFIXTURE=1",
                             "H2_JIELI_BOARD_INCLUDES=-Ifixture/include",
                             "H2_JIELI_BOARD_C_SRC_FILES=fixture.c",
                             "H2_JIELI_BOARD_LIBS=fixture.a second.a")
        for variable, token in (("DEFINES", "-DFIXTURE=1"),
                                ("INCLUDES", "-Ifixture/include"),
                                ("c_SRC_FILES", "fixture.c")):
            self.assertIn(token, values[variable].split())
        flags = values["LFLAGS"].split()
        for library in ("fixture.a", "second.a"):
            self.assertLess(flags.index("--start-group"), flags.index(library))
            self.assertLess(flags.index(library), flags.index("--end-group"))
        self.assertEqual(values["H2_JIELI_LAYOUT_ROOT"], "/caller/layout")

    def test_reserved_expand_recipe_and_insertion(self):
        self.assertNotIn("awk '", self.make("-n", "pre_build"))
        dry_run = self.make("-n", "pre_build", "H2_JIELI_RESERVED_EXPAND_CONFIG_FILE=reserved.ini")
        program = re.search(r"awk '([^']+)' reserved.ini", dry_run)
        self.assertIsNotNone(program, dry_run)
        reserved = self.root / "reserved.ini"
        reserved.write_text("REGION_ADR=0x1000;\nREGION_LEN=0x2000;\n")
        config = self.root / "isd_config.ini"
        config.write_text("[RESERVED_CONFIG]\nEXISTING=YES;\n[BURNER_CONFIG]\nSIZE=8;\n")
        result = subprocess.run(["awk", program.group(1), str(reserved), str(config)],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "[RESERVED_CONFIG]\nEXISTING=YES;\n"
                         + reserved.read_text() + "[BURNER_CONFIG]\nSIZE=8;\n")


if __name__ == "__main__":
    unittest.main()
