import pathlib
import stat
import tempfile
import unittest

from tools.bazel import firmware_archive_abi


class FirmwareArchiveAbiTest(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tempdir.name)
        self.archive = self.root / "libsample.a"
        self.archive.write_bytes(b"archive")

    def tearDown(self):
        self.tempdir.cleanup()

    def fake_nm(self, output: str, exit_code: int = 0) -> str:
        path = self.root / "fake_nm"
        path.write_text(
            "#!/bin/sh\n"
            f"printf '%b' {output!r}\n"
            f"exit {exit_code}\n",
            encoding="utf-8",
        )
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return str(path)

    def test_accepts_archive_without_forbidden_symbols(self):
        nm = self.fake_nm("         U malloc\nmember.o: U fprintf\n")
        output = self.root / "validated" / "libsample.a"
        firmware_archive_abi.validate_and_copy(
            nm, "//sample", self.archive, output
        )
        self.assertEqual(output.read_bytes(), self.archive.read_bytes())

    def test_rejects_impure_ptr(self):
        nm = self.fake_nm("member.o: U _impure_ptr\n")
        output = self.root / "validated.a"
        with self.assertRaisesRegex(RuntimeError, "_impure_ptr"):
            firmware_archive_abi.validate_and_copy(
                nm, "//sample", self.archive, output
            )
        self.assertFalse(output.exists())

    def test_rejects_getreent(self):
        nm = self.fake_nm("member.o: U __getreent\n")
        with self.assertRaisesRegex(RuntimeError, "__getreent"):
            firmware_archive_abi.validate_archive(nm, "//sample", self.archive)

    def test_matches_exact_symbol_only(self):
        nm = self.fake_nm("member.o: U prefixed__getreent\n")
        firmware_archive_abi.validate_archive(nm, "//sample", self.archive)

    def test_reports_nm_failure(self):
        nm = self.fake_nm("", exit_code=2)
        with self.assertRaisesRegex(RuntimeError, "nm failed"):
            firmware_archive_abi.validate_archive(nm, "//sample", self.archive)


if __name__ == "__main__":
    unittest.main()
