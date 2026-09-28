#!/usr/bin/env python3
"""Check PAL Core's reviewed interface-to-case inventory, not dynamic coverage.

Bazel: read only declared runfiles; no compiler, network, or third-party packages.
Local: python3 test_interface_coverage.py --root /absolute/path/to/gizos

The denominator is independently parsed from the nine public PAL vtables. The
manifest cannot reduce it by omitting a header or by deriving APIs from cases.
Function evidence is lexical: it proves that a reviewed App function contains a
wrapper call, not that a branch executes, an assertion is sufficient, or a
provider conforms. The real platform suite supplies that separate evidence.
"""

from __future__ import annotations

import argparse
import copy
import json
import os
from pathlib import Path, PurePosixPath
import re
import sys
import unittest

APP = "projects/e2e/apps/pal-core/app"
MANIFEST = f"{APP}/api_coverage.json"
REGISTRY = f"{APP}/include/h2_pal_core_cases.inc"
APP_HEADER = f"{APP}/include/h2_pal_core_e2e.h"
APP_MAIN = f"{APP}/src/h2_pal_core_e2e.c"
CORE_HEADERS = tuple(
    f"libs/pal/include/h2/pal/os/h2_pal_{name}.h"
    for name in (
        "mem", "log", "time", "timer", "task", "queue", "sync",
        "system_event", "firmware_info",
    )
)
LEXEMES = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S
)
VTABLE = re.compile(
    r"typedef\s+struct\s+(h2_pal_\w+_vtable)\s*\{(.*?)\}\s*(\w+)\s*;", re.S
)
DECLARATION = re.compile(r"(.+?)\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)\s*", re.S)
TOKEN = re.compile(r"[A-Za-z_]\w*|[0-9]+|[^\s]")
CASE = re.compile(r'H2_PAL_CORE_CASE\s*\(\s*"(pal\.core\.[a-z0-9.-]+)"\s*\)')
FUNCTION = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


class CoverageError(ValueError):
    pass


def without_comments(text: str, *, strings: bool = False) -> str:
    """Keep line positions while ignoring fake declarations/calls in comments."""
    def replace(match: re.Match[str]) -> str:
        value = match.group()
        if value.startswith(("//", "/*")) or strings:
            return "".join("\n" if ch == "\n" else " " for ch in value)
        return value
    return LEXEMES.sub(replace, text)


def signature(declaration: str) -> str:
    return " ".join(TOKEN.findall(declaration.strip().rstrip(";")))


def functions(text: str) -> dict[str, str]:
    """Extract balanced C function bodies (control statements are excluded)."""
    clean = without_comments(text, strings=True)
    result: dict[str, str] = {}
    for match in FUNCTION.finditer(clean):
        name = match.group(1)
        if name in {"if", "for", "while", "switch"}:
            continue
        cursor, depth = match.end(), 1
        while cursor < len(clean) and depth:
            if clean[cursor] in ";{}":
                break
            depth += (clean[cursor] == "(") - (clean[cursor] == ")")
            cursor += 1
        if depth:
            continue
        while cursor < len(clean) and clean[cursor].isspace():
            cursor += 1
        if cursor == len(clean) or clean[cursor] != "{":
            continue
        start = cursor + 1
        cursor, depth = start, 1
        while cursor < len(clean) and depth:
            depth += (clean[cursor] == "{") - (clean[cursor] == "}")
            cursor += 1
        if depth:
            raise CoverageError(f"unbalanced function body: {name}")
        if name in result:
            raise CoverageError(f"ambiguous function evidence: {name}")
        result[name] = clean[start:cursor - 1]
    return result


def read_vtables(headers: dict[str, str]) -> dict[str, dict[str, str]]:
    operations: dict[str, dict[str, str]] = {}
    for path in CORE_HEADERS:
        if path not in headers:
            raise CoverageError(f"missing public header: {path}")
        matches = list(VTABLE.finditer(without_comments(headers[path])))
        if len(matches) != 1:
            raise CoverageError(f"expected one public Core vtable in {path}")
        vtable, body, _alias = matches[0].groups()
        for member in body.split(";"):
            if not member.strip():
                continue
            declaration = DECLARATION.fullmatch(member.strip())
            if declaration is None:
                raise CoverageError(f"unparsed vtable member in {path}: {member.strip()}")
            _returns, operation, _args = declaration.groups()
            key = f"{vtable}.{operation}"
            if key in operations:
                raise CoverageError(f"duplicate public operation: {key}")
            operations[key] = {
                "header": path, "vtable": vtable, "operation": operation,
                "signature": signature(member),
            }
    return operations


