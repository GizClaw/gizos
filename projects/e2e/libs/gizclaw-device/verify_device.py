"""Validate one complete immutable ledger; never combine boots or replay frames."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zlib

BEGIN = re.compile(rb"^H2_GIZCLAW_LEDGER stage=begin version=([A-Za-z0-9._-]+) execution=([0-9a-f]{32}) bytes=([0-9]+) records=([0-9]+) crc32=([0-9a-f]{8}) admitted=([01]) confirm_rc=(-?[0-9]+) physical_audio=([01])$")
END = re.compile(rb"^H2_GIZCLAW_LEDGER stage=end execution=([0-9a-f]{32}) crc32=([0-9a-f]{8})$")


BOOT = re.compile(rb"^H2_GIZCLAW_BOOT board=[A-Za-z0-9._-]+ version=([A-Za-z0-9._-]+) execution=([0-9a-f]{32})$")
LAUNCHER = re.compile(rb"^H2_GIZCLAW_E2E_(?:DEVKIT|AMOLED) stage=launcher status=READY$")


def extract(data, *, version, previous_execution=None):
    candidate = None
    records = []
    discarded = 0
    latest_boot = None
    explicit_boot = False
    accepted = None
    frozen = {}
    changed_boots = set()
    for line in data.replace(b"\r\n", b"\n").split(b"\n"):
        boot = BOOT.fullmatch(line)
        if boot or LAUNCHER.fullmatch(line) or line.startswith(b"H2_GIZCLAW_SETUP_FAIL "):
            # A reboot/setup failure after a good run cannot borrow that run.
            candidate = None
            records = []
            accepted = None
            latest_boot = boot.groups() if boot else None
            explicit_boot = bool(boot)
            continue
        begin = BEGIN.fullmatch(line)
        if begin:
            candidate = begin.groups()
            records = []
            identity = candidate[:2]
            if latest_boot is None or (not explicit_boot and identity != latest_boot):
                latest_boot = identity
                accepted = None
            elif identity != latest_boot:
                accepted = None
            if identity in frozen and candidate != frozen[identity][1]:
                changed_boots.add(identity)
                accepted = None
            continue
        if line.startswith(b"H2_GIZCLAW_LEDGER stage=begin"):
            # Even a damaged new frame must not preserve an earlier PASS.
            candidate = None
            accepted = None
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
        frame_header = candidate
        candidate = None
        crc = int(checksum, 16)
        if (frame_header[:2] in changed_boots or frame_header[:2] != latest_boot or actual_version.decode() != version
                or execution.decode() == previous_execution
                or end.group(1) != execution or end.group(2) != checksum
                or not 0 < len(body) <= 384 * 1024 or len(body) != int(size)
                or len(records) != int(count) or zlib.crc32(body) != crc
                or admitted != b"1" or confirm != b"0"):
            discarded += 1
            accepted = None
            continue
        identity = frame_header[:2]
        if identity in frozen and body != frozen[identity][0]:
            changed_boots.add(identity)
            accepted = None
            discarded += 1
            continue
        frozen[identity] = (body, frame_header)
        receipt = {"version": version, "execution": execution.decode(),
                   "physical_audio": physical == b"1", "confirm_rc": 0,
                   "records": len(records), "record_bytes": len(body),
                   "transport_crc32": checksum.decode(),
                   "ledger_sha256": hashlib.sha256(body).hexdigest()}
        accepted = (body, receipt, frame_header)
    if accepted is not None:
        # A collector may stop during an identical replay of this same frozen
        # boot. A different boot/header or changed replay body is never reused.
        if candidate is not None and (candidate != accepted[2]
                or not accepted[0].startswith(b"".join(records))):
            accepted = None
        else:
            accepted[1]["discarded_frames"] = discarded
            return accepted[:2]
    raise ValueError("no complete admitted ledger for the latest expected fresh boot")


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
