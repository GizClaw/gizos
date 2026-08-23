from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
RUNNER = ROOT / "tools" / "bazel" / "esp_idf_runner.py"
SPEC = importlib.util.spec_from_file_location("esp_idf_runner", RUNNER)
assert SPEC and SPEC.loader
runner = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)


FAKE_IDF = r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import subprocess
import sys

if "H2_RUNNER_SECRET" in os.environ:
    raise SystemExit("unapproved environment variable escaped into idf.py")
if os.environ.get("ESP_IDF_VERSION") != "6.0":
    raise SystemExit("runner did not provide the pinned ESP-IDF version")
if os.environ.get("H2_FIRMWARE_VERSION") != "test-version":
    raise SystemExit("runner did not provide the internal firmware version")
if os.environ.get("H2_BAZEL_NATIVE_ARTIFACTS_ONLY") != "1":
    raise SystemExit("runner did not disable native package side effects")
args = sys.argv[1:]
definitions = [args[index + 1] for index, value in enumerate(args[:-1]) if value == "-D"]
if "PROJECT_VER=test-version" not in definitions:
    raise SystemExit("runner did not provide the ESP-IDF project version")
prebuilt = [value for value in definitions if value.startswith("H2_BAZEL_PREBUILT_H2_LIBCO=")]
if len(prebuilt) != 1:
    raise SystemExit("runner did not provide the Bazel-built h2_libco archive")
prebuilt_path = Path(prebuilt[0].split("=", 1)[1])
if not prebuilt_path.is_absolute() or prebuilt_path.read_bytes() != b"archive":
    raise SystemExit("runner provided an invalid h2_libco archive")
cmake_probe = [value for value in definitions if value.startswith("H2_TEST_CMAKE=")]
if cmake_probe and cmake_probe != ["H2_TEST_CMAKE=http://192.0.2.1:18080"]:
    raise SystemExit("runner provided an invalid allowlisted CMake value")
subprocess.run(["ninja", "build-probe"], check=True)
project = Path(args[args.index("-C") + 1])
build = Path(args[args.index("-B") + 1])
component_cache = os.environ.get("IDF_COMPONENT_CACHE_PATH")
if component_cache != str(project.parents[2] / "component-cache"):
    raise SystemExit("runner did not isolate the Component Manager cache")
if not project.parents[1].joinpath("partition.csv").is_file():
    raise SystemExit("declared launcher support file was not copied")
if project.joinpath("partition.csv").read_text(encoding="utf-8") != "partition\n":
    raise SystemExit("selected partition was not copied into the launcher project")
mode_path = project / "mode"
mode = mode_path.read_text(encoding="utf-8").strip() if mode_path.exists() else "success"
if mode == "wifi-environment":
    if os.environ.get("H2LOADER_WIFI_SSID") != "fixture-network":
        raise SystemExit("runner did not forward the Wi-Fi SSID")
    if os.environ.get("H2LOADER_WIFI_PASSWORD") != "fixture-password":
        raise SystemExit("runner did not forward the Wi-Fi password")
if mode == "wifi-environment-excluded":
    if "H2LOADER_WIFI_CREDENTIALS" in os.environ:
        raise SystemExit("runner leaked the combined Wi-Fi credentials")
    if "H2LOADER_WIFI_SSID" in os.environ:
        raise SystemExit("runner leaked the Wi-Fi SSID into an unrelated build")
    if "H2LOADER_WIFI_PASSWORD" in os.environ:
        raise SystemExit("runner leaked the Wi-Fi password into an unrelated build")
if mode == "ccache":
    if os.environ.get("IDF_CCACHE_ENABLE") != "1":
        raise SystemExit("runner did not enable ESP-IDF ccache")
    if os.environ.get("CCACHE_NAMESPACE") != "esp32s3":
        raise SystemExit("runner did not select the ESP32-S3 ccache namespace")
    if not Path(os.environ["CCACHE_DIR"]).is_dir():
        raise SystemExit("runner did not create the ESP compiler cache")
    if not Path(os.environ["PATH"].split(os.pathsep)[0], "ccache").is_symlink():
        raise SystemExit("runner did not expose ccache to ESP-IDF")
    remote = os.environ.get("CCACHE_REMOTE_STORAGE", "")
    if "https://storage.googleapis.com/cache/ccache/esp " not in remote:
        raise SystemExit("runner did not select the ESP remote ccache prefix")
    if "@bearer-token=fixture-token" not in remote:
        raise SystemExit("runner did not authenticate the ESP remote ccache")
    if os.environ.get("CCACHE_RESHARE") != "1":
        raise SystemExit("runner did not reshare local ESP ccache hits")
