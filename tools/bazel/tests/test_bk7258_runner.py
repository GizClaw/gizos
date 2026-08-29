from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest



ROOT = Path(__file__).resolve().parents[3]
RUNNER = ROOT / "tools" / "bazel" / "bk7258_runner.py"


FAKE_MAKE = r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import subprocess
import sys

if "H2_RUNNER_SECRET" in os.environ:
    raise SystemExit("unapproved environment variable escaped into make")
if Path(os.environ["H2_FIRMWARE_VERSION_FILE"]).read_text() != "test-version":
    raise SystemExit("runner did not provide the internal firmware version file")
if "-j4" not in sys.argv:
    raise SystemExit("runner did not bound BK7258 build parallelism")
source_root = Path(os.environ["H2_REPO_ROOT"])
gizos_root = Path(os.environ["H2_GIZOS_ROOT"])
if not gizos_root.joinpath(
    "native_component_src/bk7258/ap/cmake/h2_bazel_archive.cmake"
).is_file():
    raise SystemExit("runner did not provide the GizOS source root")
if Path(os.environ["H2_BAZEL_PREBUILT_H2_LIBCO"]).read_bytes() != b"libco":
    raise SystemExit("runner did not provide the Bazel-built libco archive")
if not source_root.joinpath("partition.csv").is_file():
    raise SystemExit("runner did not provide the read-only source root")
assignments = dict(value.split("=", 1) for value in sys.argv if "=" in value)
if assignments["PROJECT"] != "h2_fixture":
    raise SystemExit("runner did not provide the declared SDK project")
if assignments["COMPILER_TOOLCHAIN_PATH"] != os.environ["COMPILER_TOOLCHAIN_PATH"]:
    raise SystemExit("runner did not provide the allowlisted toolchain path")
project = Path(assignments["PROJECT_DIR"])
build = Path(assignments["BUILD_DIR"])
ap_config = project / "ap/config/bk7258_ap/config"
cp_config = project / "cp/config/bk7258/config"
if "CONFIG_BOARD_AP=y" not in ap_config.read_text() or "CONFIG_IMAGE_AP=y" not in ap_config.read_text():
    raise SystemExit("runner did not merge AP config layers")
if "CONFIG_BOARD_CP=y" not in cp_config.read_text() or "CONFIG_LAYOUT_CP=y" not in cp_config.read_text():
    raise SystemExit("runner did not merge CP config layers")
if project.joinpath("ap/config/bk7258_ap/usr_gpio_cfg.h").read_text() != "ap gpio\n":
    raise SystemExit("runner did not stage the AP GPIO profile")
if not project.parents[1].joinpath("partition.csv").is_file():
    raise SystemExit("declared launcher support file was not copied")
mode_path = project / "mode"
mode = mode_path.read_text(encoding="utf-8").strip() if mode_path.exists() else "success"
if mode == "ccache":
    if os.environ.get("CCACHE_NAMESPACE") != "bk7258":
        raise SystemExit("runner did not select the BK7258 ccache namespace")
    if not Path(os.environ["CCACHE_DIR"]).is_dir():
        raise SystemExit("runner did not create the BK7258 compiler cache")
    if os.environ.get("ARMINO_CCACHE_ENABLE") != "1":
        raise SystemExit("runner did not enable Armino ccache")
    if not Path(os.environ["PATH"].split(os.pathsep)[0], "ccache").is_symlink():
        raise SystemExit("runner did not expose ccache to Armino")
    remote = os.environ.get("CCACHE_REMOTE_STORAGE", "")
    if "https://storage.googleapis.com/cache/ccache/bk " not in remote:
        raise SystemExit("runner did not select the BK remote ccache prefix")
    if "@bearer-token=fixture-token" not in remote:
        raise SystemExit("runner did not authenticate the BK remote ccache")
    if os.environ.get("CCACHE_RESHARE") != "1":
        raise SystemExit("runner did not reshare local BK ccache hits")