def registry_ids(text: str) -> list[str]:
    clean = without_comments(text)
    ids = CASE.findall(clean)
    if not ids or CASE.sub("", clean).strip():
        raise CoverageError("registry must contain only H2_PAL_CORE_CASE entries")
    if len(set(ids)) != len(ids):
        raise CoverageError("duplicate case ID in registry")
    return ids


def validate_registry_wiring(header: str, source: str, ids: list[str]) -> None:
    """Verify both arrays and public count consume the complete same registry."""
    header = without_comments(header)
    source = without_comments(source)
    numeric = re.search(r"#define\s+H2_PAL_CORE_E2E_CASE_COUNT\s+(\d+)[uU]?\b", header)
    if numeric:
        if int(numeric.group(1)) != len(ids):
            raise CoverageError("public CASE_COUNT differs from registry count")
    else:
        generated = re.search(
            r"#define\s+H2_PAL_CORE_CASE\(id\)\s+\+\s*1\s+"
            r"enum\s*\{\s*H2_PAL_CORE_E2E_CASE_COUNT\s*=\s*0\s*"
            r'#include\s+"h2_pal_core_cases\.inc"\s*\}\s*;\s*'
            r"#undef\s+H2_PAL_CORE_CASE\b", header,
        )
        if generated is None:
            raise CoverageError("CASE_COUNT must equal the complete registry (+1 from zero)")
    array = re.search(
        r"\bcase_ids\s*\[\s*H2_PAL_CORE_E2E_CASE_COUNT\s*\]\s*=\s*\{(.*?)\}\s*;",
        source, re.S,
    )
    if array is None:
        raise CoverageError("missing bounded case_ids registry array")
    body = array.group(1)
    if '#include "h2_pal_core_cases.inc"' in body or re.search(
        r'#include\s+"h2_pal_core_cases\.inc"', body
    ):
        generated = re.search(
            r"#define\s+H2_PAL_CORE_CASE\(id\)\s+id\s*,\s*"
            r'#include\s+"h2_pal_core_cases\.inc"\s*'
            r"#undef\s+H2_PAL_CORE_CASE\b", body,
        )
        if generated is None or (body[:generated.start()] + body[generated.end():]).strip():
            raise CoverageError("case_ids must emit each registry ID exactly once")
    else:
        literals = re.findall(r'"(pal\.core\.[a-z0-9.-]+)"', body)
        remainder = re.sub(r'"pal\.core\.[a-z0-9.-]+"|,|\s', "", body)
        if remainder or literals != ids:
            raise CoverageError("case_ids count/order differs from the complete registry")


