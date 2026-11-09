"""Validate a complete PAL Disk device ledger; never operate the device."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex


def require(condition, message):
    # Ledger rejection must still work when Python runs with optimizations.
    if not condition:
        raise AssertionError(message)


def fields(line):
    values = {}
    for token in shlex.split(line)[1:]:
        key, value = token.split("=", 1)
        require(key not in values, "duplicate field")
        values[key] = value
    return values


def verify(text, registry, expected_rounds, expected_cores=None):
    names = re.findall(r'H2_PAL_DISK_CASE\(\w+, "([^"]+)"\)', registry)
    require(names and len(names) == len(set(names)), "invalid registry")
    events = []
    for line in text.splitlines():
        match = re.search(r"H2_PAL_DISK_(?:E2E|CASE) ", line)
        if match:
            row = line[match.start():]
            events.append((row.split()[0], fields(row)))
    boot_indices = [i for i, (_, row) in enumerate(events)
                    if row.get("phase") == "boot"]
    require(len(boot_indices) == 1 and boot_indices[0] == 0,
            "missing or extra boot")
    require(not re.search(
        r"MemFault|HardFault|heartbeat timeout|Assert at:|crash happend",
        text, re.I), "device fault")
    cases = [row for kind, row in events if kind == "H2_PAL_DISK_CASE"]
    require([row.get("id") for row in cases] == names,
            "incomplete or reordered cases")
    require(all(row.get("status") == "PASS" and row.get("rc") == "0"
                for row in cases), "case failed or blocked")
    phases = {}
    for kind, row in events:
        if kind != "H2_PAL_DISK_E2E":
            continue
        require(row.get("contract") == "1", "unexpected contract")
        phase = row.get("phase")
        if phase != "progress":
            require(phase not in phases, "duplicate phase")
            phases[phase] = row
    require(list(phases) == [
        "boot", "management_ready", "region", "sequential", "concurrent_begin",
        "result", "run_returned", "release",
    ], "missing or reordered lifecycle")
    require(phases["management_ready"]["rc"] == "0", "management unavailable")
    result = phases["result"]
    identity = ("rc", "cleanup_rc", "output_rc", "passed", "failed", "blocked",
                "writer_rounds", "reader_rounds", "writer_stage", "reader_stage",
                "cores", "cores_verified", "workers_joined")
    for phase in ("run_returned", "release"):
        require(all(phases[phase].get(key) == result.get(key) for key in identity),
                "terminal receipts disagree")
    require(all(result.get(key) == "0" for key in
                ("rc", "cleanup_rc", "output_rc", "failed", "blocked")),
            "terminal failure")
    require(int(result["passed"]) == len(names), "incomplete pass count")
    require(expected_rounds > 0 and int(result["writer_rounds"]) == expected_rounds,
            "incomplete writer")
    require(int(result["reader_rounds"]) > 0 and result["workers_joined"] == "1",
            "incomplete reader or join")
    require(result["writer_stage"] == result["reader_stage"] == "4",
            "workers did not finish")
    if expected_cores is not None:
        require(len(expected_cores) == 2 and expected_cores[0] != expected_cores[1],
                "invalid expected cores")
        require(result["cores"] == ",".join(map(str, expected_cores)),
                "unexpected worker cores")
        require(result["cores_verified"] == "1", "unverified worker cores")
        require(phases["concurrent_begin"]["cores"] == result["cores"],
                "core receipts disagree")
        require(phases["concurrent_begin"]["cores_verified"] == "1",
                "unverified starting cores")
    return {"passed": True, "contract": 1, "cases": cases, "result": result,
            "log_sha256": hashlib.sha256(text.encode()).hexdigest(),
            "boundary": "Ledger only; artifact identity and device restoration require separate receipts."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--registry", type=Path, required=True)
    parser.add_argument("--rounds", type=int, required=True)
    parser.add_argument("--cores", type=int, nargs=2)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = verify(args.log.read_text(errors="replace"), args.registry.read_text(),
                    args.rounds, args.cores)
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
