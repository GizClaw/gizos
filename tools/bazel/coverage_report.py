"""Build deterministic test and C/C++ coverage reports from Bazel evidence."""

from __future__ import annotations

from collections import defaultdict
import json
import math
from pathlib import Path, PurePosixPath
from typing import Iterable


SCHEMA_VERSION = 1
SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".inc",
    ".inl",
}
INSTRUMENTABLE_RULE_KINDS = {"cc_binary", "cc_library"}


class ReportError(ValueError):
    """Raised when coverage evidence is incomplete or inconsistent."""


def write_json(path: Path, value: object) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def parse_configured_labels(output: str, description: str) -> list[str]:
    labels = {
        line.strip().removeprefix("@@")
        for line in output.splitlines()
        if line.strip().startswith(("//", "@@//"))
        and " (Incompatible)" not in line
    }
    if not labels:
        raise ReportError(f"configured {description} inventory is empty")
    return sorted(labels)


def parse_test_inventory(output: str) -> list[str]:
    return parse_configured_labels(output, "automatic test")


def _label_to_path(label: str) -> str | None:
    label = label.removeprefix("@@//").removeprefix("//")
    if label.startswith("@") or ":" not in label:
        return None
    package, name = label.split(":", 1)
    path = PurePosixPath(package, name).as_posix()
    if PurePosixPath(path).suffix.lower() not in SOURCE_SUFFIXES:
        return None
    return path


def source_is_excluded(path: str) -> bool:
    parts = PurePosixPath(path).parts
    return (
        PurePosixPath(path).suffix.lower() not in SOURCE_SUFFIXES
        or "tests" in parts
        or "generated" in parts
    )


def parse_target_inventory(output: str) -> list[dict[str, object]]:
    targets: list[dict[str, object]] = []
    for line in output.splitlines():
        if not line.strip():
            continue
        fields = line.split("\t")
        if len(fields) != 4:
            raise ReportError(f"malformed C/C++ target inventory row: {line}")
        label, kind, _tags_text, srcs_text = fields
        label = label.removeprefix("@@")
        sources = sorted({
            path
            for source_label in srcs_text.split("|")
            if (path := _label_to_path(source_label)) is not None
            and not source_is_excluded(path)
        })
        package = label.removeprefix("@@//").removeprefix("//").split(":", 1)[0]
        if kind not in INSTRUMENTABLE_RULE_KINDS:
            raise ReportError(f"unsupported coverage target kind: {kind}: {label}")
        name = label.split(":", 1)[1]
        targets.append({
            "label": label,
            "kind": kind,
            "name": name,
            "display_name": f"{kind}/{name}",
            "package": package,
            "declared_sources": sources,
        })
    if not targets:
        raise ReportError("configured C/C++ target inventory is empty")
    return sorted(targets, key=lambda target: str(target["label"]))


def parse_target_inventory_json(
    output: str,
    compatible_labels: list[str],
) -> list[dict[str, object]]:
    try:
        document = json.loads(output)
    except json.JSONDecodeError as error:
        raise ReportError(f"malformed configured target JSON: {error}") from error
    if not isinstance(document, dict) or not isinstance(document.get("results"), list):
        raise ReportError("configured target JSON has no results array")
    compatible = set(compatible_labels)
    variants: dict[str, dict[str, object]] = {}
    for result in document["results"]:
        if not isinstance(result, dict):
            raise ReportError("configured target result is not an object")
        target = result.get("target")
        rule = target.get("rule") if isinstance(target, dict) else None
        if not isinstance(rule, dict):
            continue
        label = str(rule.get("name", "")).removeprefix("@@")
        if label not in compatible:
            continue
        attributes = {
            attribute.get("name"): attribute
            for attribute in rule.get("attribute", [])
            if isinstance(attribute, dict)
        }
        srcs_attribute = attributes.get("srcs", {})
        hdrs_attribute = attributes.get("hdrs", {})
        sources = sorted({
            path
            for source_label in [
                *srcs_attribute.get("stringListValue", []),
                *hdrs_attribute.get("stringListValue", []),
            ]
            if (path := _label_to_path(str(source_label))) is not None
            and not source_is_excluded(path)
        })
        kind = str(rule.get("ruleClass", ""))
        if kind not in INSTRUMENTABLE_RULE_KINDS:
            raise ReportError(f"unsupported coverage target kind: {kind}: {label}")
        name = label.split(":", 1)[1]
        target = {
            "label": label,
            "kind": kind,
            "name": name,
            "display_name": f"{kind}/{name}",
            "package": label.removeprefix("//").split(":", 1)[0],
            "declared_sources": sources,
        }
        previous = variants.get(label)
        if previous is not None and previous != target:
            raise ReportError(
                f"configured C/C++ target has inconsistent variants: {label}"
            )
        variants[label] = target
    missing = sorted(compatible - set(variants))
    if missing:
        raise ReportError(
            f"compatible C/C++ targets are missing configured attributes: {missing}"
        )
    if not variants:
        raise ReportError("configured C/C++ target inventory is empty")
    return [variants[label] for label in sorted(variants)]