def validate(manifest: dict, files: dict[str, str]) -> tuple[int, int]:
    if manifest.get("schema_version") != 1:
        raise CoverageError("unknown inventory schema_version")
    actual = read_vtables(files)
    ids = registry_ids(files[REGISTRY])
    validate_registry_wiring(files[APP_HEADER], files[APP_MAIN], ids)
    if manifest.get("operation_count") != len(actual):
        raise CoverageError("declared operation_count differs from public headers")
    rows = manifest.get("operations")
    if not isinstance(rows, list):
        raise CoverageError("operations must be an explicit reviewed list")
    source_paths = manifest.get("app_sources")
    if not isinstance(source_paths, list) or not source_paths:
        raise CoverageError("missing App source inventory")
    if len(set(source_paths)) != len(source_paths):
        raise CoverageError("duplicate App source inventory")
    for path in source_paths:
        normalized = PurePosixPath(path)
        if (".." in normalized.parts or normalized.is_absolute() or
                normalized.parent != PurePosixPath(f"{APP}/src") or
                normalized.suffix != ".c"):
            raise CoverageError(f"invalid App source path: {path}")
        if path not in files:
            raise CoverageError(f"missing App source: {path}")
    bodies = {path: functions(files[path]) for path in source_paths}
    wrappers = {path: functions(files[path]) for path in CORE_HEADERS}
    declared: set[str] = set()
    for row in rows:
        if not isinstance(row, dict):
            raise CoverageError("each operation must have exactly one owner record")
        key = f"{row.get('vtable')}.{row.get('operation')}"
        if key in declared:
            raise CoverageError(f"duplicate operation/owner: {key}")
        declared.add(key)
        if key not in actual:
            raise CoverageError(f"unknown or removed public operation: {key}")
        for field in ("header", "signature"):
            if row.get(field) != actual[key][field]:
                raise CoverageError(f"{key}: {field} differs from public header")
        owner = row.get("owner_case")
        if not isinstance(owner, str) or owner not in ids:
            raise CoverageError(f"{key}: missing/unknown single owner_case: {owner}")
        related = row.get("additional_cases", [])
        if (not isinstance(related, list) or any(not isinstance(item, str) for item in related) or
                len(set(related)) != len(related) or owner in related):
            raise CoverageError(f"{key}: duplicate or malformed additional cases")
        for case_id in related:
            if case_id not in ids:
                raise CoverageError(f"{key}: unknown additional case: {case_id}")
        wrapper = row.get("wrapper")
        if not isinstance(wrapper, str) or not re.fullmatch(r"h2_pal_\w+", wrapper):
            raise CoverageError(f"{key}: invalid PAL wrapper")
        wrapper_body = wrappers[row["header"]].get(wrapper, "")
        operation_call = rf"->\s*vtable\s*->\s*{re.escape(row['operation'])}\s*\("
        if not re.search(operation_call, wrapper_body):
            raise CoverageError(f"{key}: {wrapper} does not forward this public operation")
        evidence = row.get("evidence")
        if not isinstance(evidence, list) or not evidence:
            raise CoverageError(f"{key}: missing App wrapper-call evidence")
        sites: set[tuple[str, str]] = set()
        for site in evidence:
            if not isinstance(site, dict):
                raise CoverageError(f"{key}: malformed evidence")
            path, function = site.get("source"), site.get("function")
            if not isinstance(path, str) or not isinstance(function, str):
                raise CoverageError(f"{key}: malformed evidence path/function")
            if (path, function) in sites:
                raise CoverageError(f"{key}: duplicate evidence site")
            sites.add((path, function))
            if path not in bodies or function not in bodies[path]:
                raise CoverageError(f"{key}: unknown App evidence function: {path}:{function}")
            if not re.search(rf"\b{re.escape(wrapper)}\s*\(", bodies[path][function]):
                raise CoverageError(f"{key}: {wrapper} not called in {path}:{function}")
    missing = sorted(set(actual) - declared)
    if missing:
        raise CoverageError("unowned public operations: " + ", ".join(missing))
    return len(actual), len(ids)


