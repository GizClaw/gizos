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


def process_interval(fact):
    start, finish = utc(fact["started_utc"]), utc(fact["finished_utc"])
    elapsed = fact.get("elapsed_ms")
    if (type(fact.get("pid")) is not int or fact["pid"] <= 0
            or type(elapsed) is not int or elapsed < 0 or finish < start
            or abs((finish - start).total_seconds() * 1000 - elapsed) > 1000):
        raise ValueError("invalid actual transport process interval")
    return start, finish


def transport_arguments(fact, expected):
    argv = command_arguments(fact)
    if (not Path(argv[0]).name.startswith("h2loader")
            or fact.get("executable_sha256") != expected["cli_sha256"]):
        raise ValueError("transport executable differs from the frozen CLI")
    flags, index = {}, 1
    while index < len(argv) and argv[index].startswith("--"):
        flag = argv[index]
        if flag in flags:
            raise ValueError("duplicate transport option")
        if flag == "--no-ble":
            flags[flag] = True
            index += 1
        elif flag in {"--port", "--transport", "--baud", "--wait-timeout", "--read-timeout", "--post-delay"} and index + 1 < len(argv):
            flags[flag] = argv[index + 1]
            index += 2
        else:
            raise ValueError("unknown transport option")
    if (flags.get("--no-ble") is not True or flags.get("--port") != expected["port"]
            or flags.get("--transport") != "iostreamikcp" or flags.get("--read-timeout") != "1"):
        raise ValueError("capture did not use the explicit bounded UART transport")
    return argv[index:]


def typed_transport(fact, expected, command, marker):
    process_interval(fact)
    if (type(fact.get("exit_code")) is not int or fact["exit_code"] != 0
            or transport_arguments(fact, expected) != command
            or not isinstance(fact.get("stdout"), str)
            or fact.get("sha256") != hashlib.sha256(fact["stdout"].encode()).hexdigest()):
        raise ValueError("missing successful typed transport receipt")
    rows = [line for line in fact["stdout"].splitlines() if line.startswith(marker + " ")]
    if len(rows) != 1:
        raise ValueError("missing unique typed device status")
    fields = {}
    for token in rows[0][len(marker) + 1:].split():
        key, value = token.split("=", 1)
        if not key or not value or key in fields:
            raise ValueError("invalid typed status fields")
        fields[key] = value
    return fields


