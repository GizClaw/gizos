#!/usr/bin/env python3
"""Generate complete Linux host test and C/C++ coverage reports."""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "scripts"))
from common.bazel import cache_options  # noqa: E402
from tools.bazel import coverage_report  # noqa: E402


OUTPUT = ROOT / "build/coverage/test"
CONFIG = "test_coverage"
TEST_QUERY = 'kind(".*_test rule", //...) except attr("tags", "manual", //...)'
TARGET_QUERY = 'kind("^(cc_binary|cc_library) rule$", //...)'
TEST_EXPRESSION = (
    'str(target.label) if "IncompatiblePlatformProvider" not in '
    'providers(target) else ""'
)
TARGET_EXPRESSION = (
    'str(target.label) if "IncompatiblePlatformProvider" not in '
    'providers(target) else ""'
)


def label_set(labels: list[str]) -> str:
    """Return a query expression containing only the supplied labels."""
    return f"set({' '.join(labels)})"


def target_instrumentation_filter(
    targets: list[dict[str, object]],
) -> str:
    packages = sorted({
        str(target["package"])
        for target in targets
    })
    if not packages:
        raise coverage_report.ReportError(
            "configured C/C++ target inventory is empty"
        )
    return rf"^//({'|'.join(re.escape(package) for package in packages)}):"


def run(
    arguments: list[str],
    *,
    check: bool = True,
    capture: bool = False,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        arguments,
        cwd=ROOT,
        check=check,
        stdout=subprocess.PIPE if capture else None,
        text=True,
    )


def query_graph(bazel: str, bazel_cache_options: list[str]) -> tuple[list[str], list[dict[str, object]]]:
    candidate_tests = coverage_report.parse_configured_labels(
        run(
            [bazel, "query", TEST_QUERY, "--output=label"],
            capture=True,
        ).stdout,
        "test candidate",
    )
    candidate_targets = coverage_report.parse_configured_labels(
        run(
            [bazel, "query", TARGET_QUERY, "--output=label"],
            capture=True,
        ).stdout,
        "C/C++ target candidate",
    )
    common = [
        bazel,
        "cquery",
        f"--config={CONFIG}",
        "--define=h2_host_os=linux",
        *bazel_cache_options,
    ]
    test_output = run(
        [
            *common,
            label_set(candidate_tests),
            "--output=starlark",
            f"--starlark:expr={TEST_EXPRESSION}",
        ],
        capture=True,
    ).stdout
    # Tests can transition dependencies to another platform (for example Wasm).
    # Include those configured owners when attributing their emitted LCOV.
    target_graph = (
        f'{label_set(candidate_targets)} union '
        'filter("^//", kind("^(cc_binary|cc_library) rule$", '
        f'deps({label_set(coverage_report.parse_test_inventory(test_output))})))'
    )
    target_labels_output = run(
        [
            *common,
            target_graph,
            "--output=starlark",
            f"--starlark:expr={TARGET_EXPRESSION}",
        ],
        capture=True,
    ).stdout
    target_json_output = run(
        [*common, target_graph, "--output=jsonproto"],
        capture=True,
    ).stdout
    return (
        coverage_report.parse_test_inventory(test_output),
        coverage_report.parse_target_inventory_json(
            target_json_output,
            coverage_report.parse_configured_labels(
                target_labels_output,
                "C/C++ target",
            ),
        ),
    )


def main() -> int:
    bazel = os.environ.get("BAZEL_BIN", "bazel")
    for tool in ("lcov", "genhtml"):
        if shutil.which(tool) is None:
            print(f"required command is unavailable: {tool}", file=sys.stderr)
            return 1
    if OUTPUT.exists():
        shutil.rmtree(OUTPUT)
    (OUTPUT / "report").mkdir(parents=True)

    test_report: dict[str, object] | None = None
    target_report: dict[str, object] | None = None
    evidence_errors: list[str] = []
    bazel_exit_code = 1
    try:
        bazel_cache_options = cache_options()
        expected_tests, targets = query_graph(bazel, bazel_cache_options)
        instrumentation_filter = target_instrumentation_filter(targets)
        bazel_output = Path(
            run([bazel, "info", "output_path"], capture=True).stdout.strip()
        )
        combined = bazel_output / "_coverage/_coverage_report.dat"
        combined.unlink(missing_ok=True)

        with tempfile.NamedTemporaryFile(
            prefix="h2-coverage-bep-",
            suffix=".json",
            delete=False,
        ) as bep_file:
            bep_path = Path(bep_file.name)
        try:
            result = run(
                [
                    bazel,
                    "coverage",
                    f"--config={CONFIG}",
                    "--define=h2_host_os=linux",
                    *bazel_cache_options,
                    "--combined_report=lcov",
                    "--cache_test_results=no",
                    "--test_output=errors",
                    f"--instrumentation_filter={instrumentation_filter}",
                    f"--build_event_json_file={bep_path}",
                    *expected_tests,
                ],
                check=False,
            )
            bazel_exit_code = result.returncode
            test_report, evidence_errors = coverage_report.parse_bep(
                bep_path,
                expected_tests,
                bazel_exit_code,
            )
        finally:
            bep_path.unlink(missing_ok=True)

        coverage_report.write_json(OUTPUT / "tests.json", test_report)
        if not combined.is_file() or combined.stat().st_size == 0:
            raise coverage_report.ReportError(
                f"combined LCOV report is missing or empty: {combined}"
            )

        records, filtered = coverage_report.parse_lcov(combined, ROOT)
        (OUTPUT / "coverage.dat").write_text(filtered, encoding="utf-8")
        target_report = coverage_report.build_target_report(targets, records)
        coverage_report.write_json(OUTPUT / "targets.json", target_report)
        summary = coverage_report.render_summary(
            test_report,
            target_report,
            evidence_errors,
        )
        (OUTPUT / "summary.md").write_text(summary, encoding="utf-8")

        run(["lcov", "--summary", str(OUTPUT / "coverage.dat")])
        run(
            [
                "genhtml",
                "--quiet",
                "--output-directory",
                str(OUTPUT / "report"),
                str(OUTPUT / "coverage.dat"),
            ]
        )
        if not (OUTPUT / "report/index.html").is_file():
            raise coverage_report.ReportError("coverage HTML report was not generated")
        required = [
            OUTPUT / "coverage.dat",
            OUTPUT / "targets.json",
            OUTPUT / "tests.json",
            OUTPUT / "summary.md",
            OUTPUT / "report/index.html",
        ]
        empty = [str(path) for path in required if not path.is_file() or path.stat().st_size == 0]
        if empty:
            raise coverage_report.ReportError(f"required coverage outputs are missing or empty: {empty}")
        if bazel_exit_code != 0 or evidence_errors:
            raise coverage_report.ReportError(
                f"test evidence reconciliation failed with Bazel exit {bazel_exit_code}"
            )
        print(summary, end="")
        return 0
    except (
        OSError,
        coverage_report.ReportError,
        subprocess.CalledProcessError,
    ) as error:
        if test_report is not None:
            (OUTPUT / "summary.md").write_text(
                coverage_report.render_summary(
                    test_report,
                    target_report,
                    [*evidence_errors, str(error)],
                ),
                encoding="utf-8",
            )
        else:
            (OUTPUT / "summary.md").write_text(
                "# Test coverage\n\n"
                "Coverage generation failed before test evidence was available.\n\n"
                f"- Error: `{error}`\n",
                encoding="utf-8",
            )
        print(f"coverage generation failed: {error}", file=sys.stderr)
        return bazel_exit_code if bazel_exit_code != 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
