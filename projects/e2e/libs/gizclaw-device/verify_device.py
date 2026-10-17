"""Validate one complete immutable ledger; never combine boots or replay frames."""
import argparse
from datetime import datetime, timedelta
import hashlib
import json
from pathlib import Path
import re
import zlib

BEGIN = re.compile(rb"^H2_GIZCLAW_LEDGER stage=begin version=([A-Za-z0-9._-]+) execution=([0-9a-f]{32}) bytes=([0-9]+) records=([0-9]+) crc32=([0-9a-f]{8}) admitted=([01]) confirm_rc=(-?[0-9]+) physical_audio=([01])$")
END = re.compile(rb"^H2_GIZCLAW_LEDGER stage=end execution=([0-9a-f]{32}) crc32=([0-9a-f]{8})$")


BOOT = re.compile(rb"^H2_GIZCLAW_BOOT board=[A-Za-z0-9._-]+ version=([A-Za-z0-9._-]+) execution=([0-9a-f]{32})$")
LAUNCHER = re.compile(rb"^H2_GIZCLAW_E2E_(?:DEVKIT|AMOLED) stage=launcher status=READY$")
BLE_WINDOW = re.compile(rb"^H2_GIZCLAW_E2E stage=ble-binding-ready window=(h2e2e-[0-9a-f]{16}) revision=([0-9]+) url_bytes=([0-9]+) url_sha256=([0-9a-f]{64})$", re.MULTILINE)
PEER_TIMEOUT_MS = 120000


def utc(value):
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        if parsed.utcoffset() != timedelta(0):
            raise ValueError("UTC timestamp required")
        return parsed
    except (TypeError, AttributeError, ValueError) as error:
        raise ValueError("invalid UTC timestamp") from error


def object_data(data):
    try:
        value = json.loads(data)
    except (TypeError, ValueError) as error:
        raise ValueError("missing or invalid collector facts") from error
    if not isinstance(value, dict):
        raise ValueError("collector facts must be a JSON object")
    return value


def command_arguments(facts):
    argv = facts.get("argv")
    if not isinstance(argv, list) or not argv or any(not isinstance(arg, str) or not arg for arg in argv):
        raise ValueError("missing actual command argv")
    return argv