if mode == "ccache-read-only":
    if os.environ.get("CCACHE_READONLY") != "1":
        raise SystemExit("credential-bearing ESP build can write shared ccache")
if mode == "idf-failure":
    raise SystemExit(19)
if "merge-bin" in args:
    if mode == "merge-failure":
        raise SystemExit(23)
    output = Path(args[args.index("--output") + 1])
    if not output.is_absolute() or output != build / "combined_factory.bin":
        raise SystemExit("runner requested an invalid combined image path")
    output.write_bytes(b"combined")
    raise SystemExit(0)
project.joinpath("generated.lock").write_text("temporary\n", encoding="utf-8")
build.mkdir(parents=True)
files = {
    "fixture.elf": b"elf",
    "fixture.map": b"map",
    "fixture.bin": b"app",
    "bootloader/bootloader.bin": b"bootloader",
    "partition_table/partition-table.bin": b"partition",
    "ota_data_initial.bin": b"ota",
    "update.tar.zlib": b"not-public",
}
if mode == "missing-output":
    files.pop("fixture.map")
if mode == "empty-output":
    files["fixture.bin"] = b""
for relative, content in files.items():
    output = build / relative
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(content)
metadata = {
    "flash_files": {
        "0x0": "bootloader/bootloader.bin",
        "0x8000": "partition_table/partition-table.bin",
        "0xd000": "ota_data_initial.bin",
        "0x10000": "fixture.bin",
    },
    "write_flash_args": ["--flash_mode", "dio"],
}
if mode == "invalid-json":
    build.joinpath("flasher_args.json").write_text("{", encoding="utf-8")
else:
    if mode == "empty-flash-map":
        metadata["flash_files"] = {}
    elif mode == "invalid-offset":
        metadata["flash_files"]["nope"] = metadata["flash_files"].pop("0xd000")
    elif mode == "duplicate-offset":
        metadata["flash_files"]["65536"] = "ota_data_initial.bin"
    elif mode == "escaping-path":
        metadata["flash_files"]["0xd000"] = "../outside.bin"
    elif mode == "absolute-path":
        metadata["flash_files"]["0xd000"] = str(build / "ota_data_initial.bin")
    elif mode == "missing-flash-file":
        metadata["flash_files"]["0xd000"] = "missing.bin"
    elif mode == "missing-required-reference":
        metadata["flash_files"].pop("0x10000")
    build.joinpath("flasher_args.json").write_text(json.dumps(metadata), encoding="utf-8")
'''

FAKE_IDF_TOOLS = r'''#!/usr/bin/env python3
import sys

if sys.argv[-1] not in ("check", "check-python-dependencies"):
    raise SystemExit("unexpected idf_tools.py operation")