def _build_status(events: Iterable[dict[str, object]], exit_code: int) -> str:
    status = "FAILED" if exit_code else "SUCCESS"
    for event in events:
        finished = event.get("finished") or event.get("buildFinished")
        if not isinstance(finished, dict):
            continue
        if finished.get("overallSuccess") is True:
            status = "SUCCESS"
        elif finished.get("overallSuccess") is False:
            status = "FAILED"
        exit_value = finished.get("exitCode")
        if isinstance(exit_value, dict) and exit_value.get("name"):
            status = str(exit_value["name"])
    return status


def parse_bep(path: Path, expected: list[str], exit_code: int) -> tuple[dict[str, object], list[str]]:
    events: list[dict[str, object]] = []
    if path.is_file():
        for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if not line.strip():
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError as error:
                raise ReportError(f"malformed BEP JSON on line {line_number}: {error}") from error
            if not isinstance(event, dict):
                raise ReportError(f"BEP event on line {line_number} is not an object")
            events.append(event)

    summaries: dict[str, dict[str, object]] = {}
    results: dict[str, list[dict[str, object]]] = defaultdict(list)
    errors: list[str] = []
    expected_set = set(expected)
    for event in events:
        event_id = event.get("id")
        if not isinstance(event_id, dict):
            continue
        summary_id = event_id.get("testSummary")
        summary = event.get("testSummary")
        if isinstance(summary_id, dict) and isinstance(summary, dict):
            label = summary_id.get("label")
            if not isinstance(label, str):
                raise ReportError("BEP test summary is missing its label")
            label = label.removeprefix("@@")
            if label in summaries:
                errors.append(f"duplicate test summary: {label}")
            elif label.startswith("//") and label not in expected_set:
                errors.append(f"unexpected first-party test summary: {label}")
            else:
                summaries[label] = summary
        result_id = event_id.get("testResult")
        result = event.get("testResult")
        if isinstance(result_id, dict) and isinstance(result, dict):
            label = result_id.get("label")
            if isinstance(label, str):
                label = label.removeprefix("@@")
                results[label].append({**result_id, **result})

    tests: list[dict[str, object]] = []
    counts = {
        "expected": len(expected),
        "passed": 0,
        "failed": 0,
        "incomplete": 0,
        "not_run": 0,
        "cached": 0,
    }
    for label in expected:
        summary = summaries.get(label)
        label_results = results.get(label, [])
        if summary is None:
            status = "NOT_RUN"
            counts["not_run"] += 1
            summary = {}
        else:
            status = str(summary.get("overallStatus", "INCOMPLETE"))
            if status == "PASSED":
                counts["passed"] += 1
            elif status in {"FAILED", "TIMEOUT", "FLAKY"}:
                counts["failed"] += 1
            else:
                counts["incomplete"] += 1
        if "totalNumCached" in summary:
            cached = int(summary.get("totalNumCached", 0) or 0)
        else:
            cached = sum(
                1
                for result in label_results
                if result.get("cachedLocally") is True
                or result.get("cachedRemotely") is True
                or result.get("status") == "CACHE_HIT"
            )
        counts["cached"] += cached
        tests.append({
            "label": label,
            "overall_status": status,
            "runs": int(summary.get("runCount", len({result.get("run") for result in label_results if result.get("run") is not None}) or 0) or 0),
            "attempts": int(summary.get("attemptCount", len(label_results)) or 0),
            "shards": int(summary.get("shardCount", len({result.get("shard") for result in label_results if result.get("shard") is not None}) or 0) or 0),
            "duration_ms": int(summary.get("totalRunDurationMillis", 0) or 0),
            "cached_actions": cached,
        })
    if counts["cached"]:
        errors.append(f"BEP reported {counts['cached']} cached test action(s)")
    if exit_code == 0 and (counts["passed"] != counts["expected"] or errors):
        errors.append("successful Bazel coverage command did not produce one fresh PASSED summary per expected test")
    report = {
        "schema_version": SCHEMA_VERSION,
        "config": "test_coverage",
        "bazel_exit_code": exit_code,
        "build_status": _build_status(events, exit_code),
        "counts": counts,
        "tests": tests,
    }
    return report, errors