if mode == "native-graph":
    manifest = Path(os.environ["H2_BAZEL_COMPONENT_MANIFEST"])
    manifest_text = manifest.read_text(encoding="utf-8")
    expected = (
        "H2_BAZEL_AP_COMPONENT_DIRS",
        "/components/ap/h2_board\"",
        "H2_BAZEL_CP_COMPONENT_DIRS",
        "/components/cp/h2_transport\"",
        "H2_BAZEL_COMPONENT_SRCS_AP_H2_BOARD",
        "/components/board/board.c\"",
        "H2_BAZEL_COMPONENT_INCLUDES_AP_H2_BOARD",
        "/components/board/include\"",
        "H2_BAZEL_COMPONENT_SRCS_CP_H2_TRANSPORT",
        "/components/transport/transport.c\"",
        "H2_BAZEL_COMPONENT_INCLUDES_CP_H2_TRANSPORT",
        "/components/transport/include\"",
    )
    for value in expected:
        if value not in manifest_text:
            raise SystemExit(f"native component manifest is missing {value}")
    for unit, name in (("ap", "h2_board"), ("cp", "h2_transport")):
        staged = manifest.parents[3] / "components" / unit / name
        if not staged.joinpath("CMakeLists.txt").is_file():
            raise SystemExit(f"native component {unit}:{name} was not staged by name")
        if not staged.joinpath("include/component.h").is_file():
            raise SystemExit(f"native component {unit}:{name} omitted its declared header")
if mode == "make-failure":
    raise SystemExit(23)
project.joinpath("generated.lock").write_text("temporary\n", encoding="utf-8")
native = build / "bk7258" / project.name
files = {
    "bk7258_ap/app.elf": b"ap-elf",
    "bk7258_ap/app.map": b"ap-map",
    "bk7258_ap/app.bin": b"ap-image",
    "bk7258/app.elf": b"cp-elf",
    "bk7258/app.map": b"cp-map",
    "bk7258/app.bin": b"cp-image",
    "package/app_ab_crc.rbl": b"managed-app",
    "package/all-app.bin": b"recovery",
    "package/app_ab_ota.rbl": b"not-public",
}
if mode == "missing-ap-output":
    files.pop("bk7258_ap/app.map")
elif mode == "missing-cp-output":
    files.pop("bk7258/app.elf")
elif mode == "missing-package-output":
    files.pop("package/app_ab_crc.rbl")
elif mode == "empty-ap-output":
    files["bk7258_ap/app.bin"] = b""
elif mode == "empty-cp-output":
    files["bk7258/app.map"] = b""
elif mode == "empty-package-output":
    files["package/all-app.bin"] = b""
for relative, content in files.items():
    output = native / relative
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(content)
metadata = (
    "partitions.json",
    "bk_ota_partitions.json",
    "bk_package.json",
    "configurationab.json",
)
for name in metadata:
    output = native / "partitions" / name
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({"name": name}), encoding="utf-8")
if mode == "invalid-json":
    native.joinpath("partitions/partitions.json").write_text("{", encoding="utf-8")
elif mode == "non-object-json":
    native.joinpath("partitions/partitions.json").write_text("[]", encoding="utf-8")
elif mode == "missing-metadata":
    native.joinpath("partitions/bk_package.json").unlink()
elif mode == "escaping-metadata":
    metadata_path = native / "partitions" / "partitions.json"
    metadata_path.unlink()
    outside = build.parent / "outside.json"
    outside.write_text("{}", encoding="utf-8")
    metadata_path.symlink_to(outside)
if mode == "mutate-sdk":
    Path(os.environ["BK7258_PATH"]).joinpath("Makefile").write_text(
        "mutated by native build\n", encoding="utf-8"
    )
elif mode == "advance-sdk-head":
    sdk = Path(os.environ["BK7258_PATH"])
    sdk.joinpath("post-build-mutation").write_text("tracked\n", encoding="utf-8")
    subprocess.run(["git", "-C", str(sdk), "add", "post-build-mutation"], check=True)
    subprocess.run(
        [
            "git",
            "-C",
            str(sdk),
            "-c",
            "user.name=runner-test",
            "-c",
            "user.email=runner-test@example.invalid",
            "commit",
            "-qm",
            "native mutation",
        ],
        check=True,
    )
