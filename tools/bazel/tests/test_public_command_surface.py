from __future__ import annotations

import os
import pathlib
import re
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
SHELL_FENCE = re.compile(r"^```(?:ba)?sh(?:ell)?\s*$")
REPOSITORY_COMMAND = re.compile(
    r"(?:^|[;&|]\s*)(?:[A-Za-z_][A-Za-z0-9_]*=[^ ]+\s+)*"
    r"(?:bazel(?:isk)?\s+(?:build|test|run|query|cquery|coverage)|"
    r"npm(?:\s+--prefix\s+\S+)?\s+(?:ci|run)|"
    r"(?:emcmake\s+)?cmake\s+(?:-S|--build)|"
    r"idf\.py\s+(?:build|flash)|"
    r"python3?\s+(?:-m\s+unittest|tools/)|"
    r"go\s+test)"
)
PUBLIC_START = re.compile(
    r"^(?:[A-Za-z_][A-Za-z0-9_]*=[^ ]+\s+)*(?:make\s|bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader --\s)"
)
LOCAL_BAZEL_START = re.compile(
    r"^(?:[A-Za-z_][A-Za-z0-9_]*=[^ ]+\s+)*bazel(?:isk)?\s"
)
ALLOWED_WORKFLOW_SETUP = (
    "python -m venv",
    "python -m pip install",
)
REMOVED = (
    "native-ci-test",
    "esp-ci-test",
    "bk7258-ci-test",
    "bk3633-ci-test",
    "run_h2_gizclaw_pal_e2e.sh",
    "gizclaw-pal-e2e.yml",
    "pion-cgo",
    "bazel-ci-plan",
    "bazel-ci-exec",
    "bazel-test-e2e-gizclaw",
    "bazel-test-e2e-h106",
    "bazel-test-e2e-pal",
    "bazel-test-e2e.sh",
    "bazel-test-smoke",
)

MAKE_TARGET_SCRIPTS = {
    "help": "config/help.sh",
    "cfg-doctor": "config/cfg-doctor.sh",
    "cfg-submodules": "config/cfg-submodules.sh",
    "bazel-build": "bazel/bazel-build.py",
    "bazel-test": "bazel/bazel-test.py",
    "bazel-test-mqtt_public_broker_smoke": "bazel/bazel-test-mqtt_public_broker_smoke.sh",
    "bazel-test-gizclaw_h2peer_live_test": "bazel/bazel-test-gizclaw_h2peer_live_test.sh",
    "bazel-test-gizclaw_pion_live_test": "bazel/bazel-test-gizclaw_pion_live_test.sh",
    "bazel-coverage-report": "bazel/bazel-coverage-report.py",
    "bazel-release": "bazel/bazel-release.py",
    "h2loader-bin": "h2loader/h2loader-bin.sh",
    "test-web": "test/test-web.sh",
    "guides-build": "guides/guides-build.sh",
    "guides-watch": "guides/guides-watch.sh",
    "guides-preview": "guides/guides-preview.sh",
}

COMMON_MAKE_SCRIPTS = {
    "common/bazel.py",
    "common/bazel_manual_test.py",
    "config/h2loader-operation-env.sh",
}

REMOVED_H2LOADER_ENTRIES = (
    "tools/bin/h2loader",
    "tools/h2loader",
)

PUBLIC_TARGET_PREFIXES = {
    "config": "cfg",
    "bazel": "bazel",
    "h2loader": "h2loader",
    "test": "test",
    "guides": "guides",
}


def markdown_shell_commands(path: pathlib.Path) -> list[tuple[int, str]]:
    commands: list[tuple[int, str]] = []
    in_shell = False
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = raw.strip()
        if not in_shell:
            in_shell = bool(SHELL_FENCE.fullmatch(stripped))
            continue
        if stripped == "```":
            in_shell = False
            continue
        if not stripped or stripped.startswith("#") or stripped.startswith(">"):
            continue
        commands.append((number, stripped))
    return commands


