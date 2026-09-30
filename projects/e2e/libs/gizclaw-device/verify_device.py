"""Validate one complete immutable ledger; never combine boots or replay frames."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zlib

BEGIN = re.compile(rb"^H2_GIZCLAW_LEDGER stage=begin version=([A-Za-z0-9._-]+) execution=([0-9a-f]{32}) bytes=([0-9]+) records=([0-9]+) crc32=([0-9a-f]{8}) admitted=([01]) confirm_rc=(-?[0-9]+) physical_audio=([01])$")
END = re.compile(rb"^H2_GIZCLAW_LEDGER stage=end execution=([0-9a-f]{32}) crc32=([0-9a-f]{8})$")


def extract(data, *, version, previous_execution=None):
    candidate = None
    records = []
    discarded = 0
    for line in data.replace(b"\r\n", b"\n").split(b"\n"):
        begin = BEGIN.fullmatch(line)
        if begin:
            candidate = begin.groups()
            records = []
            continue
        if candidate is None:
            continue
        end = END.fullmatch(line)
        if not end:
            if line.startswith(b"H2_GIZCLAW_E2E "):
                records.append(line + b"\n")
            continue
        body = b"".join(records)
        actual_version, execution, size, count, checksum, admitted, confirm, physical = candidate
        candidate = None
        crc = int(checksum, 16)
        if (actual_version.decode() != version or execution.decode() == previous_execution
                or end.group(1) != execution or end.group(2) != checksum
                or not 0 < len(body) <= 384 * 1024 or len(body) != int(size)
                or len(records) != int(count) or zlib.crc32(body) != crc
                or admitted != b"1" or confirm != b"0"):
            discarded += 1
            continue
        return body, {"version": version, "execution": execution.decode(),
                      "physical_audio": physical == b"1", "confirm_rc": 0,
                      "records": len(records), "record_bytes": len(body),
                      "transport_crc32": checksum.decode(),
                      "ledger_sha256": hashlib.sha256(body).hexdigest(),
                      "discarded_frames": discarded}
    raise ValueError("no complete admitted ledger for the expected fresh boot")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--previous-execution")
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--platform", choices=("amoled", "devkit", "bk7258"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    # The independent inventory/oracle remains shared with all other platforms.
    from projects.e2e.apps.gizclaw.api_coverage import audit, requirements, repository_root, validate_inventory
    body, receipt = extract(args.log.read_bytes(), version=args.version,
                            previous_execution=args.previous_execution)
    rules = requirements()
    validate_inventory(rules, (repository_root() / "libs/gizclaw/tests/public_api.inc").read_text())
    coverage = audit(body.decode().splitlines(), rules, endpoint=args.endpoint,
                     backend="h2peer", profile=args.profile,
                     platform=args.platform, process_exit_code=0)
    receipt["api_coverage"] = coverage
    receipt["qualified"] = coverage["valid"]
    args.output.write_text(json.dumps(receipt, indent=2) + "\n")
    return 0 if receipt["qualified"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
