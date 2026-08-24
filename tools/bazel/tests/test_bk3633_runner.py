from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest import mock



MODULE_PATH = Path(__file__).parents[1] / "bk3633_runner.py"
SPEC = importlib.util.spec_from_file_location("bk3633_runner", MODULE_PATH)
assert SPEC and SPEC.loader
runner = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)


class Bk3633RunnerTest(unittest.TestCase):
    def arguments(self, root: Path, rwip_link_probe: bool = False) -> mock.Mock:
        prebuilt_archive = root / "libh2_firmware_lib.a"
        prebuilt_archive.write_bytes(b"firmware libraries")
        version_file = root / "bk3633-sdk-version.txt"
        version_file.write_text("0" * 40, encoding="utf-8")
        toolchain_archives_file = root / "bk-toolchain-archives.txt"
        toolchain_archives_file.write_text("fixture identity\n", encoding="utf-8")
        sdk_locator = root / "bk3633-sdk-locator.json"
        sdk_locator.write_text(json.dumps({
            "schema": "h2.native-locator.v1",
            "kind": "bk3633-sdk",
            "enabled": True,
            "paths": {"root": str(root / "sdk")},
            "metadata": {},
        }), encoding="utf-8")
        toolchain_locator = root / "bk-toolchain-locator.json"
        toolchain_locator.write_text(json.dumps({
            "schema": "h2.native-locator.v1",
            "kind": "bk-arm-toolchain",
            "enabled": True,
            "paths": {"bin": str(root / "toolchain")},
            "metadata": {},
        }), encoding="utf-8")
        ccache_locator = root / "ccache-locator.json"
        ccache_locator.write_text(json.dumps({
            "schema": "h2.native-locator.v1",
            "kind": "native-ccache-runtime",
            "enabled": False,
            "paths": {},
            "metadata": {},
        }), encoding="utf-8")
        return mock.Mock(
            source_root=str(root),
            project="launcher/Makefile",
            entry="launcher",
            board="bk3633_dev_board",
            image="example",
            native_target="app",
            native_merge="merge.bin",
            version="test-version",
            sdk_version_file=str(version_file),
            toolchain_archives_file=str(toolchain_archives_file),
            sdk_locator=str(sdk_locator),
            toolchain_locator=str(toolchain_locator),
            ccache_runtime_locator=str(ccache_locator),
            expected_toolchain_version="10.3.1",
            binconverter="/bin/sh",
            elf_output=str(root / "outputs/firmware.elf"),
            app_output=str(root / "outputs/app.bin"),
            map_output=str(root / "outputs/firmware.map"),
            recovery_output=str(root / "outputs/merge-crc.bin"),
            manifest_output=str(root / "outputs/manifest.json"),
            prebuilt_component=[
                "h2_firmware_lib=libh2_firmware_lib.a",
            ],
            native_component_source=[],
            native_include_root=[],
            rwip_link_probe=rwip_link_probe,
        )

    def test_prebuilt_components_require_aggregate_firmware_library(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / "libh2_firmware_lib.a").write_bytes(b"firmware libraries")
            self.assertEqual(
                runner.resolve_prebuilt_components(
                    root,
                    ["h2_firmware_lib=libh2_firmware_lib.a"],
                ),
                {
                    "h2_firmware_lib": root / "libh2_firmware_lib.a",
                },
            )
            with self.assertRaisesRegex(
                runner.RunnerError,
                "required prebuilt component is missing: h2_firmware_lib",
            ):
                runner.resolve_prebuilt_components(root, [])

    def test_prebuilt_components_reject_duplicates_and_escaped_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / "libh2_firmware_lib.a").write_bytes(b"firmware libraries")
            with self.assertRaisesRegex(runner.RunnerError, "duplicate"):
                runner.resolve_prebuilt_components(
                    root,
                    [
                        "h2_firmware_lib=libh2_firmware_lib.a",
                        "h2_firmware_lib=libh2_firmware_lib.a",
                    ],
                )
            with self.assertRaisesRegex(runner.RunnerError, "escapes"):
                runner.resolve_prebuilt_components(
                    root,
                    ["h2_firmware_lib=../libh2_firmware_lib.a"],
                )

    def test_native_component_manifest_has_stable_sources_includes_and_archives(self):
        manifest = runner.render_native_component_manifest(
            [Path("/repo/a.c"), Path("/repo/b.c")],
            [Path("/repo/include"), Path("/repo/private")],
            {
                "zeta": Path("/archives/zeta.a"),
                "alpha": Path("/archives/alpha.a"),
            },
        )
        self.assertIn("H2_BAZEL_NATIVE_SRCS := /repo/a.c \\\n/repo/b.c", manifest)
        self.assertIn(
            "H2_BAZEL_NATIVE_INCLUDES := -I/repo/include -I/repo/private",
            manifest,
        )
        self.assertIn(
            "H2_BAZEL_ARCHIVES := /archives/alpha.a /archives/zeta.a",
            manifest,
        )
        self.assertLess(manifest.index("/archives/alpha.a"), manifest.index("/archives/zeta.a"))
        self.assertIn("H2_BAZEL_PREBUILT_ALPHA := /archives/alpha.a", manifest)

    def test_native_component_sources_reject_duplicate_owners(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "shared.c").write_text("int shared;\n", encoding="utf-8")
            with self.assertRaisesRegex(runner.RunnerError, "multiple owners"):
                runner.resolve_native_component_sources(
                    root,
                    ["first=shared.c", "second=shared.c"],
                )

    def test_native_manifest_normalizes_declared_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            names = ["app.elf", "app.bin", "app.map", "merge.bin"]
            for name in names:
                (root / name).write_bytes(b"data")
            (root / "manifest.json").write_text(
                json.dumps(
                    {
                        "target": "bk3633",
                        "board": "bk3633_dev_board",
                        "image": "example",
                        "stack": "allroles",
                        "elf": "app.elf",
                        "bin": "app.bin",
                        "map": "app.map",
                        "merge": "merge.bin",
                        "runtime": "baremetal_ringbuf",
                        "toolchain": "arm-none-eabi-gcc 10.3.1",
                        "binconverter": "portable-c",
                        "persistent_layout": {"source": "test"},
                        "merge_inputs": ["bim.bin", "stack.bin", "app.bin"],
                    }
                ),
                encoding="utf-8",
            )
            arguments = mock.Mock(
                board="bk3633_dev_board",
                image="example",
                native_target="app",
                native_merge="merge.bin",
                expected_toolchain_version="10.3.1",
            )
            data, files = runner.native_manifest(root, arguments)
            self.assertEqual(data["runtime"], "baremetal_ringbuf")
            self.assertEqual(set(files), {"elf", "bin", "map", "merge"})

    def test_native_manifest_rejects_wrong_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "manifest.json").write_text(
                '{"target":"esp32s3"}', encoding="utf-8"
            )
            arguments = mock.Mock(
                board="bk3633_dev_board",
                image="example",
                native_target="app",
                native_merge="merge.bin",
                expected_toolchain_version="10.3.1",
            )
            with self.assertRaisesRegex(runner.RunnerError, "target mismatch"):
                runner.native_manifest(root, arguments)

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

    def test_required_environment_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            arguments = self.arguments(Path(temporary))
            Path(arguments.sdk_locator).write_text(json.dumps({
                "schema": "h2.native-locator.v1",
                "kind": "bk3633-sdk",
                "enabled": False,
                "paths": {},
            }), encoding="utf-8")
            with self.assertRaisesRegex(
                runner.NativeRuntimeError, "locator is not configured"
            ):
                runner.required_environment(arguments)

    def test_required_environment_ignores_non_allowlisted_values(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            arguments = self.arguments(root)
            with mock.patch.dict(
                os.environ,
                {
                    "HOME": "/untrusted-home",
                    "HTTP_PROXY": "http://proxy.invalid",
                    "PATH": "/untrusted-bin",
                    "PYTHONPATH": "/untrusted-python",
                },
                clear=True,
            ):
                environment = runner.required_environment(arguments)
            self.assertEqual(environment["BK3633_PATH"], str(root / "sdk"))
            self.assertEqual(
                environment["COMPILER_TOOLCHAIN_PATH"],
                str(root / "toolchain"),
            )
            self.assertEqual(environment["H2_GIZOS_ROOT"], str(runner.GIZOS_ROOT))
            self.assertEqual(environment["H2_NATIVE_BUILD_JOBS"], "4")
            self.assertNotIn("HTTP_PROXY", environment)
            self.assertNotIn("PYTHONPATH", environment)
            self.assertNotIn("/untrusted-bin", environment["PATH"])

    def test_required_environment_ignores_ambient_native_build_jobs(self):
        for value in ("0", "invalid"):
            with tempfile.TemporaryDirectory() as temporary, self.subTest(value=value):
                arguments = self.arguments(Path(temporary).resolve())
                with mock.patch.dict(
                    os.environ,
                    {"H2_NATIVE_BUILD_JOBS": value},
                    clear=True,
                ):
                    environment = runner.required_environment(arguments)
                self.assertEqual(environment["H2_NATIVE_BUILD_JOBS"], "4")

    def test_executable_rejects_non_executable_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "tool"
            path.write_text("tool", encoding="utf-8")
            path.chmod(stat.S_IRUSR | stat.S_IWUSR)
            with self.assertRaisesRegex(runner.RunnerError, "not executable"):
                runner.executable(path, "tool")

    def test_resolve_under_rejects_absolute_and_parent_paths(self):
        root = Path("/source")
        for value in ("/outside/Makefile", "launcher/../Makefile"):
            with self.subTest(value=value):
                with self.assertRaisesRegex(runner.RunnerError, "escapes"):
                    runner.resolve_under(root, value, "project Makefile")

    def test_validate_sdk_rejects_wrong_commit(self):
        checkout = Path("/sdk")
        with mock.patch.object(
            runner,
            "git_output",
            side_effect=[str(checkout), "actual-commit"],
        ):
            with self.assertRaisesRegex(runner.RunnerError, "commit mismatch"):
                runner.validate_sdk(
                    Path("/usr/bin/git"), checkout, "expected-commit", {}
                )

    def test_sdk_version_file_rejects_invalid_commit(self):
        with tempfile.TemporaryDirectory() as temporary:
            version_file = Path(temporary) / "version.txt"
            version_file.write_text("not-a-commit", encoding="utf-8")
            with self.assertRaisesRegex(
                runner.RunnerError,
                "invalid expected BK3633 SDK commit",
            ):
                runner.read_expected_commit(str(version_file), "BK3633 SDK")

    def test_validate_toolchain_rejects_wrong_version(self):
        completed = mock.Mock(stdout="9.3.1\n", returncode=0)
        with (
            mock.patch.object(runner, "executable", return_value=Path("/tool/gcc")),
            mock.patch.object(runner, "run", return_value=completed),
        ):
            with self.assertRaisesRegex(runner.RunnerError, "toolchain mismatch"):
                runner.validate_toolchain(Path("/tool"), "10.3.1", {})

    def test_build_rejects_dirty_sdk_before_native_command(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "launcher").mkdir()
            (root / "launcher/Makefile").touch()
            with (
                mock.patch.object(
                    runner,
                    "required_environment",
                    return_value={
                        "BK3633_PATH": str(root / "sdk"),
                        "BK_TOOLCHAIN_ARCHIVE": str(root / "bk-toolchain.tar.bz2"),
                        "COMPILER_TOOLCHAIN_PATH": str(root / "toolchain"),
                        "PATH": "/usr/bin",
                    },
                ),
                mock.patch.object(runner, "command_path", return_value=Path("/bin/sh")),
                mock.patch.object(runner, "executable", return_value=Path("/bin/sh")),
                mock.patch.object(runner, "validate_sdk"),
                mock.patch.object(runner, "validate_toolchain"),
                mock.patch.object(runner, "checkout_state", return_value=" M SDK/file"),
                mock.patch.object(runner.subprocess, "run") as native_run,
            ):
                with self.assertRaisesRegex(runner.RunnerError, "must be clean"):
                    runner.build(self.arguments(root))
                native_run.assert_not_called()

    def test_build_reports_sdk_mutation_even_when_native_build_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "launcher").mkdir()
            (root / "launcher/Makefile").touch()
            with (
                mock.patch.object(
                    runner,
                    "required_environment",
                    return_value={
                        "BK3633_PATH": str(root / "sdk"),
                        "BK_TOOLCHAIN_ARCHIVE": str(root / "bk-toolchain.tar.bz2"),
                        "COMPILER_TOOLCHAIN_PATH": str(root / "toolchain"),
                        "PATH": "/usr/bin",
                    },
                ),
                mock.patch.object(runner, "command_path", return_value=Path("/bin/sh")),
                mock.patch.object(runner, "executable", return_value=Path("/bin/sh")),
                mock.patch.object(runner, "validate_sdk"),
                mock.patch.object(runner, "validate_toolchain"),
                mock.patch.object(
                    runner, "checkout_state", side_effect=["", "?? SDK/output.bin"]
                ),
                mock.patch.object(
                    runner.subprocess,
                    "run",
                    return_value=mock.Mock(returncode=2),
                ),
            ):
                with self.assertRaisesRegex(runner.RunnerError, "checkout changed"):
                    runner.build(self.arguments(root, rwip_link_probe=True))

    def test_build_runs_probe_and_release_without_parallel_make_goals(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "launcher").mkdir()
            (root / "launcher/Makefile").touch()
            calls: list[list[str]] = []

            def record(command, **_kwargs):
                calls.append(command)
                return mock.Mock(returncode=0)

            with (
                mock.patch.object(
                    runner,
                    "required_environment",
                    return_value={
                        "BK3633_PATH": str(root / "sdk"),
                        "BK_TOOLCHAIN_ARCHIVE": str(root / "bk-toolchain.tar.bz2"),
                        "COMPILER_TOOLCHAIN_PATH": str(root / "toolchain"),
                        "PATH": "/usr/bin",
                        "H2_NATIVE_BUILD_JOBS": "4",
                    },
                ),
                mock.patch.object(
                    runner, "command_path", return_value=Path("/bin/make")
                ),
                mock.patch.object(runner, "executable", return_value=Path("/bin/sh")),
                mock.patch.object(runner, "validate_sdk"),
                mock.patch.object(runner, "validate_toolchain"),
                mock.patch.object(runner, "checkout_state", return_value=""),
                mock.patch.object(runner.subprocess, "run", side_effect=record),
                mock.patch.object(
                    runner,
                    "native_manifest",
                    side_effect=runner.RunnerError("stop after native commands"),
                ),
            ):
                with self.assertRaisesRegex(
                    runner.RunnerError, "stop after native commands"
                ):
                    runner.build(self.arguments(root, rwip_link_probe=True))

            self.assertEqual(
                [call[-1] for call in calls],
                ["sdk_probe", "rwip_link_probe", "release"],
            )
            sdk_build_dir = next(
                value for value in calls[0] if value.startswith("BUILD_DIR=")
            )
            rwip_build_dir = next(
                value for value in calls[1] if value.startswith("BUILD_DIR=")
            )
            release_build_dir = next(
                value for value in calls[2] if value.startswith("BUILD_DIR=")
            )
            self.assertEqual(sdk_build_dir, release_build_dir)
            self.assertNotEqual(
                rwip_build_dir,
                release_build_dir,
            )

    def test_build_uses_ccache_wrapped_toolchain_when_configured(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "launcher").mkdir()
            (root / "launcher/Makefile").touch()
            wrapped = root / "wrapped-toolchain"
            ccache = root / "ccache"
            ccache.write_text(
                "#!/bin/sh\nprintf 'ccache version 4.13.6\\n'\n",
                encoding="utf-8",
            )
            ccache.chmod(0o755)
            ccache_helper = root / "ccache-storage-https"
            ccache_helper.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            ccache_helper.chmod(0o755)
            ccache_token = root / "ccache-token"
            ccache_token.write_text("fixture-token", encoding="utf-8")
            ccache_token.chmod(0o600)
            cache = root / "cache"
            cache.mkdir()
            runtime_manifest = root / "runtime.json"
            runtime_manifest.write_text(json.dumps({
                "schema": "h2.native-ccache-runtime.v1",
                "ccache": "ccache",
                "cache_root": "cache",
                "remote_base_url": "https://storage.googleapis.com/cache/ccache",
                "storage_helper": "ccache-storage-https",
                "token_file": "ccache-token",
            }), encoding="utf-8")
            arguments = self.arguments(root)
            Path(arguments.ccache_runtime_locator).write_text(json.dumps({
                "schema": "h2.native-locator.v1",
                "kind": "native-ccache-runtime",
                "enabled": True,
                "paths": {
                    "root": str(root),
                    "manifest": str(runtime_manifest),
                },
                "metadata": {},
            }), encoding="utf-8")
            calls: list[tuple[list[str], dict[str, str]]] = []

            def record(command, **kwargs):
                if Path(command[0]).name == "ccache" and command[1:] == ["--version"]:
                    return mock.Mock(
                        returncode=0,
                        stdout="ccache version 4.13.6\n",
                        stderr="",
                    )
                calls.append((command, kwargs["env"]))
                return mock.Mock(returncode=0)

            with (
                mock.patch.object(
                    runner,
                    "required_environment",
                    return_value={
                        "BK3633_PATH": str(root / "sdk"),
                        "BK_TOOLCHAIN_ARCHIVE": str(root / "bk-toolchain.tar.bz2"),
                        "COMPILER_TOOLCHAIN_PATH": str(root / "toolchain"),
                        "PATH": "/usr/bin",
                        "H2_NATIVE_BUILD_JOBS": "4",
                    },
                ),
                mock.patch.object(
                    runner, "command_path", return_value=Path("/bin/make")
                ),
                mock.patch.object(runner, "executable", return_value=Path("/bin/sh")),
                mock.patch.object(runner, "validate_sdk"),
                mock.patch.object(runner, "validate_toolchain"),
                mock.patch.object(runner, "checkout_state", return_value=""),
                mock.patch.object(
                    runner,
                    "create_wrapped_toolchain",
                    return_value=wrapped,
                ) as create_wrapper,
                mock.patch.object(runner.subprocess, "run", side_effect=record),
                mock.patch.object(
                    runner,
                    "native_manifest",
                    side_effect=runner.RunnerError("stop after native commands"),
                ),
            ):
                with self.assertRaisesRegex(
                    runner.RunnerError, "stop after native commands"
                ):
                    runner.build(arguments)

            create_wrapper.assert_called_once()
            self.assertEqual(len(calls), 2)
            for command, environment in calls:
                self.assertIn(f"COMPILER_TOOLCHAIN_PATH={wrapped}", command)
                self.assertEqual(
                    environment["COMPILER_TOOLCHAIN_PATH"],
                    str(wrapped),
                )
                self.assertIn(
                    "https://storage.googleapis.com/cache/ccache/bk ",
                    environment["CCACHE_REMOTE_STORAGE"],
                )
                self.assertIn(
                    "@bearer-token=fixture-token",
                    environment["CCACHE_REMOTE_STORAGE"],
                )
                self.assertEqual(environment["CCACHE_RESHARE"], "1")


if __name__ == "__main__":
    unittest.main()