def _normalize_source(path: str, root: Path) -> str | None:
    path = path.replace("\\", "/")
    for prefix in ("/proc/self/cwd/", "./"):
        if path.startswith(prefix):
            path = path[len(prefix):]
    for marker in ("/execroot/_main/", "/execroot/gizos/"):
        if marker in path:
            path = path.split(marker, 1)[1]
            break
    root_text = root.resolve().as_posix().rstrip("/") + "/"
    if path.startswith(root_text):
        path = path[len(root_text):]
    elif path.startswith("/"):
        try:
            path = Path(path).resolve().relative_to(root.resolve()).as_posix()
        except ValueError:
            return None
    if path.startswith("external/") or path.startswith("bazel-out/"):
        return None
    if path.startswith("/") or path.startswith("../"):
        return None
    return PurePosixPath(path).as_posix()


def _metric(covered: int, total: int) -> dict[str, int | float | None]:
    if covered < 0 or total < 0 or covered > total:
        raise ReportError(f"invalid coverage metric: covered={covered}, total={total}")
    percent = None if total == 0 else round(covered / total * 100.0, 2)
    if percent is not None and not math.isfinite(percent):
        raise ReportError("coverage percentage is not finite")
    return {"covered": covered, "total": total, "percent": percent}


def parse_lcov(path: Path, root: Path) -> tuple[dict[str, dict[str, object]], str]:
    if not path.is_file() or path.stat().st_size == 0:
        raise ReportError(f"combined LCOV report is missing or empty: {path}")
    records: dict[str, dict[str, object]] = {}
    kept_blocks: list[str] = []
    for raw_block in path.read_text(encoding="utf-8").split("end_of_record"):
        lines = [line for line in raw_block.splitlines() if line]
        if not lines:
            continue
        source_values = [line[3:] for line in lines if line.startswith("SF:")]
        if len(source_values) != 1:
            raise ReportError("LCOV record must contain exactly one SF field")
        source = _normalize_source(source_values[0], root)
        if source is None or source_is_excluded(source):
            continue
        if source in records:
            raise ReportError(f"duplicate LCOV source record: {source}")

        values: dict[str, int] = {}
        for key in ("LF", "LH", "FNF", "FNH", "BRF", "BRH"):
            matches = [line for line in lines if line.startswith(f"{key}:")]
            if len(matches) > 1:
                raise ReportError(f"duplicate {key} field for {source}")
            if matches:
                try:
                    values[key] = int(matches[0].split(":", 1)[1])
                except ValueError as error:
                    raise ReportError(f"invalid {key} field for {source}") from error
        for required in ("LF", "LH", "FNF", "FNH"):
            if required not in values:
                raise ReportError(f"LCOV record for {source} is missing {required}")
        branch_present = "BRF" in values or "BRH" in values
        if branch_present and not {"BRF", "BRH"}.issubset(values):
            raise ReportError(f"LCOV branch summary is incomplete for {source}")

        line_hits: dict[int, int] = {}
        for line in lines:
            if not line.startswith("DA:"):
                continue
            fields = line[3:].split(",")
            try:
                line_number, hit_count = int(fields[0]), int(fields[1])
            except (IndexError, ValueError) as error:
                raise ReportError(f"invalid DA field for {source}: {line}") from error
            if line_number in line_hits:
                raise ReportError(f"duplicate DA line {line_number} for {source}")
            line_hits[line_number] = hit_count
        if len(line_hits) != values["LF"] or sum(hit > 0 for hit in line_hits.values()) != values["LH"]:
            raise ReportError(f"LCOV line summary does not match DA records for {source}")

        function_hits: list[int] = []
        for line in lines:
            if not line.startswith("FNDA:"):
                continue
            try:
                hit_count_text, _ = line[5:].split(",", 1)
                function_hits.append(int(hit_count_text))
            except ValueError as error:
                raise ReportError(f"invalid FNDA field for {source}: {line}") from error
        if len(function_hits) != values["FNF"] or sum(hit > 0 for hit in function_hits) != values["FNH"]:
            raise ReportError(
                f"LCOV function summary does not match FNDA records for {source}"
            )

        branch_hits: list[bool] = []
        for line in lines:
            if not line.startswith("BRDA:"):
                continue
            fields = line[5:].split(",")
            if len(fields) != 4:
                raise ReportError(f"invalid BRDA field for {source}: {line}")
            taken = fields[3]
            try:
                branch_hits.append(taken != "-" and int(taken) > 0)
            except ValueError as error:
                raise ReportError(f"invalid BRDA field for {source}: {line}") from error
        if branch_hits and not branch_present:
            raise ReportError(f"LCOV branch summary is missing for {source}")
        if branch_present and (
            len(branch_hits) != values["BRF"]
            or sum(branch_hits) != values["BRH"]
        ):
            raise ReportError(
                f"LCOV branch summary does not match BRDA records for {source}"
            )
        metrics = {
            "lines": _metric(values["LH"], values["LF"]),
            "functions": _metric(values["FNH"], values["FNF"]),
            "branches": _metric(values.get("BRH", 0), values.get("BRF", 0)),
        }
        records[source] = {"metrics": metrics, "branch_present": branch_present}
        normalized_lines = [f"SF:{source}" if line.startswith("SF:") else line for line in lines]
        kept_blocks.append("\n".join(normalized_lines) + "\nend_of_record\n")
    if not records:
        raise ReportError("filtered LCOV report has no first-party production records")
    return records, "".join(kept_blocks)


