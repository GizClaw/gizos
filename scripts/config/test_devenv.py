import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("setup_devenv", ROOT / "scripts/config/setup-devenv.py")
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


class DevenvTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.gizos = self.root / "gizos"
        self.provider = self.root / "installed provider"
        self.upstream = self.root / "upstream"
        self.upstream.mkdir()
        self.git(self.upstream, "init", "--quiet")
        (self.upstream / "export.sh").write_text(
            'export TEST_PROVIDER_LOADED="$GIZOS_ROOT"\n'
        )
        (self.upstream / "Makefile").write_text(
            'environment-test:\n\t@test -d "$(GIZOS_ROOT)/tools/bazel/native_versions"\n'
        )
        self.git(self.upstream, "add", ".")
        self.git(self.upstream, "commit", "--quiet", "-m", "fixture")
        self.commit = self.git(self.upstream, "rev-parse", "HEAD")
        self.manifest = self.gizos / "tools/bazel/native_versions/gizos_devenv_commit.txt"
        self.manifest.parent.mkdir(parents=True)
        self.manifest.write_text(self.commit + "\n")
        scripts = self.gizos / "scripts/config"
        scripts.mkdir(parents=True)
        for name in ("gizos-devenv-env.sh", "h2loader-operation-env.sh"):
            shutil.copy2(ROOT / "scripts/config" / name, scripts / name)
        (scripts / "h2loader-operation-env.sh").chmod(0o755)

    def git(self, root, *args):
        return subprocess.check_output(
            ["git", "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
             "-c", "core.hooksPath=/dev/null", "-C", str(root), *args],
            text=True, stderr=subprocess.DEVNULL,
        ).strip()

    def install(self):
        with mock.patch.object(setup, "ROOT", self.gizos), \
                mock.patch.object(setup, "URL", str(self.upstream)), \
                mock.patch.dict(os.environ, {"GIZOS_DEVENV_ROOT": str(self.provider)}), \
                mock.patch.object(sys, "argv", ["setup-devenv.py", "environment-test"]):
            return setup.main()

    def operation(self, command, provider=True):
        environment = dict(os.environ)
        environment.pop("GIZOS_DEVENV_ROOT", None)
        environment.pop("TEST_PROVIDER_LOADED", None)
        environment["HOME"] = str(self.root / "empty-home")
        if provider:
            environment["GIZOS_DEVENV_ROOT"] = str(self.provider)
        return subprocess.run(
            [str(self.gizos / "scripts/config/h2loader-operation-env.sh"), "/bin/sh", "-eu", "-c", command],
            env=environment, cwd="/", text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )

    def test_installs_exact_pin_in_custom_location_and_loads_it(self):
        self.assertEqual(self.install(), 0)
        self.assertEqual(self.git(self.provider, "rev-parse", "HEAD"), self.commit)
        result = self.operation('test "$TEST_PROVIDER_LOADED" = "$GIZOS_ROOT"')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.upstream.rename(self.root / "offline")
        self.assertEqual(self.install(), 0)

    def test_preserves_dirty_provider_and_rejects_activation(self):
        self.install()
        (self.provider / "export.sh").write_text("operator changes\n")
        with self.assertRaisesRegex(ValueError, "tracked changes"):
            self.install()
        self.assertEqual((self.provider / "export.sh").read_text(), "operator changes\n")
        result = self.operation("exit 0")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("env invalid", result.stdout)

    def test_wrong_revision_is_not_changed(self):
        self.install()
        self.manifest.write_text("a" * 40 + "\n")
        with self.assertRaisesRegex(ValueError, "existing checkout was preserved"):
            self.install()
        self.assertEqual(self.git(self.provider, "rev-parse", "HEAD"), self.commit)
        self.assertNotEqual(self.operation("exit 0").returncode, 0)

    def test_host_only_operation_needs_no_provider(self):
        result = self.operation('test -z "${TEST_PROVIDER_LOADED-}"', provider=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_explicit_missing_provider_is_an_error(self):
        self.assertNotEqual(self.operation("exit 0").returncode, 0)

    def test_legacy_adapter_is_only_a_fallback(self):
        legacy = self.gizos / ".env/devenv"
        legacy.parent.mkdir()
        legacy.write_text("export LEGACY_LOADED=yes\n")
        result = self.operation('test "$LEGACY_LOADED" = yes', provider=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.install()
        result = self.operation('test -z "${LEGACY_LOADED-}"; test -n "$TEST_PROVIDER_LOADED"')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