def verify_ble_peer(body, data, *, version, execution, process_exit_code,
                    raw_log=None, capture_data=None, process_data=None):
    """Compare one execution to independent historical capture/process facts.

    This never compares evidence to the verifier's current wall clock. A whole
    historical bundle remains auditable; mixing it with another boot fails.
    main() first binds this identity to the admitted ledger and its wrapper.
    """
    windows = BLE_WINDOW.findall(body)
    if len(windows) != 1 or type(process_exit_code) is not int or process_exit_code != 0:
        raise ValueError("missing unique BLE window or successful central process")
    if (not re.fullmatch(r"[A-Za-z0-9._-]{1,127}", version)
            or not re.fullmatch(r"[0-9a-f]{32}", execution) or execution == "0" * 32
            or not isinstance(raw_log, bytes)):
        raise ValueError("missing expected firmware execution or raw capture")
    boots = [match.groups() for line in raw_log.replace(b"\r\n", b"\n").splitlines()
             if (match := BOOT.fullmatch(line))]
    if boots != [(version.encode(), execution.encode())]:
        raise ValueError("capture does not contain this unique boot identity")
    window, revision, size, digest = windows[0]
    peer, capture, process = (object_data(value) for value in (data, capture_data, process_data))
    expected = dict(schema=2, backend="darwin-pal", version=version, execution=execution,
                    timeout_ms=PEER_TIMEOUT_MS, window=window.decode(),
                    revision=int(revision), url_bytes=int(size),
                    url_sha256=digest.decode(), completed_connections=2,
                    rediscovered=True, rc=0, qualified=True)
    if (not 0 < int(size) <= 1024 or not int(revision)
            or any(type(peer.get(key)) is not type(value) or peer[key] != value
                   for key, value in expected.items())):
        raise ValueError("central receipt differs from this BLE execution")
    chunks = peer.get("chunks")
    if (not isinstance(chunks, list) or len(chunks) != 2
            or any(type(value) is not int or not 0 < value <= int(size)
                   for value in chunks)):
        raise ValueError("central did not complete two bounded transfers")
    elapsed = peer.get("elapsed_ms")
    actual_elapsed = process.get("elapsed_ms")
    if (type(elapsed) is not int or not 0 <= elapsed < PEER_TIMEOUT_MS
            or type(actual_elapsed) is not int or actual_elapsed < elapsed
            or type(process.get("pid")) is not int or process["pid"] <= 0
            or type(process.get("exit_code")) is not int or process["exit_code"] != process_exit_code
            or process.get("receipt_sha256") != hashlib.sha256(data).hexdigest()
            or type(capture.get("exit_code")) is not int or capture["exit_code"] not in (0, 130, -2)
            or capture.get("monitor_stop") != "complete-qualified-ledger"
            or capture.get("sha256") != hashlib.sha256(raw_log).hexdigest()
            or capture.get("version") != version or capture.get("execution") != execution):
        raise ValueError("missing successful independent capture/process facts")
    argv = command_arguments(process)
    if not (Path(argv[0]).name == "ble_peer" or
            ("run" in argv and any(arg.endswith("//projects/e2e/libs/gizclaw-device:ble_peer") for arg in argv))):
        raise ValueError("actual process is not the BLE central")
    for flag, value in {"--version": version, "--execution": execution,
                        "--window": window.decode(), "--revision": revision.decode(),
                        "--size": size.decode(), "--sha256": digest.decode()}.items():
        if argv.count(flag) != 1 or argv.index(flag) + 1 >= len(argv) or argv[argv.index(flag) + 1] != value:
            raise ValueError("central argv differs from this execution")
    boot_argv = command_arguments(capture)
    if (boot_argv.count("reboot") != 1 or boot_argv.index("reboot") + 1 >= len(boot_argv)
            or boot_argv[boot_argv.index("reboot") + 1] not in ("upgrade", "app")
            or not ({"--monitor", "--continuous-monitor"} & set(boot_argv))):
        raise ValueError("missing actual monitored boot command")
    try:
        start, finish = utc(peer["started_at_utc"]), utc(peer["finished_at_utc"])
        command_start, command_finish = utc(process["started_utc"]), utc(process["finished_utc"])
        if not (utc(capture["started_utc"]) <= utc(capture["boot_observed_utc"])
                <= utc(capture["ble_ready_utc"]) <= command_start <= start <= finish
                <= command_finish <= utc(capture["finished_utc"])):
            raise ValueError("central timestamps are outside this capture/process interval")
        duration = (finish - start).total_seconds() * 1000
        actual_duration = (command_finish - command_start).total_seconds() * 1000
        if (not 0 <= duration < PEER_TIMEOUT_MS or abs(duration - elapsed) > 1000
                or abs(actual_duration - actual_elapsed) > 1000):
            raise ValueError("central duration exceeds its enforced timeout or process facts")
    except KeyError as error:
        raise ValueError("missing independent capture/process timestamps") from error
    return {**expected, "valid": True, "chunks": chunks,
            "receipt_sha256": hashlib.sha256(data).hexdigest(),
            "capture_receipt_sha256": hashlib.sha256(capture_data).hexdigest(),
            "command_receipt_sha256": hashlib.sha256(process_data).hexdigest(),
            "process_exit_code": process_exit_code}


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
        if (line.startswith(b"H2_GIZCLAW_BOOT ") or LAUNCHER.fullmatch(line)
                or line.startswith(b"H2_GIZCLAW_SETUP_FAIL ")):
            # A reboot/setup failure after a good run cannot borrow that run.
            # Early platform-only markers precede READY and the execution nonce.
            # Retire old frozen identities even if their bytes replay afterwards.
            changed_boots.update(frozen)
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
                   "wrapper_observed": explicit_boot,
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
    parser.add_argument("--execution", required=True,
                        help="execution observed in this boot's wrapper, including historical offline audits")
    parser.add_argument("--previous-execution")
    parser.add_argument("--ble-peer-receipt", type=Path, required=True)
    parser.add_argument("--ble-peer-exit-code", type=int, required=True,
                        help="actual exit status of the independent BLE central")
    parser.add_argument("--capture-receipt", type=Path, required=True)
    parser.add_argument("--ble-peer-command-receipt", type=Path, required=True)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[a-z0-9][a-z0-9_.-]{0,127}", args.platform):
        parser.error("--platform requires the exact canonical board identity")
    # The independent inventory/oracle remains shared with all other platforms.
    from projects.e2e.apps.gizclaw.api_coverage import audit, requirements
    raw_log = args.log.read_bytes()
    body, receipt = extract(raw_log, version=args.version,
                            previous_execution=args.previous_execution)
    if receipt["execution"] != args.execution or not receipt["wrapper_observed"]:
        parser.error("admitted ledger must match the expected execution and its observed boot wrapper")
    rules = requirements()
    coverage = audit(body.decode().splitlines(), rules, endpoint=args.endpoint,
                     backend="h2peer", profile=args.profile,
                     platform=args.platform, process_exit_code=0)
    receipt["api_coverage"] = coverage
    try:
        receipt["ble_binding_peer"] = verify_ble_peer(
            body, args.ble_peer_receipt.read_bytes(),
            version=receipt["version"], execution=receipt["execution"],
            process_exit_code=args.ble_peer_exit_code, raw_log=raw_log,
            capture_data=args.capture_receipt.read_bytes(),
            process_data=args.ble_peer_command_receipt.read_bytes())
    except (OSError, ValueError, TypeError):
        receipt["ble_binding_peer"] = {"valid": False}
    receipt["qualified"] = coverage["valid"] and receipt["ble_binding_peer"]["valid"]
    args.output.write_text(json.dumps(receipt, indent=2) + "\n")
    return 0 if receipt["qualified"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