def _sum_metrics(records: Iterable[dict[str, object]]) -> dict[str, dict[str, int | float | None]]:
    totals = {
        "lines": [0, 0],
        "functions": [0, 0],
        "branches": [0, 0],
    }
    for record in records:
        metrics = record["metrics"]
        assert isinstance(metrics, dict)
        for name in totals:
            metric = metrics[name]
            assert isinstance(metric, dict)
            totals[name][0] += int(metric["covered"])
            totals[name][1] += int(metric["total"])
    return {name: _metric(values[0], values[1]) for name, values in totals.items()}


def build_target_report(
    targets: list[dict[str, object]],
    records: dict[str, dict[str, object]],
) -> dict[str, object]:
    declared_sources = {
        str(source)
        for target in targets
        for source in target["declared_sources"]
    }
    unknown = sorted(set(records) - declared_sources)
    if unknown:
        raise ReportError(
            f"LCOV contains first-party sources outside eligible targets: {unknown}"
        )

    target_reports: list[dict[str, object]] = []
    measured_sources: set[str] = set()
    counts = {"eligible": len(targets), "measured": 0, "uncovered": 0, "not_applicable": 0}
    for target in targets:
        sources = [str(source) for source in target["declared_sources"]]
        attributed = sorted(source for source in sources if source in records)
        target_report: dict[str, object] = {
            "label": target["label"],
            "kind": target["kind"],
            "name": target["name"],
            "display_name": target["display_name"],
            "package": target["package"],
            "declared_sources": sources,
            "attributed_lcov_files": attributed,
        }
        if attributed:
            target_records = [records[source] for source in attributed]
            target_report.update({
                "status": "measured",
                "reason": None,
                "metrics": _sum_metrics(target_records),
            })
            measured_sources.update(attributed)
            counts["measured"] += 1
        elif not sources:
            target_report.update({
                "status": "not-applicable",
                "reason": "target declares no instrumentable C/C++ production source",
                "metrics": None,
            })
            counts["not_applicable"] += 1
        else:
            target_report.update({
                "status": "uncovered",
                "reason": "selected tests produced no LCOV record for the target's production sources",
                "metrics": None,
            })
            counts["uncovered"] += 1
        target_reports.append(target_report)
    if counts["measured"] == 0:
        raise ReportError("no eligible C/C++ target has measured production coverage")
    return {
        "schema_version": SCHEMA_VERSION,
        "config": "test_coverage",
        "counts": counts,
        "measured_overall": _sum_metrics(
            records[source] for source in sorted(measured_sources)
        ),
        "targets": target_reports,
    }