'''


class EspIdfRunnerTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

        self.source = self.root / "repo"
        self.project = self.source / "projects" / "fixture"
        self.project.mkdir(parents=True)
        (self.project / "CMakeLists.txt").write_text(
            'set(H2_ESP_TARGET "esp32s3")\n', encoding="utf-8"
        )
        (self.source / "partition.csv").write_text("partition\n", encoding="utf-8")

        self.idf = self.root / "esp-idf"
        idf_py = self.idf / "tools" / "idf.py"
        idf_py.parent.mkdir(parents=True)
        idf_py.write_text(FAKE_IDF, encoding="utf-8")
        idf_py.chmod(0o755)
        idf_tools = self.idf / "tools" / "idf_tools.py"
        idf_tools.write_text(FAKE_IDF_TOOLS, encoding="utf-8")
        idf_tools.chmod(0o755)
        subprocess.run(["git", "init", "-q"], cwd=self.idf, check=True)
        subprocess.run(["git", "add", "."], cwd=self.idf, check=True)
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
            cwd=self.idf,
            check=True,
        )
        self.commit = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=self.idf, text=True
        ).strip()

        self.python_environment = self.root / "python-env"
        python = self.python_environment / "bin" / "python"
        python.parent.mkdir(parents=True)
        python.symlink_to(sys.executable)
        self.tools = self.root / "idf-tools"
        self.tools.mkdir()
        self.tool_bin = self.root / "bin"
        self.tool_bin.mkdir()
        compiler_versions = {
            "xtensa-esp-elf-gcc": (
                "xtensa-esp-elf-gcc (crosstool-NG esp-15.2.0_20251204) 15.2.0"
            ),
            "riscv32-esp-elf-gcc": (
                "riscv32-esp-elf-gcc (crosstool-NG esp-15.2.0_20251204) 15.2.0"
            ),
        }
        for compiler, version in compiler_versions.items():
            path = self.tool_bin / compiler
            path.write_text(f"#!/bin/sh\nprintf '%s\\n' '{version}'\n", encoding="utf-8")
            path.chmod(0o755)
        for tool in ("git", "python3"):
            tool_path = shutil.which(tool)
            self.assertIsNotNone(tool_path)
            self.tool_bin.joinpath(tool).symlink_to(tool_path)
        ninja = self.tool_bin / "ninja"
        ninja.write_text(
            "#!/bin/sh\n"
            "if test \"$1\" = --version; then echo 1.13.2; exit 0; fi\n"
            "test \"$1\" = -j && test \"$2\" = \"$H2_NATIVE_BUILD_JOBS\" && "
            "test \"$3\" = build-probe\n",
            encoding="utf-8",
        )
        ninja.chmod(0o755)
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
        target: str = "esp32s3",
        commit: str | None = None,
        environment_updates: dict[str, str | None] | None = None,
        h2loader_wifi_environment: bool = False,
        precreate_flash_output: bool = False,
        prebuilt_component: str = "h2_libco=bazel-out/libfixture.a",
        tool_versions: str | None = None,
        cmake_variable: str | None = None,
    ) -> tuple[subprocess.CompletedProcess[str], Path]:
        mode_path = self.project / "mode"
        if mode == "success":
            mode_path.unlink(missing_ok=True)
        else:
            mode_path.write_text(mode, encoding="utf-8")
        output = self.root / name
        version_file = self.root / f"{name}-idf-version.txt"
        version_file.write_text(commit or self.commit, encoding="utf-8")
        tool_versions_file = self.root / f"{name}-idf-tool-versions.txt"
        tool_versions_file.write_text(
            tool_versions
            or (
                "esp32p4=riscv32-esp-elf-gcc (crosstool-NG "
                "esp-15.2.0_20251204) 15.2.0\n"
                "esp32s3=xtensa-esp-elf-gcc (crosstool-NG "
                "esp-15.2.0_20251204) 15.2.0\n"
                "esp32s3_archive_compiler=xtensa-esp-elf-gcc\n"
                "esp32s3_dynconfig=xtensa_esp32s3.so\n"
                "ninja=1.13.2\n"
                "python=esp-idf-v6.0-constraints\n"
            ),
            encoding="utf-8",
        )
        environment_updates = environment_updates or {}
        sdk_locator = self.root / f"{name}-idf-sdk-locator.json"
        sdk_enabled = environment_updates.get("IDF_PATH", str(self.idf)) is not None
        sdk_root = environment_updates.get("IDF_PATH", str(self.idf))
        sdk_locator.write_text(json.dumps({
            "schema": "h2.native-locator.v1",
            "kind": "esp-idf-sdk",
            "enabled": sdk_enabled,
            "paths": {"root": str(sdk_root)} if sdk_enabled else {},
            "metadata": {},
        }), encoding="utf-8")
        tools_locator = self.root / f"{name}-idf-tools-locator.json"
        tools_enabled = all(
            environment_updates.get(variable, "configured") is not None
            for variable in ("IDF_PYTHON_ENV_PATH", "IDF_TOOLS_PATH", "PATH")
        )
        tools_root = self.root if tools_enabled else self.root / "missing-tools"
        tools_locator.write_text(json.dumps({
            "schema": "h2.native-locator.v1",
            "kind": "esp-idf-tools",
            "enabled": tools_enabled,
            "paths": {
                "python_root": str(environment_updates.get("IDF_PYTHON_ENV_PATH", self.python_environment)),
                "tools_root": str(tools_root),
            } if tools_enabled else {},
            "metadata": {},
        }), encoding="utf-8")
        ccache_root = self.root / f"{name}-ccache-runtime"
        ccache_root.mkdir(exist_ok=True)
        ccache_locator = ccache_root / "locator.json"
        ccache_enabled = "H2_NATIVE_CCACHE" in environment_updates
        if ccache_enabled:
            (ccache_root / "bin").mkdir(exist_ok=True)
            (ccache_root / "cache").mkdir(exist_ok=True)
            shutil.copy2(self.ccache, ccache_root / "bin/ccache")
            shutil.copy2(
                self.ccache_helper,
                ccache_root / "bin/ccache-storage-https",
            )
            (ccache_root / "token").write_text(self.ccache_token.read_text(), encoding="utf-8")
            (ccache_root / "token").chmod(0o600)
            (ccache_root / "runtime.json").write_text(json.dumps({
                "schema": "h2.native-ccache-runtime.v1",
                "ccache": "bin/ccache",
                "cache_root": "cache",
                "remote_base_url": "https://storage.googleapis.com/cache/ccache",
                "storage_helper": "bin/ccache-storage-https",
                "token_file": "token",
            }), encoding="utf-8")
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
        if precreate_flash_output:
            (output / "flash-files").mkdir(parents=True)
        prebuilt_archive = self.source / "bazel-out" / "libfixture.a"
        prebuilt_archive.parent.mkdir(parents=True, exist_ok=True)
        prebuilt_archive.write_bytes(b"archive")
        environment = dict(os.environ)
        environment.update(
            {
                "IDF_PATH": str(self.idf),
                "IDF_PYTHON_ENV_PATH": str(self.python_environment),
                "IDF_TOOLS_PATH": str(self.tools),
                "PATH": f"{self.tool_bin}{os.pathsep}{environment['PATH']}",
                "H2_RUNNER_SECRET": "must-not-propagate",
            }
        )
        for key, value in environment_updates.items():
            if value is None:
                environment.pop(key, None)
            else:
                environment[key] = value
        command = [
            sys.executable,
            str(RUNNER),
            "--source-root",
            str(self.source),
            "--project",
            "projects/fixture/CMakeLists.txt",
            "--partition",
            "partition.csv",
            "--project-name",
            "fixture",
            "--target",
            target,
            "--board",
            "fixture-board",
            "--version",
            "test-version",
            "--idf-version-file",
            str(version_file),
            "--idf-tool-versions-file",
            str(tool_versions_file),
            "--idf-sdk-locator",
            str(sdk_locator),
            "--idf-tools-locator",
            str(tools_locator),
            "--ccache-runtime-locator",
            str(ccache_locator),
            "--support-file",
            "partition.csv",
            "--prebuilt-component",
            prebuilt_component,
            "--elf-output",
            str(output / "firmware.elf"),
            "--map-output",
            str(output / "firmware.map"),
            "--app-output",
            str(output / "app.bin"),
            "--bootloader-output",
            str(output / "bootloader.bin"),
            "--combined-factory-output",
            str(output / "combined_factory.bin"),
            "--partition-table-output",
            str(output / "partition-table.bin"),
            "--flash-files-output",
            str(output / "flash-files"),
            "--flash-metadata-output",
            str(output / "flasher_args.json"),
        ]
        if h2loader_wifi_environment:
            command.append("--h2loader-wifi-environment")
        if cmake_variable is not None:
            command.extend(["--cmake-variable", cmake_variable])
        return (
            subprocess.run(command, env=environment, text=True, capture_output=True),
            output,
        )

    def test_success_publishes_only_validated_native_outputs(self):
        result, output = self._run()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output.joinpath("firmware.elf").read_bytes(), b"elf")
        self.assertEqual(output.joinpath("app.bin").read_bytes(), b"app")
        self.assertEqual(
            output.joinpath("combined_factory.bin").read_bytes(),
            b"combined",
        )
        self.assertTrue(output.joinpath("flash-files/ota_data_initial.bin").is_file())
        self.assertFalse(output.joinpath("update.tar.zlib").exists())
        metadata = json.loads(output.joinpath("flasher_args.json").read_text())
        self.assertEqual(metadata["flash_files"]["0x10000"], "fixture.bin")
        self.assertFalse(self.project.joinpath("generated.lock").exists())

    def test_allowlisted_cmake_variable_reaches_idf(self):
        result, _ = self._run(
            name="cmake-variable",
            cmake_variable="H2_TEST_CMAKE=http://192.0.2.1:18080",
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_ccache_environment_reaches_idf(self):
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

    def test_credential_bearing_build_uses_ccache_read_only(self):
        result, _ = self._run(
            name="ccache-read-only",
            mode="ccache-read-only",
            environment_updates={
                "H2_NATIVE_CCACHE": str(self.ccache),
                "H2_NATIVE_CCACHE_ROOT": str(self.root / "native-cache"),
                "H2_NATIVE_CCACHE_REMOTE_BASE_URL": (
                    "https://storage.googleapis.com/cache/ccache"
                ),
                "H2_NATIVE_CCACHE_STORAGE_HELPER": str(self.ccache_helper),
                "H2_NATIVE_CCACHE_TOKEN_FILE": str(self.ccache_token),
                "H2LOADER_WIFI_CREDENTIALS": json.dumps(
                    {"ssid": "fixture-network", "password": "fixture-password"}
                ),
            },
            h2loader_wifi_environment=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_invalid_cmake_variable_fails_closed(self):
        result, _ = self._run(
            name="invalid-cmake-variable",
            cmake_variable="bad-name=value",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid CMake variable", result.stderr)

    def test_native_component_manifest_is_stable_and_complete(self):
        components = {
            "zeta": Path("/components/zeta"),
            "alpha": Path("/components/alpha"),
        }
        sources = {
            "zeta": [Path("/repo/zeta_b.c"), Path("/repo/zeta_a.c")],
            "alpha": [Path("/repo/alpha.c")],
        }
        manifest = runner.render_native_component_manifest(
            "fixture-board", components, sources
        )
        self.assertLess(manifest.index("/components/alpha"), manifest.index("/components/zeta"))
        self.assertLess(manifest.index("/repo/zeta_a.c"), manifest.index("/repo/zeta_b.c"))
        self.assertIn('set(H2_BAZEL_BOARD "fixture-board")', manifest)

    def test_native_component_manifest_rejects_undeclared_source_owner(self):
        with self.assertRaisesRegex(
            runner.RunnerError, "sources have no declared component: missing"
        ):
            runner.render_native_component_manifest(
                "fixture-board",
                {"main": Path("/components/main")},
                {"missing": [Path("/repo/missing.c")]},
            )

    def test_native_component_sources_reject_duplicate_owners(self):
        source = self.source / "shared.c"
        source.write_text("int shared;\n", encoding="utf-8")
        with self.assertRaisesRegex(runner.RunnerError, "multiple owners"):
            runner.resolve_native_component_sources(
                self.source,
                ["first=shared.c", "second=shared.c"],
            )

    def test_success_accepts_bazel_precreated_tree_artifact(self):
        result, output = self._run(
            name="precreated-tree",
            precreate_flash_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(output.joinpath("flash-files/fixture.bin").is_file())

    def test_two_invocations_use_isolated_work_directories(self):
        with ThreadPoolExecutor(max_workers=2) as executor:
            futures = [executor.submit(self._run, f"result-{index}") for index in range(2)]
        for future in futures:
            result, output = future.result()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(output.joinpath("app.bin").is_file())

    def test_missing_locator_fails_closed(self):
        for name in ("IDF_PATH", "IDF_PYTHON_ENV_PATH", "IDF_TOOLS_PATH"):
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

    def test_prebuilt_component_path_fails_closed(self):
        result, _ = self._run(
            name="invalid-prebuilt",
            prebuilt_component="h2_libco=../libfixture.a",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid ESP-IDF prebuilt archive path", result.stderr)

    def test_custom_native_build_jobs_is_ignored(self):
        result, _ = self._run(
            name="custom-jobs",
            environment_updates={"H2_NATIVE_BUILD_JOBS": "7"},
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_optional_wifi_environment_reaches_idf(self):
        result, _ = self._run(
            name="wifi-environment",
            mode="wifi-environment",
            environment_updates={
                "H2LOADER_WIFI_CREDENTIALS": json.dumps(
                    {"ssid": "fixture-network", "password": "fixture-password"}
                ),
            },
            h2loader_wifi_environment=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_wifi_environment_is_excluded_without_opt_in(self):
        result, _ = self._run(
            name="wifi-environment-excluded",
            mode="wifi-environment-excluded",
            environment_updates={
                "H2LOADER_WIFI_CREDENTIALS": json.dumps(
                    {"ssid": "fixture-network", "password": "fixture-password"}
                ),
            },
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_invalid_wifi_credentials_fail_closed(self):
        cases = (
            ("not-json", "must be valid JSON"),
            (json.dumps({"ssid": "fixture-network"}), "exactly ssid and password"),
            (
                json.dumps({"ssid": "fixture-network", "password": ""}),
                ".password must be a non-empty string",
            ),
        )
        for value, message in cases:
            with self.subTest(value=value):
                result, _ = self._run(
                    name="invalid-wifi-credentials",
                    environment_updates={"H2LOADER_WIFI_CREDENTIALS": value},
                    h2loader_wifi_environment=True,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)

    def test_missing_wifi_credentials_fail_closed(self):
        result, _ = self._run(
            environment_updates={"H2LOADER_WIFI_CREDENTIALS": None},
            h2loader_wifi_environment=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "required action environment is missing: H2LOADER_WIFI_CREDENTIALS",
            result.stderr,
        )

    def test_missing_idf_and_python_executables_fail_closed(self):
        cases = (
            ("IDF_PATH", self.root / "missing-idf", "idf.py"),
            (
                "IDF_PYTHON_ENV_PATH",
                self.root / "missing-python",
                "IDF Python",
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
        self.assertIn("ESP-IDF commit mismatch", result.stderr)

    def test_sdk_version_file_rejects_invalid_commit(self):
        result, _ = self._run(commit="not-a-commit")
        self.assertIn("invalid expected ESP-IDF commit", result.stderr)

    def test_tool_versions_file_rejects_malformed_entry(self):
        result, _ = self._run(tool_versions="not-a-version-entry\n")
        self.assertIn("invalid ESP-IDF tool version entry", result.stderr)

    def test_compiler_version_mismatch_fails_closed(self):
        result, _ = self._run(
            tool_versions=(
                "esp32p4=riscv32-esp-elf-gcc (crosstool-NG "
                "esp-15.2.0_20251204) 15.2.0\n"
                "esp32s3=unexpected\n"
                "ninja=1.13.2\n"
                "python=esp-idf-v6.0-constraints\n"
            ),
        )
        self.assertIn("compiler version mismatch", result.stderr)

    def test_compiler_missing_fails_closed(self):
        compiler = self.tool_bin / "xtensa-esp-elf-gcc"
        compiler.unlink()
        result, _ = self._run()
        self.assertIn("expected exactly one xtensa-esp-elf-gcc", result.stderr)

    def test_ninja_missing_fails_closed(self):
        self.tool_bin.joinpath("ninja").unlink()
        result, _ = self._run(tool_versions=(
            "esp32p4=riscv32-esp-elf-gcc (crosstool-NG esp-15.2.0_20251204) 15.2.0\n"
            "esp32s3=xtensa-esp-elf-gcc (crosstool-NG esp-15.2.0_20251204) 15.2.0\n"
            "ninja=definitely-not-the-system-version\n"
            "python=esp-idf-v6.0-constraints\n"
        ))
        self.assertIn("Ninja version mismatch", result.stderr)

    def test_launcher_target_mismatch_fails_closed(self):
        result, _ = self._run(target="esp32p4")
        self.assertIn("launcher target mismatch", result.stderr)

    def test_idf_failure_is_reported(self):
        result, _ = self._run(mode="idf-failure")
        self.assertIn("idf.py build failed with exit code 19", result.stderr)

    def test_merge_bin_failure_is_reported(self):
        result, _ = self._run(mode="merge-failure")
        self.assertIn("idf.py merge-bin failed with exit code 23", result.stderr)

    def test_missing_and_empty_outputs_fail_closed(self):
        for mode in ("missing-output", "empty-output"):
            with self.subTest(mode=mode):
                result, _ = self._run(name=mode, mode=mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("required ESP-IDF output", result.stderr)

    def test_invalid_flash_metadata_fails_closed(self):
        modes = (
            "invalid-json",
            "empty-flash-map",
            "invalid-offset",
            "duplicate-offset",
            "escaping-path",
            "absolute-path",
            "missing-flash-file",
            "missing-required-reference",
        )
        for mode in modes:
            with self.subTest(mode=mode):
                result, _ = self._run(name=mode, mode=mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("error:", result.stderr)


if __name__ == "__main__":
    unittest.main()