def workflow_run_blocks(path: pathlib.Path) -> list[tuple[int, str]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    blocks: list[tuple[int, str]] = []
    index = 0
    while index < len(lines):
        match = re.match(r"^(\s*)run:\s*(?:\||>)?\s*$", lines[index])
        if not match:
            index += 1
            continue
        indent = len(match.group(1))
        start = index + 1
        body: list[str] = []
        index += 1
        while index < len(lines):
            current = lines[index]
            if current.strip() and len(current) - len(current.lstrip()) <= indent:
                break
            body.append(current.strip())
            index += 1
        blocks.append((start, "\n".join(body)))
    return blocks


class PublicCommandSurfaceTest(unittest.TestCase):
    def run_make(
        self,
        target: str,
        *variables: str,
        bazel_exit: int = 0,
        bazel_output: str = "",
        environment_updates: dict[str, str] | None = None,
    ) -> tuple[subprocess.CompletedProcess[str], list[str]]:
        with tempfile.TemporaryDirectory() as directory:
            temporary = pathlib.Path(directory)
            log = temporary / "bazel.log"
            fake_bazel = temporary / "bazel"
            fake_bazel.write_text(
                "#!/bin/sh\n"
                'printf \'%s\\n\' "$*" >> "$H2_TEST_BAZEL_LOG"\n'
                'printf \'%s\' "$H2_TEST_BAZEL_OUTPUT"\n'
                'exit "$H2_TEST_BAZEL_EXIT"\n',
                encoding="utf-8",
            )
            fake_bazel.chmod(0o755)
            environment = dict(os.environ)
            environment.update({
                "H2_TEST_BAZEL_LOG": str(log),
                "H2_TEST_BAZEL_EXIT": str(bazel_exit),
                "H2_TEST_BAZEL_OUTPUT": bazel_output,
            })
            environment.update(environment_updates or {})
            environment.pop("BAZEL_REMOTE_CACHE_URL", None)
            environment.pop("BAZEL_REMOTE_CACHE_MODE", None)
            environment.pop("BAZEL_DISK_CACHE_MODE", None)
            result = subprocess.run(
                [
                    "make",
                    "--silent",
                    target,
                    f"BAZEL_BIN={fake_bazel}",
                    *variables,
                ],
                cwd=ROOT,
                env=environment,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            commands = (
                log.read_text(encoding="utf-8").splitlines()
                if log.exists()
                else []
            )
            return result, commands

    def test_bazel_build_keeps_wifi_credentials_out_of_argv(self) -> None:
        credentials = '{"ssid":"fixture","password":"fixture"}'
        result, commands = self.run_make(
            "bazel-build",
            "BAZEL_CONFIG=esp32s3",
            environment_updates={"H2LOADER_WIFI_CREDENTIALS": credentials},
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(credentials, result.stdout + result.stderr)
        self.assertEqual(len(commands), 1)
        self.assertNotIn(credentials, commands[0])
        arguments = commands[0].split()
        self.assertTrue(arguments[0].startswith("--bazelrc="), arguments)
        self.assertEqual(
            arguments[1:],
            ["build", "--noannounce_rc", "--config=esp32s3", "//..."],
        )
        self.assertFalse(pathlib.Path(arguments[0].partition("=")[2]).exists())

    def test_workflow_repository_commands_start_at_make(self) -> None:
        findings = []
        for path in sorted((ROOT / ".github/workflows").glob("*.yml")):
            for line, block in workflow_run_blocks(path):
                for offset, command in enumerate(block.splitlines()):
                    if not REPOSITORY_COMMAND.search(command):
                        continue
                    if any(item in command for item in ALLOWED_WORKFLOW_SETUP):
                        continue
                    if PUBLIC_START.match(command):
                        continue
                    findings.append(
                        f"{path.relative_to(ROOT)}:{line + offset}: {command}"
                    )
        self.assertEqual(findings, [])

    def test_ci_cache_contract_is_uniform(self) -> None:
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(
            encoding="utf-8"
        )
        cache_action = (
            "actions/cache/{kind}@"
            "caa296126883cff596d87d8935842f9db880ef25 # v5.1.0"
        )
        self.assertNotIn(
            "actions/cache/restore@0057852bfaa89a56745cba8c7296529d2fc39830",
            workflow,
        )
        self.assertNotIn(
            "actions/cache/save@0057852bfaa89a56745cba8c7296529d2fc39830",
            workflow,
        )
        self.assertEqual(workflow.count(cache_action.format(kind="restore")), 9)
        self.assertEqual(workflow.count(cache_action.format(kind="save")), 9)
        self.assertEqual(workflow.count("Save Bazel repository cache seed"), 6)
        self.assertEqual(
            workflow.count(
                "key: ${{ steps.bazel-repository-cache.outputs.cache-primary-key }}"
            ),
            6,
        )
        self.assertEqual(
            workflow.count(
                "Native ccache GCS requires the Bazel cache WIF variables."
            ),
            5,
        )
        self.assertEqual(
            workflow.count(
                "Native ccache and Bazel remote cache must use isolated prefixes "
                "in the same bucket."
            ),
            5,
        )
        self.assertEqual(workflow.count("Refresh native compiler cache token"), 3)
        self.assertEqual(
            workflow.count("Configure native compiler cache GCS access"), 3
        )
        self.assertEqual(
            workflow.count("Show native compiler cache statistics"), 3
        )
        for config in ("esp32s3", "esp32p4", "esp32c5", "bk7258", "bk3633"):
            self.assertIn(config, workflow)
        self.assertIn(
            "github.event_name == 'push' && github.ref == 'refs/heads/main'",
            workflow,
        )

    def test_release_workflow_is_public_and_closed(self) -> None:
        workflow = (ROOT / ".github/workflows/release.yml").read_text(
            encoding="utf-8"
        )
        for job in (
            "catalog:",
            "esp:",
            "bk7258:",
            "firmware-bundle:",
            "release-bundle:",
        ):
            self.assertIn(job, workflow)
        self.assertIn("tags: [\"v*\"]", workflow)
        self.assertIn("workflow_dispatch:", workflow)
        self.assertIn("if: github.event_name == 'push'", workflow)
        self.assertIn("--draft=false", workflow)
        self.assertIn("gh release download", workflow)
        self.assertIn("actions/create-github-app-token@", workflow)
        self.assertNotIn("BK_SDK_DEPLOY_KEY", workflow)
        self.assertNotIn("ssh-key:", workflow)
        for private_product in (
            "projects/h106",
            "boards/h200",
            "boards/tiga",
            "boards/zero_",
            "tapdoki",
            "lucky_kitty",
        ):
            self.assertNotIn(private_product, workflow.lower())

    def test_markdown_repository_commands_use_public_entry(self) -> None:
        findings = []
        for path in sorted(ROOT.rglob("*.md")):
            relative = path.relative_to(ROOT)
            if relative.parts[0] in {"build", "third_party", "x"}:
                continue
            for line, command in markdown_shell_commands(path):
                if (
                    REPOSITORY_COMMAND.search(command)
                    and not PUBLIC_START.match(command)
                    and not LOCAL_BAZEL_START.match(command)
                ):
                    findings.append(f"{relative}:{line}: {command}")
        self.assertEqual(findings, [])

    def test_removed_public_commands_and_paths_have_no_callers(self) -> None:
        findings = []
        paths = [ROOT / "Makefile", ROOT / "README.md"]
        paths.extend(sorted((ROOT / "scripts").rglob("*.py")))
        paths.extend(sorted((ROOT / "scripts").rglob("*.sh")))
        paths.extend(sorted((ROOT / ".github/workflows").glob("*.yml")))
        for owner in ("guides", "projects", "tools"):
            paths.extend(sorted((ROOT / owner).rglob("*.md")))
        for path in paths:
            text = path.read_text(encoding="utf-8")
            for removed in REMOVED:
                if removed in text:
                    findings.append(f"{path.relative_to(ROOT)}: {removed}")
        self.assertEqual(findings, [])

    def test_removed_h2loader_entries_have_no_callers(self) -> None:
        findings = []
        paths = [ROOT / "AGENTS.md", ROOT / "Makefile"]
        paths.extend(sorted((ROOT / ".env").rglob("*")))
        paths.extend(sorted((ROOT / "scripts").rglob("*.sh")))
        paths.extend(sorted((ROOT / "guides").rglob("*.md")))
        paths.extend(sorted((ROOT / "projects").rglob("*.md")))
        paths = [path for path in paths if path.is_file()]
        for path in paths:
            text = path.read_text(encoding="utf-8")
            for removed in REMOVED_H2LOADER_ENTRIES:
                if removed in text:
                    findings.append(f"{path.relative_to(ROOT)}: {removed}")
        self.assertEqual(findings, [])

    def test_native_h2loader_entry_is_documented(self) -> None:
        entry = (
            "bazel run --config=<host> "
            "//projects/h2loader/targets/cc_binary/cli:h2loader --"
        )
        guide = (ROOT / "guides/zh/using/h2loader/cli.md").read_text(
            encoding="utf-8"
        )
        self.assertIn(entry, guide)

    def test_public_make_targets_dispatch_one_same_named_script(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        expected_targets = set(MAKE_TARGET_SCRIPTS)
        root_targets = set(
            re.findall(r"(?m)^([a-z][a-z0-9_-]*):$", makefile)
        )
        phony = re.search(r"(?m)^\.PHONY: (.+)$", makefile)
        self.assertIsNotNone(phony)
        assert phony
        self.assertEqual(root_targets, expected_targets)
        self.assertEqual(set(phony.group(1).split()), expected_targets)
        actual_scripts = {
            path.relative_to(ROOT / "scripts").as_posix()
            for path in (ROOT / "scripts").rglob("*")
            if path.suffix in {".py", ".sh"}
        }
        self.assertEqual(
            actual_scripts,
            set(MAKE_TARGET_SCRIPTS.values()) | COMMON_MAKE_SCRIPTS,
        )
        self.assertEqual(
            {
                path
                for path in actual_scripts
                if path not in COMMON_MAKE_SCRIPTS
            },
            set(MAKE_TARGET_SCRIPTS.values()),
        )
        public_directories = {
            pathlib.PurePosixPath(relative).parts[0]
            for relative in MAKE_TARGET_SCRIPTS.values()
        }
        self.assertEqual(public_directories, set(PUBLIC_TARGET_PREFIXES))
        self.assertEqual(list((ROOT / "scripts").rglob("*.mk")), [])
        for target, relative in MAKE_TARGET_SCRIPTS.items():
            script = ROOT / "scripts" / relative
            with self.subTest(target=target):
                directory = pathlib.PurePosixPath(relative).parts[0]
                prefix = PUBLIC_TARGET_PREFIXES[directory]
                if target != "help":
                    self.assertTrue(
                        target == prefix or target.startswith(f"{prefix}-")
                    )
                self.assertTrue(script.is_file())
                self.assertEqual(script.stem, target)
                self.assertRegex(
                    makefile,
                    rf"(?m)^{re.escape(target)}:\n\t"
                    rf"@scripts/{re.escape(relative)}(?: .*)?$",
                )
                self.assertTrue(script.stat().st_mode & 0o100)
        for relative in COMMON_MAKE_SCRIPTS:
            with self.subTest(common=relative):
                self.assertTrue((ROOT / "scripts" / relative).is_file())

    def test_help_lists_every_public_make_target_once(self) -> None:
        result, _ = self.run_make("help")
        self.assertEqual(result.returncode, 0, result.stderr)
        listed_targets = re.findall(
            r"(?m)^  ([a-z][a-z0-9_-]*)(?:\s|$)", result.stdout
        )
        self.assertEqual(set(listed_targets), set(MAKE_TARGET_SCRIPTS))
        self.assertEqual(len(listed_targets), len(set(listed_targets)))

    def test_manual_tests_have_one_to_one_make_entries(self) -> None:
        cases = {
            "bazel-test-mqtt_public_broker_smoke": (
                "",
                "//projects/e2e/targets/cc_binary/pal:mqtt_public_broker_smoke",
            ),
            "bazel-test-gizclaw_h2peer_live_test": (
                "",
                "//projects/e2e/targets/cc_test/gizclaw:gizclaw_h2peer_live_test",
            ),
            "bazel-test-gizclaw_pion_live_test": (
                "",
                "//projects/e2e/targets/cc_test/gizclaw:gizclaw_pion_live_test",
            ),
        }
        for target, (extra, label) in cases.items():
            with self.subTest(target=target):
                result, commands = self.run_make(target)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(
                    commands,
                    [
                        "test --cache_test_results=no --test_output=streamed "
                        f"{extra}{label}"
                    ],
                )

    def test_bazel_remote_cache_read_mode(self) -> None:
        result, commands = self.run_make(
            "bazel-build",
            "BAZEL_CONFIG=linux_x86_64",
            "BAZEL_REMOTE_CACHE_URL=https://storage.googleapis.com/cache/gizos",
            "BAZEL_REMOTE_CACHE_MODE=read",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            commands,
            [
                "build --config=linux_x86_64 "
                "--remote_cache=https://storage.googleapis.com/cache/gizos "
                "--google_default_credentials --remote_timeout=60 "
                "--remote_retries=5 --remote_upload_local_results=false //..."
            ],
        )

    def test_bazel_disk_cache_falls_back_without_remote_cache(self) -> None:
        result, commands = self.run_make(
            "bazel-build",
            "BAZEL_CONFIG=linux_x86_64",
            "BAZEL_DISK_CACHE_MODE=off",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            commands,
            ["build --config=linux_x86_64 //..."],
        )

    def test_bazel_build_can_minimize_remote_output_downloads(self) -> None:
        result, commands = self.run_make(
            "bazel-build",
            "BAZEL_CONFIG=linux_x86_64",
            "BAZEL_BUILD_REMOTE_DOWNLOAD_OUTPUTS=minimal",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            commands,
            [
                "build --config=linux_x86_64 "
                "--remote_download_outputs=minimal //..."
            ],
        )

    def test_bazel_disk_cache_is_disabled_with_remote_cache(self) -> None:
        result, commands = self.run_make(
            "bazel-build",
            "BAZEL_CONFIG=linux_x86_64",
            "BAZEL_DISK_CACHE_MODE=off",
            "BAZEL_REMOTE_CACHE_URL=https://storage.googleapis.com/cache/gizos",
            "BAZEL_REMOTE_CACHE_MODE=read",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            commands,
            [
                "build --config=linux_x86_64 --disk_cache= "
                "--remote_cache=https://storage.googleapis.com/cache/gizos "
                "--google_default_credentials --remote_timeout=60 "
                "--remote_retries=5 --remote_upload_local_results=false //..."
            ],
        )

    def test_bazel_remote_cache_configuration_fails_closed(self) -> None:
        for variables in (
            (
                "BAZEL_REMOTE_CACHE_URL=http://storage.googleapis.com/cache/gizos",
                "BAZEL_REMOTE_CACHE_MODE=read",
            ),
            (
                "BAZEL_REMOTE_CACHE_URL=https://storage.googleapis.com/cache/gizos",
                "BAZEL_REMOTE_CACHE_MODE=",
            ),
            (
                "BAZEL_REMOTE_CACHE_URL=https://storage.googleapis.com/cache/team/gizos",
                "BAZEL_REMOTE_CACHE_MODE=read",
            ),
            (
                "BAZEL_REMOTE_CACHE_URL=https://storage.googleapis.com//gizos",
                "BAZEL_REMOTE_CACHE_MODE=read",
            ),
            ("BAZEL_DISK_CACHE_MODE=write",),
            ("BAZEL_BUILD_REMOTE_DOWNLOAD_OUTPUTS=all",),
        ):
            with self.subTest(variables=variables):
                result, commands = self.run_make("bazel-build", *variables)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(commands, [])

    def test_bazel_build_and_test_cover_the_selected_config(self) -> None:
        result, commands = self.run_make("bazel-build", "BAZEL_CONFIG=")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(commands, ["build //..."])

        result, commands = self.run_make(
            "bazel-build", "BAZEL_CONFIG=linux_x86_64"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(commands, ["build --config=linux_x86_64 //..."])

        result, commands = self.run_make(
            "bazel-test", "BAZEL_CONFIG=macos_arm64"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            commands,
            [
                "test --config=macos_arm64 //..."
            ],
        )

        result, commands = self.run_make(
            "bazel-test", "BAZEL_CONFIG=ios_sim_arm64"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            commands,
            ["test --config=ios_sim_arm64 //..."],
        )

        result, commands = self.run_make(
            "bazel-test", "BAZEL_CONFIG=android_arm64"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            commands,
            ["test --config=android_arm64 //..."],
        )

    def test_preview_inputs_fail_before_build_or_server(self) -> None:
        cases = (
            ("guides-watch", "GUIDES_WATCH_PORT=0"),
            ("guides-preview", "GUIDES_PREVIEW_HOST=bad host"),
        )
        for target, variable in cases:
            with self.subTest(target=target, variable=variable):
                result, commands = self.run_make(target, variable)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(commands, [])

if __name__ == "__main__":
    unittest.main()