def _display_metric(metric: dict[str, object]) -> str:
    total = int(metric["total"])
    if total == 0:
        return "n/a (0 total)"
    return f"{metric['percent']:.2f}% ({metric['covered']}/{total})"


def render_summary(
    tests: dict[str, object],
    targets: dict[str, object] | None,
    errors: Iterable[str] = (),
) -> str:
    test_counts = tests["counts"]
    assert isinstance(test_counts, dict)
    lines = [
        "# Linux host test coverage",
        "",
        f"- Bazel result: `{tests['build_status']}` (exit `{tests['bazel_exit_code']}`)",
        f"- Tests passed: **{test_counts['passed']}/{test_counts['expected']}**",
        f"- Failed / incomplete / not run / cached: **{test_counts['failed']} / {test_counts['incomplete']} / {test_counts['not_run']} / {test_counts['cached']}**",
    ]
    if targets is not None:
        target_counts = targets["counts"]
        overall = targets["measured_overall"]
        target_reports = targets["targets"]
        assert isinstance(target_counts, dict) and isinstance(overall, dict)
        assert isinstance(target_reports, list)
        lines.extend([
            f"- Measured C/C++ targets: **{target_counts['measured']}/{target_counts['eligible']}** (uncovered {target_counts['uncovered']}, not applicable {target_counts['not_applicable']})",
            "",
            "Measured-scope coverage (not whole-repository coverage):",
            "",
            "| Metric | Coverage |",
            "| --- | ---: |",
            f"| Lines | {_display_metric(overall['lines'])} |",
            f"| Functions | {_display_metric(overall['functions'])} |",
            f"| Branches | {_display_metric(overall['branches'])} |",
            "",
            "## Coverage by target",
            "",
            "| Target | Bazel package | Status | Lines | Functions | Branches |",
            "| --- | --- | --- | ---: | ---: | ---: |",
        ])
        for target in target_reports:
            assert isinstance(target, dict)
            metrics = target["metrics"]
            if isinstance(metrics, dict):
                metric_values = [
                    _display_metric(metrics[name])
                    for name in ("lines", "functions", "branches")
                ]
            else:
                metric_values = ["n/a", "n/a", "n/a"]
            lines.append(
                f"| `{target['display_name']}` | `//{target['package']}` | "
                f"{target['status']} | {' | '.join(metric_values)} |"
            )
    error_list = list(errors)
    if error_list:
        lines.extend(["", "## Evidence errors", ""] + [f"- {error}" for error in error_list])
    return "\n".join(lines) + "\n"