def unique_keys(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise CoverageError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def find_root(explicit: str | None) -> Path:
    if explicit:
        root = Path(explicit)
        if not root.is_absolute():
            raise CoverageError("--root must be an absolute checkout/runfiles root")
        return root
    runfiles = os.environ.get("TEST_SRCDIR") or os.environ.get("RUNFILES_DIR")
    if runfiles:
        # TEST_WORKSPACE is authoritative. Bzlmod executables also expose _main.
        workspace = os.environ.get("TEST_WORKSPACE", "_main")
        root = Path(runfiles) / workspace
        if (root / MANIFEST).is_file():
            return root
        root = Path(runfiles) / "_main"
        if (root / MANIFEST).is_file():
            return root
    raise CoverageError("use Bazel runfiles or supply --root /absolute/checkout")


def load(root: Path) -> tuple[dict, dict[str, str]]:
    manifest = json.loads((root / MANIFEST).read_text(), object_pairs_hook=unique_keys)
    paths = set(CORE_HEADERS) | {REGISTRY, APP_HEADER, APP_MAIN}
    # Reject unsafe paths before touching them, including when inventory is bad.
    for path in manifest.get("app_sources", []):
        normalized = PurePosixPath(path)
        if (".." in normalized.parts or normalized.is_absolute() or
                normalized.parent != PurePosixPath(f"{APP}/src") or normalized.suffix != ".c"):
            raise CoverageError(f"invalid App source path: {path}")
        paths.add(path)
    return manifest, {path: (root / path).read_text() for path in paths}


class InterfaceCoverageTest(unittest.TestCase):
    """Inventory check plus meaningful negative tests against current headers."""
    root: Path

    def setUp(self) -> None:
        self.inventory, self.files = load(self.root)

    def test_reviewed_inventory_matches_public_interfaces(self) -> None:
        operations, cases = validate(self.inventory, self.files)
        print(f"PAL Core interface inventory: {operations} public operations, {cases} registered cases")

    def test_new_operation_requires_owner(self) -> None:
        path = CORE_HEADERS[0]
        self.files[path] = self.files[path].replace(
            "} h2_pal_mem_vtable_t;", "    void (*new_operation)(void *user);\n} h2_pal_mem_vtable_t;"
        )
        self.inventory["operation_count"] += 1
        with self.assertRaisesRegex(CoverageError, "unowned public operations"):
            validate(self.inventory, self.files)

    def test_removed_operation_rejects_stale_owner(self) -> None:
        path = CORE_HEADERS[0]
        self.files[path] = re.sub(r"\s*void\s*\*\(\*alloc\)\(void \*user, size_t len\);", "", self.files[path])
        self.inventory["operation_count"] -= 1
        with self.assertRaisesRegex(CoverageError, "removed public operation"):
            validate(self.inventory, self.files)

    def test_signature_change_requires_review(self) -> None:
        row = self.inventory["operations"][0]
        row["signature"] += " changed"
        with self.assertRaisesRegex(CoverageError, "signature differs"):
            validate(self.inventory, self.files)

    def test_missing_and_duplicate_owner_fail(self) -> None:
        removed = self.inventory["operations"].pop()
        with self.assertRaisesRegex(CoverageError, "unowned public operations"):
            validate(self.inventory, self.files)
        self.inventory["operations"].extend([removed, copy.deepcopy(removed)])
        with self.assertRaisesRegex(CoverageError, "duplicate operation/owner"):
            validate(self.inventory, self.files)

    def test_unknown_case_fails(self) -> None:
        self.inventory["operations"][0]["owner_case"] = "pal.core.unknown"
        with self.assertRaisesRegex(CoverageError, "unknown single owner_case"):
            validate(self.inventory, self.files)

    def test_duplicate_registry_case_fails(self) -> None:
        first = registry_ids(self.files[REGISTRY])[0]
        self.files[REGISTRY] += f'\nH2_PAL_CORE_CASE("{first}")\n'
        with self.assertRaisesRegex(CoverageError, "duplicate case ID"):
            validate(self.inventory, self.files)

    def test_wrong_registry_total_fails(self) -> None:
        self.files[APP_HEADER] = re.sub(
            r"H2_PAL_CORE_E2E_CASE_COUNT\s*=\s*0", "H2_PAL_CORE_E2E_CASE_COUNT = 1", self.files[APP_HEADER]
        )
        self.files[APP_HEADER] = re.sub(
            r"(#define\s+H2_PAL_CORE_E2E_CASE_COUNT\s+)\d+[uU]?", r"\g<1>999u", self.files[APP_HEADER]
        )
        with self.assertRaisesRegex(CoverageError, "CASE_COUNT"):
            validate(self.inventory, self.files)

    def test_missing_source_wrapper_call_fails(self) -> None:
        row = self.inventory["operations"][0]
        for path in self.inventory["app_sources"]:
            self.files[path] = re.sub(
                rf"\b{row['wrapper']}\s*\(", "removed_wrapper(", self.files[path]
            )
        with self.assertRaisesRegex(CoverageError, "not called"):
            validate(self.inventory, self.files)

    def test_comments_cannot_supply_wrapper_evidence(self) -> None:
        row = self.inventory["operations"][0]
        for path in self.inventory["app_sources"]:
            self.files[path] = re.sub(
                rf"\b{row['wrapper']}\s*\(", f"/* {row['wrapper']}() */ removed_wrapper(", self.files[path]
            )
        with self.assertRaisesRegex(CoverageError, "not called"):
            validate(self.inventory, self.files)

    def test_wrong_wrapper_for_operation_fails(self) -> None:
        self.inventory["operations"][0]["wrapper"] = "h2_pal_mem_free"
        with self.assertRaisesRegex(CoverageError, "does not forward"):
            validate(self.inventory, self.files)

    def test_duplicate_json_keys_fail(self) -> None:
        with self.assertRaisesRegex(CoverageError, "duplicate JSON key"):
            json.loads('{"owner_case":"one","owner_case":"two"}', object_pairs_hook=unique_keys)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, add_help=False)
    parser.add_argument("--root")
    options, remaining = parser.parse_known_args()
    try:
        InterfaceCoverageTest.root = find_root(options.root)
    except (CoverageError, OSError) as error:
        parser.error(str(error))
    unittest.main(argv=[sys.argv[0], *remaining])