elif mode == "mutate-source":
    source_root.joinpath("partition.csv").write_text(
        "mutated by native build\n", encoding="utf-8"
    )
'''


class Bk7258RunnerTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.source = self.root / "repo"
        self.project = self.source / "projects" / "fixture"
        self.project.mkdir(parents=True)
        (self.project / "CMakeLists.txt").write_text(
            'set(H2_BK_TARGET "bk7258")\n', encoding="utf-8"
        )
        self.project.joinpath("ap/config/bk7258_ap").mkdir(parents=True)
        self.project.joinpath("cp/config/bk7258").mkdir(parents=True)
        self.project.joinpath("ap/config/bk7258_ap/config").write_text(
            "CONFIG_IMAGE_AP=y\n", encoding="utf-8"
        )
        config_root = self.source / "boards" / "fixture" / "bk7258"
        config_root.joinpath("ap").mkdir(parents=True)
        config_root.joinpath("cp").mkdir(parents=True)
        config_root.joinpath("layouts/h2loader").mkdir(parents=True)
        config_root.joinpath("ap.defaults").write_text("CONFIG_BOARD_AP=y\n")
        config_root.joinpath("cp.defaults").write_text("CONFIG_BOARD_CP=y\n")
        config_root.joinpath("layouts/h2loader/ap.defaults").write_text(
            "CONFIG_LAYOUT_AP=y\n"
        )
        config_root.joinpath("layouts/h2loader/cp.defaults").write_text(
            "CONFIG_LAYOUT_CP=y\n"
        )
        config_root.joinpath("ap/usr_gpio_cfg.h").write_text("ap gpio\n")
        config_root.joinpath("cp/usr_gpio_cfg.h").write_text("cp gpio\n")
        config_root.joinpath("ram_regions.csv").write_text("name,type,offset,size\n")
        (self.source / "partition.csv").write_text("partition\n", encoding="utf-8")
        for name in ("board", "transport"):
            component = self.source / "components" / name
            component.joinpath("include").mkdir(parents=True)
            component.joinpath("CMakeLists.txt").write_text(
                f"# {name} component\n", encoding="utf-8"
            )
            component.joinpath("include/component.h").write_text(
                "#pragma once\n", encoding="utf-8"
            )
            component.joinpath(f"{name}.c").write_text(
                f"void {name}(void) {{}}\n", encoding="utf-8"
            )
        self.prebuilt_archive = self.source / "libh2_libco.a"
        self.prebuilt_archive.write_bytes(b"libco")
        subprocess.run(["git", "init", "-q"], cwd=self.source, check=True)
        subprocess.run(["git", "add", "."], cwd=self.source, check=True)
        subprocess.run(
            [
                "git",
                "-c",
                "user.name=runner-test",
                "-c",
                "user.email=runner-test@example.invalid",
                "commit",
                "-qm",
                "fixture",
            ],
            cwd=self.source,
            check=True,
        )

        self.sdk = self.root / "bk-sdk"
        self.sdk.mkdir()
        (self.sdk / "Makefile").write_text("all:\n\t@true\n", encoding="utf-8")
        subprocess.run(["git", "init", "-q"], cwd=self.sdk, check=True)
        subprocess.run(["git", "add", "."], cwd=self.sdk, check=True)
        subprocess.run(
            [
                "git",
                "-c",
                "user.name=runner-test",
                "-c",
                "user.email=runner-test@example.invalid",
                "commit",
                "-qm",
                "fixture",
            ],
            cwd=self.sdk,
            check=True,
        )
        self.commit = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=self.sdk, text=True
        ).strip()

        self.tool_bin = self.root / "bin"
        self.tool_bin.mkdir()
        make = self.tool_bin / "make"
        make.write_text(FAKE_MAKE, encoding="utf-8")
        make.chmod(0o755)
        fake_python = (
            f"#!{sys.executable}\n"
            "import os\n"
            "import sys\n"
            "if len(sys.argv) >= 3 and sys.argv[1] == '-c' and "
            "'distutils.dir_util' in sys.argv[2]:\n"
            "    print('3.11')\n"
            "    raise SystemExit(0)\n"
            f"os.execv({sys.executable!r}, [{sys.executable!r}, *sys.argv[1:]])\n"
        )
        for name in ("python", "python3", "python3.11"):
            executable = self.tool_bin / name
            executable.write_text(fake_python, encoding="utf-8")
            executable.chmod(0o755)
        self.toolchain = self.root / "toolchain" / "bin"
        self.toolchain.mkdir(parents=True)
        compiler = self.toolchain / "arm-none-eabi-gcc"
        compiler.write_text("#!/bin/sh\nprintf '10.3.1\\n'\n", encoding="utf-8")
        compiler.chmod(0o755)
        shutil.copy2(make, self.toolchain / "make")
        self.ccache = self.root / "fake-ccache"
        self.ccache.write_text(
            "#!/bin/sh\ntest \"$1\" = --version && echo 'ccache version 4.13.6'\n",
            encoding="utf-8",
        )
        self.ccache.chmod(0o755)
        self.ccache_helper = self.root / "ccache-storage-https"
        self.ccache_helper.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        self.ccache_helper.chmod(0o755)
        self.ccache_token = self.root / "ccache-token"
        self.ccache_token.write_text("fixture-token", encoding="utf-8")
        self.ccache_token.chmod(0o600)

    def tearDown(self):
        self.temporary.cleanup()

    def _run(
        self,
        name: str = "result",
        *,
        mode: str = "success",
        commit: str | None = None,
        target: str = "bk7258",
        environment_updates: dict[str, str | None] | None = None,
        prebuilt_component: str = "h2_libco=libh2_libco.a",
        precreate_metadata_output: bool = False,
        source_root: Path | None = None,
        native_graph: bool = False,
        extra_arguments: list[str] | None = None,
    ) -> tuple[subprocess.CompletedProcess[str], Path]:
        mode_path = self.project / "mode"
        if mode == "success":
            mode_path.unlink(missing_ok=True)
        else:
            mode_path.write_text(mode, encoding="utf-8")
        output = self.root / name
        version_file = self.root / f"{name}-sdk-version.txt"
        version_file.write_text(commit or self.commit, encoding="utf-8")
        toolchain_archives_file = self.root / f"{name}-toolchain-archives.txt"
        toolchain_archives_file.write_text("fixture identity\n", encoding="utf-8")
        if precreate_metadata_output:
            (output / "partition-metadata").mkdir(parents=True)
        environment = dict(os.environ)
        environment.update(
            {
                "BK_SDK_DIR": str(self.sdk),
                "COMPILER_TOOLCHAIN_PATH": str(self.toolchain),
                "PATH": f"{self.tool_bin}{os.pathsep}{environment['PATH']}",
                "H2_RUNNER_SECRET": "must-not-propagate",
            }
        )
        for key, value in (environment_updates or {}).items():
            if value is None:
                environment.pop(key, None)
            else:
                environment[key] = value
        sdk_root = (environment_updates or {}).get("BK_SDK_DIR", str(self.sdk))
        toolchain_bin = (environment_updates or {}).get(
            "COMPILER_TOOLCHAIN_PATH", str(self.toolchain)
        )
        sdk_locator = self.root / f"{name}-bk7258-sdk-locator.json"
        sdk_locator.write_text(json.dumps({
            "schema": "h2.native-locator.v1",
            "kind": "bk7258-sdk",
            "enabled": bool(sdk_root),
            "paths": {"root": sdk_root} if sdk_root else {},
            "metadata": {},
        }), encoding="utf-8")
        toolchain_locator = self.root / f"{name}-toolchain-locator.json"
        toolchain_locator.write_text(json.dumps({
            "schema": "h2.native-locator.v1",
            "kind": "bk-arm-toolchain",
            "enabled": bool(toolchain_bin),
            "paths": {"bin": toolchain_bin} if toolchain_bin else {},
            "metadata": {},
        }), encoding="utf-8")
        ccache_root = self.root / f"{name}-ccache-runtime"
        ccache_root.mkdir(exist_ok=True)
        ccache_enabled = "H2_NATIVE_CCACHE" in (environment_updates or {})
        if ccache_enabled:
            (ccache_root / "bin").mkdir(exist_ok=True)
            (ccache_root / "cache").mkdir(exist_ok=True)
            shutil.copy2(self.ccache, ccache_root / "bin/ccache")
            shutil.copy2(
                self.ccache_helper,
                ccache_root / "bin/ccache-storage-https",
            )
            (ccache_root / "token").write_text("fixture-token", encoding="utf-8")
            (ccache_root / "token").chmod(0o600)
            (ccache_root / "runtime.json").write_text(json.dumps({
                "schema": "h2.native-ccache-runtime.v1",
                "ccache": "bin/ccache",
                "cache_root": "cache",
                "remote_base_url": "https://storage.googleapis.com/cache/ccache",
                "storage_helper": "bin/ccache-storage-https",
                "token_file": "token",
            }), encoding="utf-8")
        ccache_locator = ccache_root / "locator.json"
        ccache_locator.write_text(json.dumps({
            "schema": "h2.native-locator.v1",
            "kind": "native-ccache-runtime",
            "enabled": ccache_enabled,
            "paths": {
                "root": str(ccache_root),
                "manifest": str(ccache_root / "runtime.json"),
            } if ccache_enabled else {},
            "metadata": {},
        }), encoding="utf-8")
        command = [
            sys.executable,
            str(RUNNER),
            "--source-root",
            str(source_root or self.source),
            "--project",
            "projects/fixture/CMakeLists.txt",
            "--project-name",
            "h2_fixture",
            "--target",
            target,
            "--board",
            "fixture-board",
            "--version",
            "test-version",
            "--sdk-version-file",
            str(version_file),
            "--toolchain-archives-file",
            str(toolchain_archives_file),
            "--sdk-locator",
            str(sdk_locator),
            "--toolchain-locator",
            str(toolchain_locator),
            "--ccache-runtime-locator",
            str(ccache_locator),
            "--expected-python-version",
            f"{sys.version_info.major}.{sys.version_info.minor}",
            "--expected-toolchain-version",
            "10.3.1",
            "--support-file",
            "partition.csv",
            "--ram-regions",
            "boards/fixture/bk7258/ram_regions.csv",
            "--ap-config",
            "boards/fixture/bk7258/ap.defaults",
            "--ap-config",
            "boards/fixture/bk7258/layouts/h2loader/ap.defaults",
            "--cp-config",
            "boards/fixture/bk7258/cp.defaults",
            "--cp-config",
            "boards/fixture/bk7258/layouts/h2loader/cp.defaults",
            "--ap-gpio",
            "boards/fixture/bk7258/ap/usr_gpio_cfg.h",
            "--cp-gpio",
            "boards/fixture/bk7258/cp/usr_gpio_cfg.h",
            "--prebuilt-component",
            prebuilt_component,
            "--ap-elf-output",
            str(output / "ap/firmware.elf"),
            "--ap-map-output",
            str(output / "ap/firmware.map"),
            "--ap-image-output",
            str(output / "ap/app.bin"),
            "--cp-elf-output",
            str(output / "cp/firmware.elf"),
            "--cp-map-output",
            str(output / "cp/firmware.map"),
            "--cp-image-output",
            str(output / "cp/app.bin"),
            "--managed-app-output",
            str(output / "app_ab_crc.rbl"),
            "--recovery-output",
            str(output / "all-app.bin"),
            "--partition-metadata-output",
            str(output / "partition-metadata"),
        ]
        if native_graph:
            command.extend(
                [
                    "--native-component",
                    "ap:h2_board=components/board",
                    "--native-component-file",
                    "ap:h2_board=components/board/CMakeLists.txt",
                    "--native-component-file",
                    "ap:h2_board=components/board/include/component.h",
                    "--native-component-include",
                    "ap:h2_board=components/board/include",
                    "--native-component-source",
                    "ap:h2_board=components/board/board.c",
                    "--native-component",
                    "cp:h2_transport=components/transport",
                    "--native-component-file",
                    "cp:h2_transport=components/transport/CMakeLists.txt",
                    "--native-component-file",
                    "cp:h2_transport=components/transport/include/component.h",
                    "--native-component-include",
                    "cp:h2_transport=components/transport/include",
                    "--native-component-source",
                    "cp:h2_transport=components/transport/transport.c",
                ]
            )
        command.extend(extra_arguments or [])
        return (
            subprocess.run(command, env=environment, text=True, capture_output=True),
            output,
        )

    def test_success_publishes_only_validated_native_outputs(self):
        result, output = self._run()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output.joinpath("ap/firmware.elf").read_bytes(), b"ap-elf")
        self.assertEqual(output.joinpath("cp/app.bin").read_bytes(), b"cp-image")
        self.assertEqual(output.joinpath("app_ab_crc.rbl").read_bytes(), b"managed-app")
        self.assertFalse(output.joinpath("app_ab_ota.rbl").exists())
        for name in (
            "partitions.json",
            "bk_ota_partitions.json",
            "bk_package.json",
            "configurationab.json",
        ):
            self.assertIsInstance(
                json.loads(output.joinpath("partition-metadata", name).read_text()),
                dict,
            )
        self.assertFalse(self.project.joinpath("generated.lock").exists())

    def test_ccache_environment_reaches_armino(self):
        result, _ = self._run(
            name="ccache",
            mode="ccache",
            environment_updates={
                "H2_NATIVE_CCACHE": str(self.ccache),
                "H2_NATIVE_CCACHE_ROOT": str(self.root / "native-cache"),
                "H2_NATIVE_CCACHE_REMOTE_BASE_URL": (
                    "https://storage.googleapis.com/cache/ccache"
                ),
                "H2_NATIVE_CCACHE_STORAGE_HELPER": str(self.ccache_helper),
                "H2_NATIVE_CCACHE_TOKEN_FILE": str(self.ccache_token),
            },
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_native_component_graph_is_staged_and_isolated_by_execution_unit(self):
        result, output = self._run(
            name="native-graph",
            mode="native-graph",
            native_graph=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(output.joinpath("all-app.bin").is_file())

    def test_native_component_inputs_require_a_matching_descriptor(self):
        result, _ = self._run(
            name="missing-native-descriptor",
            extra_arguments=[
                "--native-component-source",
                "ap:missing=components/board/board.c",
            ],
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("has no component descriptor: ap:missing", result.stderr)

    def test_prebuilt_archive_must_stay_inside_source_root(self):
        result, _ = self._run(
            name="escaping-prebuilt",
            prebuilt_component="h2_libco=../libh2_libco.a",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid BK7258 prebuilt archive path", result.stderr)

    def test_success_accepts_bazel_precreated_tree_artifact(self):
        result, output = self._run(
            name="precreated-tree",
            precreate_metadata_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(output.joinpath("partition-metadata/partitions.json").is_file())

    def test_success_resolves_checkout_from_bazel_style_source_symlinks(self):
        execroot = self.root / "execroot"
        (execroot / "projects").mkdir(parents=True)
        (execroot / "projects/fixture").symlink_to(
            self.project,
            target_is_directory=True,
        )
        (execroot / "boards").symlink_to(
            self.source / "boards",
            target_is_directory=True,
        )
        (execroot / "partition.csv").symlink_to(self.source / "partition.csv")
        (execroot / "libh2_libco.a").write_bytes(self.prebuilt_archive.read_bytes())
        result, output = self._run(name="symlink-source", source_root=execroot)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(output.joinpath("all-app.bin").is_file())

    def test_two_invocations_use_isolated_work_directories(self):
        with ThreadPoolExecutor(max_workers=2) as executor:
            futures = [executor.submit(self._run, f"result-{index}") for index in range(2)]
        for future in futures:
            result, output = future.result()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(output.joinpath("all-app.bin").is_file())

    def test_missing_locator_fails_closed(self):
        for name in ("BK_SDK_DIR", "COMPILER_TOOLCHAIN_PATH"):
            with self.subTest(name=name):
                result, _ = self._run(
                    name=f"missing-{name.lower()}",
                    environment_updates={name: None},
                )
                self.assertIn("locator is not configured", result.stderr)

    def test_ambient_native_build_jobs_do_not_change_fixed_parallelism(self):
        for value in ("0", "invalid"):
            with self.subTest(value=value):
                result, _ = self._run(
                    name=f"invalid-jobs-{value}",
                    environment_updates={"H2_NATIVE_BUILD_JOBS": value},
                )
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_missing_sdk_and_compiler_fail_closed(self):
        cases = (
            ("BK_SDK_DIR", self.root / "missing-sdk", "SDK Makefile"),
            (
                "COMPILER_TOOLCHAIN_PATH",
                self.root / "missing-toolchain",
                "BK7258 compiler",
            ),
        )
        for variable, value, message in cases:
            with self.subTest(variable=variable):
                result, _ = self._run(
                    name=f"invalid-{variable.lower()}",
                    environment_updates={variable: str(value)},
                )
                self.assertIn(message, result.stderr)

    def test_sdk_commit_mismatch_fails_closed(self):
        result, _ = self._run(commit="0" * 40)
        self.assertIn("BK7258 SDK commit mismatch", result.stderr)

    def test_sdk_version_file_rejects_invalid_commit(self):
        result, _ = self._run(commit="not-a-commit")
        self.assertIn("invalid expected BK7258 SDK commit", result.stderr)

    def test_tracked_sdk_modifications_fail_closed(self):
        makefile = self.sdk / "Makefile"
        for staged in (False, True):
            with self.subTest(staged=staged):
                makefile.write_text("changed\n", encoding="utf-8")
                if staged:
                    subprocess.run(["git", "add", "Makefile"], cwd=self.sdk, check=True)
                result, _ = self._run(name=f"dirty-{staged}")
                self.assertIn("tracked modifications", result.stderr)
                subprocess.run(["git", "restore", "--staged", "Makefile"], cwd=self.sdk)
                subprocess.run(["git", "restore", "Makefile"], cwd=self.sdk, check=True)

    def test_untracked_sdk_file_is_not_treated_as_tracked_modification(self):
        (self.sdk / "local-only").write_text("untracked\n", encoding="utf-8")
        result, _ = self._run(name="untracked-sdk")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_native_build_sdk_mutation_fails_before_publishing(self):
        result, output = self._run(name="mutate-sdk", mode="mutate-sdk")
        self.assertIn("tracked modifications", result.stderr)
        self.assertFalse(output.joinpath("ap/firmware.elf").exists())

    def test_native_build_sdk_head_change_fails_before_publishing(self):
        result, output = self._run(name="advance-sdk-head", mode="advance-sdk-head")
        self.assertIn("BK7258 SDK commit mismatch", result.stderr)
        self.assertFalse(output.joinpath("ap/firmware.elf").exists())

    def test_native_build_source_mutation_fails_before_publishing(self):
        result, output = self._run(name="mutate-source", mode="mutate-source")
        self.assertIn("source checkout tracked state changed", result.stderr)
        self.assertFalse(output.joinpath("ap/firmware.elf").exists())

    def test_wrong_compiler_version_fails_closed(self):
        other = self.root / "wrong-toolchain"
        other.mkdir()
        compiler = other / "arm-none-eabi-gcc"
        compiler.write_text("#!/bin/sh\nprintf '9.2.0\\n'\n", encoding="utf-8")
        compiler.chmod(0o755)
        result, _ = self._run(
            environment_updates={"COMPILER_TOOLCHAIN_PATH": str(other)}
        )
        self.assertIn("compiler version mismatch", result.stderr)

    def test_ambient_python_path_does_not_select_the_build_python(self):
        wrong_python_bin = self.root / "wrong-python"
        wrong_python_bin.mkdir()
        fake_python = (
            f"#!{sys.executable}\n"
            "import os\n"
            "import sys\n"
            "if len(sys.argv) >= 3 and sys.argv[1] == '-c' and "
            "'distutils.dir_util' in sys.argv[2]:\n"
            "    print('3.12')\n"
            "    raise SystemExit(0)\n"
            f"os.execv({sys.executable!r}, [{sys.executable!r}, *sys.argv[1:]])\n"
        )
        for name in ("python", "python3", "python3.11"):
            executable = wrong_python_bin / name
            executable.write_text(fake_python, encoding="utf-8")
            executable.chmod(0o755)
        result, _ = self._run(
            environment_updates={
                "PATH": (
                    f"{wrong_python_bin}{os.pathsep}{self.tool_bin}"
                    f"{os.pathsep}{os.environ['PATH']}"
                )
            }
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_ambient_incompatible_python_does_not_select_the_build_python(self):
        incompatible_python_bin = self.root / "incompatible-python"
        incompatible_python_bin.mkdir()
        fake_python = (
            f"#!{sys.executable}\n"
            "import os\n"
            "import sys\n"
            "if len(sys.argv) >= 3 and sys.argv[1] == '-c' and "
            "'distutils.dir_util' in sys.argv[2]:\n"
            "    print('missing distutils', file=sys.stderr)\n"
            "    raise SystemExit(1)\n"
            f"os.execv({sys.executable!r}, [{sys.executable!r}, *sys.argv[1:]])\n"
        )
        for name in ("python", "python3", "python3.11"):
            executable = incompatible_python_bin / name
            executable.write_text(fake_python, encoding="utf-8")
            executable.chmod(0o755)
        result, _ = self._run(
            environment_updates={
                "PATH": (
                    f"{incompatible_python_bin}{os.pathsep}{self.tool_bin}"
                    f"{os.pathsep}{os.environ['PATH']}"
                )
            }
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_launcher_target_mismatch_fails_closed(self):
        cmake = self.project / "CMakeLists.txt"
        cmake.write_text('set(H2_BK_TARGET "bk3633")\n', encoding="utf-8")
        result, _ = self._run()
        self.assertIn("launcher target mismatch", result.stderr)

    def test_unsupported_target_fails_closed(self):
        result, _ = self._run(target="bk3633")
        self.assertIn("unsupported BK target: bk3633", result.stderr)

    def test_missing_support_file_fails_closed(self):
        (self.source / "partition.csv").unlink()
        result, _ = self._run(name="missing-support")
        self.assertIn("launcher support file", result.stderr)

    def test_make_failure_is_reported(self):
        result, _ = self._run(mode="make-failure")
        self.assertIn("BK7258 make failed with exit code 23", result.stderr)

    def test_missing_and_empty_outputs_fail_closed(self):
        modes = (
            "missing-ap-output",
            "missing-cp-output",
            "missing-package-output",
            "empty-ap-output",
            "empty-cp-output",
            "empty-package-output",
        )
        for mode in modes:
            with self.subTest(mode=mode):
                result, _ = self._run(name=mode, mode=mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("required BK7258 output", result.stderr)

    def test_invalid_partition_metadata_fails_closed(self):
        modes = (
            "invalid-json",
            "non-object-json",
            "missing-metadata",
            "escaping-metadata",
        )
        for mode in modes:
            with self.subTest(mode=mode):
                result, _ = self._run(name=mode, mode=mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("error:", result.stderr)


if __name__ == "__main__":
    unittest.main()