def verify_temporary_capture(capture, raw, body, version, execution):
    """Two actual processes; a PRECASE/BOOT nonce bridges their UART handoff.

    Metadata is parsed from immutable real-command stdout, never substituted
    for the first case ledger. The caller owns artifact/device provenance.
    """
    try:
        device, segments = capture["device"], capture["segments"]
        expected = device["expected"]
        if (not isinstance(segments, list) or len(segments) != 2
                or any(not re.fullmatch(r"[0-9a-f]{64}", expected[k])
                       for k in ("cli_sha256", "package_checksum", "image_checksum"))
                or any(type(expected[k]) is not int or expected[k] <= 0
                       for k in ("package_size", "image_size"))
                or not re.fullmatch(r"[0-9a-f]{12,32}", expected["uid"])
                or not re.fullmatch(r"[a-z0-9_]+", expected["board"])
                or expected["target"] != "esp32s3" or not expected["port"]):
            raise ValueError("missing frozen device/package identity")
        slices, intervals, offset = [], [], 0
        for segment in segments:
            count = segment.get("bytes")
            if (type(count) is not int or count <= 0 or type(segment.get("offset")) is not int
                    or segment["offset"] != offset or count > len(raw) - offset
                    or type(segment.get("exit_code")) is not int or segment["exit_code"] not in (0, 130, -2)):
                raise ValueError("invalid actual capture segment")
            chunk = raw[offset:offset + count]
            if hashlib.sha256(chunk).hexdigest() != segment.get("sha256"):
                raise ValueError("capture segment bytes changed")
            slices.append(chunk.replace(b"\r\n", b"\n"))
            intervals.append(process_interval(segment))
            offset += count
        if (offset != len(raw) or intervals[0][1] > intervals[1][0]
                or segments[0]["pid"] == segments[1]["pid"]
                or segments[0].get("monitor_stop") != "precase-awaiting-temporary-wifi"
                or segments[1].get("monitor_stop") != "complete-qualified-ledger"
                or capture["exit_code"] != segments[1]["exit_code"]
                or utc(capture["started_utc"]) != intervals[0][0]
                or utc(capture["finished_utc"]) != intervals[1][1]):
            raise ValueError("capture processes overlap or contain missing bytes")
        reboot = transport_arguments(segments[0], expected)
        wifi = transport_arguments(segments[1], expected)
        if (len(reboot) != 3 or reboot[0] != "reboot" or reboot[1] not in ("upgrade", "app")
                or reboot[2] != "--monitor" or len(wifi) != 6 or wifi[:2] != ["wifi", "connect"]
                or wifi[4:] != ["--temporary", "--monitor"]):
            raise ValueError("missing real monitored reboot and temporary Wi-Fi commands")
        ack = f"H2_LOADER_REBOOT target={reboot[1]} result=accepted\n".encode()
        precase = (f"H2_GIZCLAW_PRECASE board={expected['board']} version={version} "
                   f"execution={execution} state=precase\n").encode()
        boot = (f"H2_GIZCLAW_BOOT board={expected['board']} version={version} execution={execution}\n").encode()
        first, second = slices
        requested = first.split(ack, 1)[-1]
        precases = [line for line in (first + second).splitlines() if line.startswith(b"H2_GIZCLAW_PRECASE ")]
        if (first.count(ack) != 1 or precases != [precase.rstrip(b"\n")]
                or first.find(ack) >= first.find(precase) or second.count(boot) != 1
                or any(line.startswith((b"H2_GIZCLAW_BOOT ", b"H2_GIZCLAW_LEDGER ", b"H2_GIZCLAW_E2E "))
                       for line in requested.splitlines())
                or b"status=NO_SAVED_WIFI " not in requested):
            raise ValueError("reboot ACK and precase identity do not bind this waiting boot")
        boot_index = second.find(boot)
        if any(line.startswith((b"H2_GIZCLAW_E2E ", b"H2_GIZCLAW_LEDGER "))
               for line in second[:boot_index].splitlines()):
            raise ValueError("business evidence appeared before this boot's network gate")
        boundaries = (b"H2_LOADER_REBOOT ", b"H2_GIZCLAW_PRECASE ", b"H2_GIZCLAW_SETUP_FAIL ",
                      b"ESP-ROM:", b"rst:0x", b"Guru Meditation Error:", b"Rebooting...", b"H2_BOARD_ENTRY_FAIL ")
        if (any(line.startswith(boundaries) or LAUNCHER.fullmatch(line) for line in second.splitlines())
                or any(line.startswith(boundaries) for line in first[first.find(precase) + len(precase):].splitlines())
                or sum(line.startswith(b"H2_LOADER_REBOOT ") for line in first.splitlines()) != 1):
            raise ValueError("another reboot/setup boundary interrupted the handoff")
        frame_index = second.find(b"H2_GIZCLAW_LEDGER stage=begin ")
        end = re.search(rb"^H2_GIZCLAW_LEDGER stage=end [^\n]+\n", second, re.MULTILINE)
        ready = BLE_WINDOW.findall(body)
        live = BLE_WINDOW.search(second[:frame_index]) if frame_index >= 0 else None
        if (end is None or not live or live.groups() != ready[0] or live.start() <= boot_index
                or not any(line.startswith(b"H2_LOADER_WIFI_TEMPORARY result=connected ")
                           for line in second[:frame_index].splitlines())
                or second[:end.end()].count(b"H2_GIZCLAW_LEDGER stage=begin ") != 1):
            raise ValueError("missing immediate ready or first complete ledger after real temporary Wi-Fi")
        first_body, first_identity = extract(second[:end.end()], version=version)
        if first_body != body or first_identity["execution"] != execution or first_identity["discarded_frames"]:
            raise ValueError("a replay replaced the first complete case ledger")
        if not (intervals[0][0] <= utc(capture["precase_observed_utc"]) <= intervals[0][1]
                and intervals[1][0] <= utc(capture["boot_observed_utc"])
                <= utc(capture["ble_ready_utc"]) <= intervals[1][1]):
            raise ValueError("identity observations are outside their actual process")
        snapshots, transport_intervals = {}, list(intervals)
        for phase in ("before", "precase", "after"):
            snapshots[phase] = {}
            for name, command, marker in (("status", ["status"], "H2_LOADER_STATUS"),
                    ("wifi", ["wifi", "status"], "H2_LOADER_WIFI_STATUS"),
                    ("coredump", ["coredump", "status"], "H2_LOADER_COREDUMP_STATUS")):
                if (phase == "precase" and name == "coredump") or (phase == "before" and name == "wifi"): continue
                fact = device[phase][name]
                began, finished = process_interval(fact)
                transport_intervals.append((began, finished))
                if ((phase == "before" and finished > intervals[0][0])
                        or (phase == "precase" and not intervals[0][1] <= began <= finished <= intervals[1][0])
                        or (phase == "after" and began < intervals[1][1])):
                    raise ValueError("typed metadata belongs to the wrong capture phase")
                snapshots[phase][name] = typed_transport(fact, expected, command, marker)
        transport_intervals.sort()
        if any(a[1] > b[0] for a, b in zip(transport_intervals, transport_intervals[1:])):
            raise ValueError("UART command processes overlapped")
        names = ("valid", "package_checksum", "package_size", "image_checksum", "image_size", "role", "version", "board", "target")
        original_p1 = {k: snapshots["before"]["status"]["partition_1_" + k] for k in names}
        if original_p1["valid"] != "1" or original_p1["role"] != "loader":
            raise ValueError("original Loader/P1 identity is missing")
        partition = dict(valid="1", package_checksum=expected["package_checksum"],
            package_size=str(expected["package_size"]), image_checksum=expected["image_checksum"],
            image_size=str(expected["image_size"]), role="app", version=version,
            board=expected["board"], target=expected["target"])
        empty = dict(valid="0", package_checksum="-", package_size="0", image_checksum="-",
                     image_size="0", role="unknown", version="-", board="-", target="-")
        for phase, snapshot in snapshots.items():
            status = snapshot["status"]
            if (any(status[k] != expected[v] for k, v in (("device_uid", "uid"), ("board", "board"), ("target", "target")))
                    or {k: status["partition_1_" + k] for k in names} != original_p1):
                raise ValueError("device or original Loader/P1 changed")
            if phase != "before":
                active = dict(active_role="app", active_version=version, active_checksum=expected["image_checksum"],
                              active_image_size=str(expected["image_size"]), running_partition="2")
                if any(status[k] != v for k, v in active.items()):
                    raise ValueError("active App differs from frozen native image")
            stage = {k: status["stage_" + k] for k in names}
            if stage != (partition if phase == "precase" and stage["valid"] == "1" else empty):
                raise ValueError("unexpected or residual Stage")
        final = snapshots["after"]["status"]
        if final["last_result"] != "0" or {k: final["partition_2_" + k] for k in names} != partition:
            raise ValueError("final typed P2 package/image identity is not confirmed")
        for phase in ("precase", "after"):
            wifi_status = snapshots[phase]["wifi"]
            if any(wifi_status.get(k) != v for k, v in dict(result="OK", saved="0", saved_code="0", saved_ssid_hex="-").items()):
                raise ValueError("actual Saved=0 was not preserved")
        for phase in ("before", "after"):
            dump = snapshots[phase]["coredump"]
            if (set(dump) != {"result", "code", "partition", "bytes", "stored_bytes", "blank"}
                    or not re.fullmatch(r"[A-Za-z0-9_]+", dump["partition"])
                    or not re.fullmatch(r"[1-9][0-9]*", dump["bytes"])
                    or any(dump.get(k) != v for k, v in dict(result="OK", code="0", stored_bytes="0", blank="1").items())):
                raise ValueError("original blank coredump was not preserved")
        if snapshots["before"]["coredump"] != snapshots["after"]["coredump"]:
            raise ValueError("actual coredump status bytes/partition changed")
        return {"capture_schema": 3, "typed_device_valid": True}
    except (KeyError, TypeError, IndexError, AttributeError) as error:
        raise ValueError("missing typed two-process capture facts") from error


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
    capture_identity = {}
    if "schema" in capture:
        if type(capture["schema"]) is not int or capture["schema"] != 3:
            raise ValueError("unsupported capture schema")
        capture_identity = verify_temporary_capture(capture, raw_log, body, version, execution)
    else:
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
    return {**expected, **capture_identity, "valid": True, "chunks": chunks,
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
        if (line.startswith((b"H2_GIZCLAW_BOOT ", b"H2_GIZCLAW_PRECASE ")) or LAUNCHER.fullmatch(line)
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
