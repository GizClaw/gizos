from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).parents[1] / "jieli_runner.py"
SPEC = importlib.util.spec_from_file_location("jieli_runner", MODULE_PATH)
assert SPEC and SPEC.loader
runner = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)

LOCATOR_SCHEMA = "h2.native-locator.v1"
ORIGINAL_REQUIRE_SUPPORTED_HOST = runner.require_supported_host


def write_executable(path: Path, body: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("#!/bin/sh\n" + body, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return path


def write_locator(path: Path, kind: str, paths: dict[str, str], enabled: bool = True) -> Path:
    path.write_text(
        json.dumps(
            {
                "schema": LOCATOR_SCHEMA,
                "kind": kind,
                "enabled": enabled,
                "paths": paths,
                "metadata": {"archive": f"{kind}.tar.xz", "tree_sha256": "0" * 64},
            }
        ),
        encoding="utf-8",
    )
    return path


def git(checkout: Path, *arguments: str) -> str:
    return subprocess.run(
        ["git", "-C", str(checkout), *arguments],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.com",
             "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.com"},
    ).stdout.strip()


class JieliRunnerFixture:
    """A fake SDK checkout, toolchain tree and post-build script."""

    def __init__(self, root: Path, target: str = "br23") -> None:
        self.root = root
        self.target = target
        self.family = runner.FAMILIES[target]
        self.subdirectory = "SDK" if target == "br23" else "."
        self.checkout = root / "sdk-checkout"
        sdk_root = self.checkout / self.subdirectory
        (sdk_root / "cpu" / target / "tools").mkdir(parents=True)
        (sdk_root / "sdk-source.c").write_text("int sdk_source;\n", encoding="utf-8")
        git(self.checkout, "init", "-q")
        git(self.checkout, "add", "-A")
        git(self.checkout, "commit", "-q", "-m", "sdk")
        self.commit = git(self.checkout, "rev-parse", "HEAD")

        self.toolchain = root / "toolchain"
        self.toolchain_bin = self.toolchain / "pi32v2" / "bin"
        for name in ("clang", "lto-wrapper", "objcopy", "objdump", "objsizedump"):
            write_executable(self.toolchain_bin / name, "exit 0\n")
        # The fake `make` lives in the toolchain bin so the fixed PATH finds it.
        self.make_log = root / "make.log"
        write_executable(
            self.toolchain_bin / "make",
            f'printf \'%s\\n\' "$*" >> "{self.make_log}"\n'
            'while [ $# -gt 0 ]; do\n'
            '  case "$1" in -C) shift; cd "$1" ;; esac\n'
            '  shift\n'
            'done\n'
            'exit 0\n',
        )
        self.postbuild = root / "postbuild"
        for name in ("isd_download", "fw_add", "ufw_maker", "remove_tailing_zeros"):
            write_executable(self.postbuild / name, "exit 0\n")

        self.post_script = write_executable(
            root / "post.sh",
            'sdk=$1; out=$4\n'
            'test -d "$sdk" || exit 3\n'
            f'test "$QT_QPA_PLATFORM" = offscreen || exit 4\n'
            'mkdir -p "$out"\n'
            'for name in firmware.elf symbols.txt jl_isd.bin jl_isd.fw update.ufw; do\n'
            '  printf "%s" "$name" > "$out/$name"\n'
            'done\n',
        )
        self.project_makefile = root / "project.mk"
        self.project_makefile.write_text("h2_link:\n", encoding="utf-8")
        self.project_rules = root / "h2_project_rules.mk"
        self.project_rules.write_text("# fixture rules\n", encoding="utf-8")
        self.version_file = root / "sdk-commit.txt"
        self.version_file.write_text(self.commit + "\n", encoding="utf-8")
        self.archives_file = root / "archives.txt"
        self.archives_file.write_text("fixture identity\n", encoding="utf-8")
        self.sdk_locator = write_locator(
            root / "sdk-locator.json",
            f"jieli-{self.family}-sdk",
            {"root": str(sdk_root), "checkout": str(self.checkout)},
        )
        self.toolchain_locator = write_locator(
            root / "toolchain-locator.json",
            "jieli-toolchain",
            {"root": str(self.toolchain), "pi32v2_bin": str(self.toolchain_bin)},
        )
        self.postbuild_locator = write_locator(
            root / "postbuild-locator.json", "jieli-postbuild", {"root": str(self.postbuild)}
        )
        self.outputs = root / "outputs"

    def arguments(self, **overrides: object) -> mock.Mock:
        values = dict(
            source_root=str(self.root),
            target=self.target,
            entry="projects/e2e/targets/jieli_firmware/pal/jieli_dev_board",
            board="fixture_board",
            image="demo",
            project_makefile=str(self.project_makefile.relative_to(self.root)),
            project_rules=str(self.project_rules.relative_to(self.root)),
            version="test-version",
            sdk_version_file=str(self.version_file),
            toolchain_archives_file=str(self.archives_file),
            sdk_locator=str(self.sdk_locator),
            toolchain_locator=str(self.toolchain_locator),
            postbuild_locator=str(self.postbuild_locator),
            post_script=str(self.post_script),
            elf_output=str(self.outputs / "firmware.elf"),
            symbols_output=str(self.outputs / "symbols.txt"),
            flash_image_output=str(self.outputs / "jl_isd.bin"),
            fw_output=str(self.outputs / "jl_isd.fw"),
            update_output=str(self.outputs / "update.ufw"),
            manifest_output=str(self.outputs / "manifest.json"),
            native_component=["launcher"],
            native_component_source=[],
            native_include_root=[],
            prebuilt_component=[],
        )
        values.update(overrides)
        return mock.Mock(**values)


class JieliRunnerTest(unittest.TestCase):
    def setUp(self) -> None:
        # The build flow is exercised on every CI host; the real host gate is
        # covered by test_unsupported_hosts_fail_closed.
        patcher = mock.patch.object(runner, "require_supported_host", lambda: None)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_unsupported_hosts_fail_closed(self):
        with mock.patch.object(runner.platform, "system", return_value="Darwin"), \
                mock.patch.object(runner.platform, "machine", return_value="arm64"):
            with self.assertRaisesRegex(runner.RunnerError, "UNSUPPORTED"):
                ORIGINAL_REQUIRE_SUPPORTED_HOST()
        with mock.patch.object(runner.platform, "system", return_value="Linux"), \
                mock.patch.object(runner.platform, "machine", return_value="aarch64"):
            with self.assertRaisesRegex(runner.RunnerError, "UNSUPPORTED"):
                ORIGINAL_REQUIRE_SUPPORTED_HOST()
        with mock.patch.object(runner.platform, "system", return_value="Linux"), \
                mock.patch.object(runner.platform, "machine", return_value="x86_64"):
            ORIGINAL_REQUIRE_SUPPORTED_HOST()

    def test_required_environment_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = JieliRunnerFixture(Path(temporary).resolve())
            write_locator(fixture.sdk_locator, "jieli-ac695n-sdk", {}, enabled=False)
            with self.assertRaisesRegex(
                runner.NativeRuntimeError, "locator is not configured"
            ):
                runner.required_environment(fixture.arguments())

    def test_required_environment_rejects_wrong_family(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = JieliRunnerFixture(Path(temporary).resolve())
            with self.assertRaisesRegex(runner.NativeRuntimeError, "kind mismatch"):
                runner.required_environment(fixture.arguments(target="wl82"))

    def test_required_environment_is_fixed_and_headless(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = JieliRunnerFixture(Path(temporary).resolve())
            environment = runner.required_environment(fixture.arguments())
            self.assertEqual(environment["QT_QPA_PLATFORM"], "offscreen")
            self.assertEqual(environment["H2_NATIVE_BUILD_JOBS"], "4")
            self.assertTrue(environment["PATH"].startswith(str(fixture.toolchain_bin)))
            self.assertNotIn("JIELI_AC695N_SDK_PATH", environment)

    def test_copy_sdk_skips_only_top_level_vcs_and_doc_directories(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source = root / "sdk"
            for relative in (".git/HEAD", "doc/guide.md", "ui_project/x", "Makefile",
                             "apps/demo/doc/readme.md", "apps/demo/main.c"):
                path = source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(relative, encoding="utf-8")
            runner.copy_sdk(source, root / "copy")
            self.assertFalse((root / "copy/.git").exists())
            self.assertFalse((root / "copy/doc").exists())
            self.assertFalse((root / "copy/ui_project").exists())
            self.assertTrue((root / "copy/Makefile").is_file())
            self.assertTrue((root / "copy/apps/demo/doc/readme.md").is_file())
            self.assertTrue((root / "copy/apps/demo/main.c").is_file())

    def test_prebuilt_components_reject_duplicates(self):
        self.assertEqual(
            runner.parse_prebuilt_components(["h2_firmware_lib=lib.a"]),
            {"h2_firmware_lib": "lib.a"},
        )
        with self.assertRaisesRegex(runner.RunnerError, "duplicate"):
            runner.parse_prebuilt_components(["a=x.a", "a=y.a"])
        with self.assertRaisesRegex(runner.RunnerError, "invalid"):
            runner.parse_prebuilt_components(["bad definition"])

    def test_release_file_rejects_empty_and_symlink(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "empty.bin").touch()
            with self.assertRaisesRegex(runner.RunnerError, "non-empty"):
                runner.release_file(root, "empty.bin", "artifact")
            target = root / "target.bin"
            target.write_bytes(b"data")
            (root / "link.bin").symlink_to(target)
            with self.assertRaisesRegex(runner.RunnerError, "symlink"):
                runner.release_file(root, "link.bin", "artifact")

    def test_build_runs_make_and_post_script_in_an_sdk_copy(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = JieliRunnerFixture(Path(temporary).resolve())
            runner.build(fixture.arguments())
            for name in ("firmware.elf", "symbols.txt", "jl_isd.bin", "jl_isd.fw", "update.ufw"):
                self.assertEqual((fixture.outputs / name).read_text(encoding="utf-8"), name)
            manifest = json.loads((fixture.outputs / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["target"], "br23")
            self.assertEqual(manifest["family"], "ac695n")
            self.assertEqual(manifest["sdk_commit"], fixture.commit)
            self.assertEqual(manifest["native_components"], ["launcher"])
            self.assertEqual(manifest["outputs"]["flash_image"], "jl_isd.bin")
            invocations = fixture.make_log.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(invocations), 1)
            self.assertIn("h2_link", invocations[0])
            self.assertIn(f"-f {fixture.project_makefile}", invocations[0])
            self.assertIn("H2_BAZEL_COMPONENT_MANIFEST=", invocations[0])
            self.assertIn(f"H2_JIELI_PROJECT_RULES={fixture.project_rules}", invocations[0])
            self.assertIn(f"TOOL_DIR={fixture.toolchain_bin}", invocations[0])
            self.assertNotIn(str(fixture.checkout), invocations[0])
            self.assertEqual(manifest["project_makefile"], "project.mk")
            self.assertEqual(manifest["native_sources"], [])
            # The pinned checkout is never written to.
            self.assertEqual(git(fixture.checkout, "status", "--porcelain"), "")

    def test_build_does_not_select_an_sdk_application_project(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = JieliRunnerFixture(Path(temporary).resolve(), target="wl82")
            sdk_application = fixture.checkout / "apps/demo/board"
            sdk_application.mkdir(parents=True)
            (sdk_application / "Makefile").write_text("$(error must not load)\n", encoding="utf-8")
            git(fixture.checkout, "add", "-A")
            git(fixture.checkout, "commit", "-q", "-m", "sdk application")
            fixture.commit = git(fixture.checkout, "rev-parse", "HEAD")
            fixture.version_file.write_text(fixture.commit + "\n", encoding="utf-8")
            runner.build(fixture.arguments())
            invocations = fixture.make_log.read_text(encoding="utf-8").splitlines()
            self.assertNotIn("apps/demo/board", invocations[0])
            self.assertIn(str(fixture.project_makefile), invocations[0])
            self.assertIn("h2_link", invocations[0])

    def test_build_renders_component_manifest_with_sources_and_archives(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = JieliRunnerFixture(Path(temporary).resolve())
            (fixture.root / "launcher").mkdir()
            (fixture.root / "launcher/main.c").write_text("int x;\n", encoding="utf-8")
            (fixture.root / "launcher/include").mkdir()
            (fixture.root / "libh2_firmware_lib.a").write_bytes(b"!<arch>\n")
            captured: dict[str, str] = {}
            original_make = runner.run_native_make

            def capture(make, project, environment, variables, targets, stage):
                for variable in variables:
                    if variable.startswith("H2_BAZEL_COMPONENT_MANIFEST="):
                        captured["manifest"] = Path(variable.split("=", 1)[1]).read_text(encoding="utf-8")
                return original_make(make, project, environment, variables, targets, stage)

            with mock.patch.object(runner, "run_native_make", capture):
                runner.build(
                    fixture.arguments(
                        native_component_source=["launcher=launcher/main.c"],
                        native_include_root=["launcher/include"],
                        prebuilt_component=["h2_firmware_lib=libh2_firmware_lib.a"],
                    )
                )
            manifest = captured["manifest"]
            self.assertIn(f"H2_BAZEL_NATIVE_SRCS := {fixture.root / 'launcher/main.c'}", manifest)
            self.assertIn(f"H2_BAZEL_NATIVE_INCLUDES := -I{fixture.root / 'launcher/include'}", manifest)
            self.assertIn(f"H2_BAZEL_ARCHIVES := {fixture.root / 'libh2_firmware_lib.a'}", manifest)
            self.assertIn('H2_BAZEL_DEFINES := -DH2_JIELI_FIRMWARE_VERSION=\\"test-version\\"', manifest)
            self.assertIn(f"H2_BAZEL_PREBUILT_H2_FIRMWARE_LIB := {fixture.root / 'libh2_firmware_lib.a'}", manifest)
            output = json.loads((fixture.outputs / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(output["native_sources"], ["launcher/main.c"])
            self.assertEqual(output["prebuilt_components"], ["h2_firmware_lib"])

    def test_build_rejects_missing_project_and_escaping_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = JieliRunnerFixture(Path(temporary).resolve())
            with self.assertRaisesRegex(runner.RunnerError, "project makefile is missing"):
                runner.build(fixture.arguments(project_makefile="missing.mk"))
            with self.assertRaisesRegex(runner.RunnerError, "escapes the source root"):
                runner.build(fixture.arguments(native_include_root=["../outside"]))
            with self.assertRaisesRegex(runner.RunnerError, "source is missing"):
                runner.build(fixture.arguments(native_component_source=["launcher=launcher/none.c"]))

    def test_build_rejects_commit_mismatch_and_dirty_checkout(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = JieliRunnerFixture(Path(temporary).resolve())
            fixture.version_file.write_text("1" * 40 + "\n", encoding="utf-8")
            with self.assertRaisesRegex(runner.RunnerError, "commit mismatch"):
                runner.build(fixture.arguments())
            fixture.version_file.write_text(fixture.commit + "\n", encoding="utf-8")
            (fixture.checkout / "SDK" / "sdk-source.c").write_text(
                "int sdk_source = 1;\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(runner.RunnerError, "must be clean"):
                runner.build(fixture.arguments())

    def test_build_rejects_missing_post_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = JieliRunnerFixture(Path(temporary).resolve())
            write_executable(fixture.post_script, 'mkdir -p "$4"; : > "$4/firmware.elf"\n')
            with self.assertRaisesRegex(runner.RunnerError, "non-empty"):
                runner.build(fixture.arguments())

    def test_build_rejects_invalid_identity_values(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = JieliRunnerFixture(Path(temporary).resolve())
            with self.assertRaisesRegex(runner.RunnerError, "invalid JieLi board"):
                runner.build(fixture.arguments(board="bad board"))

    def test_local_post_scripts_are_executable_and_headless(self):
        for name in ("local_post_br23.sh", "local_post_wl82.sh"):
            script = MODULE_PATH.parent / "jieli" / name
            with self.subTest(script=name):
                contents = script.read_text(encoding="utf-8")
                self.assertTrue(os.access(script, os.X_OK))
                self.assertIn("QT_QPA_PLATFORM=offscreen", contents)
                self.assertIn("set -euo pipefail", contents)
                self.assertNotIn("host-client", contents.split("\n", 6)[-1].split("#", 1)[0] or "")
                for output in ("firmware.elf", "symbols.txt", "jl_isd.bin", "jl_isd.fw", "update.ufw"):
                    self.assertIn(f"\"$out/{output}\"", contents)


if __name__ == "__main__":
    unittest.main()
