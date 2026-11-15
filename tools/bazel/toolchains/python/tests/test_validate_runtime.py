from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import unittest

from tools.bazel.toolchains.python import validate_runtime as validator


class RuntimeValidationTest(unittest.TestCase):
    ROOT = Path("/managed/toolchains/python/3.11.18")
    INFO = {
        "version": "3.11.18",
        "implementation": "cpython",
        "abi_flags": "",
        "cache_tag": "cpython-311",
        "soabi": "cpython-311-aarch64-linux-gnu",
        "extension_suffix": ".cpython-311-aarch64-linux-gnu.so",
        "pointer_bits": 64,
        "byteorder": "little",
        "platform": "linux",
        "machine": "aarch64",
        "elf_machine": 183,
        "interpreter": str(ROOT / "python/bin/python3.11"),
        "prefix": str(ROOT / "python"),
    }

    def test_exact_declared_native_runtime_is_accepted(self):
        validator.validate_runtime(dict(self.INFO), "3.11.18", "3.11", self.ROOT)

    def test_mismatched_versions_and_abis_fail_closed(self):
        for name, value in (
            ("version", "3.11.17"), ("implementation", "pypy"), ("abi_flags", "d"),
            ("cache_tag", "cpython-312"), ("soabi", "cpython-311-x86_64-linux-gnu"),
            ("extension_suffix", ".cpython-311-aarch64-linux-musl.so"),
            ("pointer_bits", 32), ("byteorder", "big"), ("platform", "darwin"),
            ("machine", "x86_64"), ("elf_machine", 62),
            ("interpreter", "/outside/python3"), ("prefix", "/outside"),
        ):
            with self.subTest(name=name):
                info = {**self.INFO, name: value}
                with self.assertRaises(validator.RuntimeValidationError):
                    validator.validate_runtime(info, "3.11.18", "3.11", self.ROOT)
        for version, requested in (("3.11.18", "3.12"), ("3.11", "3.11"), ("3.11.18+unknown", "3.11")):
            with self.subTest(version=version, requested=requested):
                with self.assertRaises(validator.RuntimeValidationError):
                    validator.validate_runtime(dict(self.INFO), version, requested, self.ROOT)

    def test_cli_probes_the_real_host_interpreter_and_rejects_a_wrong_pin(self):
        info = validator.runtime_info()
        command = [
            sys.executable, "-I", "-B", validator.__file__,
            "--requested", f"{sys.version_info.major}.{sys.version_info.minor}",
            "--package-root", sys.base_prefix,
            "--host-os", info["platform"], "--host-arch", info["machine"],
        ]
        result = subprocess.run([*command, "--version", info["version"]], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        wrong = f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro + 1}"
        result = subprocess.run([*command, "--version", wrong], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("version mismatch", result.stderr)


if __name__ == "__main__":
    unittest.main()
