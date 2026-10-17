import unittest
import zlib
import json
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
from verify_device import extract, verify_ble_peer

PRODUCER = None
VERIFIER = None


def frame(body, nonce="1" * 32, *, admitted=1, confirm=0):
    crc = zlib.crc32(body)
    return (f"H2_GIZCLAW_LEDGER stage=begin version=unit-r1 execution={nonce} bytes={len(body)} records={len(body.splitlines())} crc32={crc:08x} admitted={admitted} confirm_rc={confirm} physical_audio=0\n".encode()
            + body + f"H2_GIZCLAW_LEDGER stage=end execution={nonce} crc32={crc:08x}\n".encode())


class LedgerAdmission(unittest.TestCase):
    def test_complete_single_frame_and_fresh_execution(self):
        body = b"H2_GIZCLAW_E2E stage=coverage-begin case=resource\n"
        result, identity = extract(frame(body).replace(b"\n", b"\r\n"), version="unit-r1")
        self.assertEqual(result, body)
        self.assertFalse(identity["physical_audio"])
        with self.assertRaises(ValueError):
            extract(frame(body), version="unit-r1", previous_execution="1" * 32)
        result, identity = extract(frame(body) + frame(body, "2" * 32), version="unit-r1", previous_execution="1" * 32)
        self.assertEqual(identity["execution"], "2" * 32)

    def test_partial_changed_unconfirmed_and_wrong_version_rejected(self):
        body = b"H2_GIZCLAW_E2E symbol=unit stage=unit result=PASS rc=0\n"
        for data in [frame(body)[:-20], frame(body).replace(b"symbol=unit", b"symbol=evil"), frame(body, admitted=0), frame(body, confirm=-7)]:
            with self.assertRaises(ValueError):
                extract(data, version="unit-r1")
        with self.assertRaises(ValueError):
            extract(frame(body), version="unit-r2")
        # A later complete replay is accepted on its own. No concatenation of
        # fragments from separate frames can supply missing original records.
        _, receipt = extract(frame(body).replace(b"symbol=unit", b"symbol=evil") + frame(body), version="unit-r1")
        self.assertEqual(receipt["discarded_frames"], 1)

    def test_latest_boot_cannot_borrow_earlier_success(self):
        body = b"H2_GIZCLAW_E2E symbol=unit stage=unit result=PASS rc=0\n"
        good = frame(body)
        second = frame(body, "2" * 32)
        boot = f"H2_GIZCLAW_BOOT board=devkit version=unit-r1 execution={'2' * 32}\n".encode()
        for later in (frame(body, "2" * 32, admitted=0),
                      frame(body, "2" * 32, confirm=-7), second[:-20],
                      second.replace(b"symbol=unit", b"symbol=evil"),
                      second.replace(b"version=unit-r1", b"version=unit-r2"),
                      boot, boot + good,
                      b"H2_GIZCLAW_E2E_AMOLED stage=launcher status=READY\n",
                      b"H2_GIZCLAW_SETUP_FAIL stage=memory rc=-5\n"):
            with self.subTest(later=later), self.assertRaises(ValueError):
                extract(good + later, version="unit-r1")
        _, receipt = extract(good + boot + second, version="unit-r1")
        self.assertEqual(receipt["execution"], "2" * 32)

    def test_latest_complete_frame_and_same_boot_replay(self):
        body = b"H2_GIZCLAW_E2E symbol=unit stage=unit result=PASS rc=0\n"
        # An identical partial replay is the same frozen execution, not a boot.
        _, receipt = extract(frame(body) + frame(body)[:-20], version="unit-r1")
        self.assertEqual(receipt["execution"], "1" * 32)
        _, receipt = extract(frame(body) + frame(body, "2" * 32), version="unit-r1")
        self.assertEqual(receipt["execution"], "2" * 32)
        with self.assertRaises(ValueError):
            extract(frame(body) + frame(body).replace(b"symbol=unit", b"symbol=evil"), version="unit-r1")

    def test_amoled_reboot_before_ready_invalidates_previous_ledger(self):
        body = b"H2_GIZCLAW_E2E symbol=unit stage=unit result=PASS rc=0\n"
        good = frame(body)
        boot = b"H2_GIZCLAW_BOOT platform=amoled reset_reason=3\n"
        fresh = frame(body, "2" * 32)
        for later in (boot,
                      boot + b"H2_BOARD_ENTRY_FAIL board=amoled code=-5\n",
                      boot + fresh[:-20],
                      boot + good,
                      boot + b"H2_GIZCLAW_E2E_AMOLED stage=launcher status=READY\n" + good):
            with self.subTest(later=later), self.assertRaises(ValueError):
                extract(good + later, version="unit-r1")
        _, receipt = extract(good + boot + fresh, version="unit-r1")
        self.assertEqual(receipt["execution"], "2" * 32)

    def test_unknown_boot_marker_is_a_fail_closed_boundary(self):
        body = b"H2_GIZCLAW_E2E symbol=unit stage=unit result=PASS rc=0\n"
        for marker in (b"H2_GIZCLAW_BOOT platform=amoled reset_reason=unknown\n",
                       b"H2_GIZCLAW_BOOT board=devkit version=broken\n"):
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                extract(frame(body) + marker + frame(body), version="unit-r1")

    def test_changed_valid_replay_poisoning_is_permanent_for_that_boot(self):
        body = b"H2_GIZCLAW_E2E symbol=unit stage=unit result=PASS rc=0\n"
        changed = b"H2_GIZCLAW_E2E symbol=changed stage=unit result=PASS rc=0\n"
        # Both frames are independently valid and admitted, but a frozen boot
        # cannot change either its content or header and later recover itself.
        for log in (frame(body) + frame(changed),
                    frame(body) + frame(changed) + frame(body)):
            with self.assertRaises(ValueError):
                extract(log, version="unit-r1")
        _, receipt = extract(frame(body) + frame(changed) + frame(body, "2" * 32), version="unit-r1")
        self.assertEqual(receipt["execution"], "2" * 32)


class BlePeerAdmission(unittest.TestCase):
    def setUp(self):
        self.body = ("H2_GIZCLAW_E2E stage=ble-binding-ready window=h2e2e-" + "a" * 16
                     + " revision=42 url_bytes=112 url_sha256=" + "b" * 64 + "\n").encode()
        self.execution = "1" * 32
        self.raw = ("H2_GIZCLAW_BOOT board=zero_esp_v3_0 version=unit-r1 execution="
                    + self.execution + "\n").encode() + frame(self.body)
        self.peer = dict(schema=2, backend="darwin-pal", version="unit-r1", execution=self.execution,
                         window="h2e2e-" + "a" * 16, timeout_ms=120000, elapsed_ms=20000,
                         revision=42, url_bytes=112, url_sha256="b" * 64,
                         completed_connections=2, rediscovered=True, rc=0,
                         qualified=True, chunks=[14, 14],
                         started_at_utc="2020-01-01T00:00:02.000000000Z",
                         finished_at_utc="2020-01-01T00:00:22.000000000Z")
        self.capture = dict(argv=["/unit/h2loader", "reboot", "app", "--monitor"],
                            version="unit-r1", execution=self.execution,
                            started_utc="2020-01-01T00:00:00Z",
                            boot_observed_utc="2020-01-01T00:00:00.5Z",
                            ble_ready_utc="2020-01-01T00:00:01Z",
                            finished_utc="2020-01-01T00:00:30Z", exit_code=130,
                            monitor_stop="complete-qualified-ledger",
                            sha256=hashlib.sha256(self.raw).hexdigest())
        self.process = dict(argv=["/unit/ble_peer", "--version", "unit-r1",
                                 "--execution", self.execution, "--window", "h2e2e-" + "a" * 16,
                                 "--revision", "42", "--size", "112", "--sha256", "b" * 64,
                                 "--output", "/unit/peer.json"], pid=345, exit_code=0,
                            started_utc="2020-01-01T00:00:01.5Z",
                            finished_utc="2020-01-01T00:00:23.5Z", elapsed_ms=22000)

    def verify(self, peer=None, body=None, exit_code=0, capture=None, process=None,
               version="unit-r1", execution=None, data=None, raw_log=None):
        data = json.dumps(self.peer if peer is None else peer).encode() if data is None else data
        command = {**self.process, "receipt_sha256": hashlib.sha256(data).hexdigest()} if process is None else process
        return verify_ble_peer(self.body if body is None else body,
                               data, version=version, execution=execution or self.execution,
                               process_exit_code=exit_code, raw_log=self.raw if raw_log is None else raw_log,
                               capture_data=json.dumps(self.capture if capture is None else capture).encode(),
                               process_data=json.dumps(command).encode())

    def test_complete_real_peer_contract_matches_one_device_window(self):
        receipt = self.verify()
        self.assertTrue(receipt["valid"])
        self.assertEqual(receipt["completed_connections"], 2)
        self.assertEqual(len(receipt["receipt_sha256"]), 64)

    def test_actual_producer_serializer_and_valid_historical_facts(self):
        self.assertIsNotNone(PRODUCER, "Bazel must supply the compiled producer serializer")
        result = subprocess.run([str(PRODUCER)], capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), self.peer)
        # These actual serialized 2020 timestamps stay valid independent of
        # today's date when their historical collector facts also match.
        self.assertTrue(self.verify(data=result.stdout)["valid"])

    def test_missing_stale_partial_failed_or_different_peer_rejected(self):
        for change in (dict(schema=True), dict(window="h2e2e-" + "c" * 16),
                       dict(version="unit-r2"), dict(execution="2" * 32),
                       dict(revision=41), dict(url_bytes=111), dict(url_sha256="c" * 64),
                       dict(completed_connections=1), dict(rediscovered=False),
                       dict(qualified=False), dict(rc=1), dict(rc=False),
                       dict(chunks=[14]), dict(chunks=[0, 14]), dict(chunks=[True, 14]),
                       dict(started_at_utc="2020-01-01T00:00:02"),
                       dict(started_at_utc="2019-12-31T23:59:59Z"),
                       dict(finished_at_utc="2030-01-01T00:00:22Z"),
                       dict(finished_at_utc="2019-12-31T23:00:00Z"),
                       dict(elapsed_ms=120000), dict(timeout_ms=240000),
                       dict(elapsed_ms=10)):
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.verify(peer={**self.peer, **change})
        for body in (b"", self.body + self.body):
            with self.assertRaises(ValueError): self.verify(body=body)
        with self.assertRaises(ValueError): self.verify(exit_code=1)
        with self.assertRaises(ValueError): self.verify(exit_code=False)
        with self.assertRaises(ValueError): self.verify(peer=[])

    def test_independent_process_and_capture_facts_are_mandatory(self):
        process = {**self.process, "receipt_sha256": hashlib.sha256(json.dumps(self.peer).encode()).hexdigest()}
        for change in (dict(pid=0), dict(exit_code=1), dict(exit_code=False),
                       dict(receipt_sha256="0" * 64), dict(elapsed_ms=1),
                       dict(finished_utc="2020-01-01T00:00:21Z"),
                       dict(started_utc="2020-01-01T00:00:03Z"),
                       dict(argv=["/unit/echo", *process["argv"][1:]])):
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.verify(process={**process, **change})
        for flag in ("--version", "--execution", "--window", "--revision", "--size", "--sha256"):
            argv = list(process["argv"]); argv[argv.index(flag) + 1] = "wrong"
            with self.subTest(flag=flag), self.assertRaises(ValueError):
                self.verify(process={**process, "argv": argv})
        for change in (dict(execution="2" * 32), dict(version="unit-r2"),
                       dict(sha256="0" * 64), dict(exit_code=1),
                       dict(monitor_stop="host-deadline"),
                       dict(boot_observed_utc="2020-01-01T00:00:02Z"),
                       dict(ble_ready_utc="2020-01-01T00:00:03Z"),
                       dict(finished_utc="2020-01-01T00:00:23Z"), dict(argv=[])):
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.verify(capture={**self.capture, **change})
        for key in ("started_utc", "finished_utc", "boot_observed_utc", "ble_ready_utc"):
            capture = dict(self.capture); del capture[key]
            with self.subTest(missing=key), self.assertRaises(ValueError): self.verify(capture=capture)
        with self.assertRaises(ValueError): self.verify(capture={})
        with self.assertRaises(ValueError): self.verify(process={})
        with self.assertRaises(ValueError): self.verify(execution="2" * 32)
        with self.assertRaises(ValueError): self.verify(raw_log=self.raw + b"changed")
        with self.assertRaises(ValueError):
            verify_ble_peer(self.body, json.dumps(self.peer).encode(), version="unit-r1",
                            execution=self.execution, process_exit_code=0)

    def test_excessive_duration_rejected_even_inside_matching_capture(self):
        peer = {**self.peer, "finished_at_utc": "2020-01-01T00:02:03Z", "elapsed_ms": 121000}
        data = json.dumps(peer).encode()
        process = {**self.process, "finished_utc": "2020-01-01T00:02:04Z", "elapsed_ms": 122500,
                   "receipt_sha256": hashlib.sha256(data).hexdigest()}
        capture = {**self.capture, "finished_utc": "2020-01-01T00:02:05Z"}
        with self.assertRaises(ValueError): self.verify(data=data, process=process, capture=capture)


def temporary_bundle(base, body):
    """Protocol fixture only; never physical capture or device qualification."""
    expected = dict(board="zero_esp_v3_0", target="esp32s3", uid="7c4fadbc2cea",
                    port="/unit/zero", cli_sha256="e" * 64,
                    package_checksum="d" * 64, package_size=100,
                    image_checksum="c" * 64, image_size=200)
    prefix = ["/unit/h2loader", "--no-ble", "--port", expected["port"],
              "--transport", "iostreamikcp", "--read-timeout", "1"]
    first = ("H2_LOADER_REBOOT target=upgrade result=accepted\n"
             "H2_GIZCLAW_PRECASE board=zero_esp_v3_0 version=unit-r1 execution="
             + base.execution + " state=precase\n"
             "H2_GIZCLAW_E2E_DEVKIT stage=launcher status=READY\n"
             "H2_GIZCLAW_E2E_DEVKIT stage=wifi status=NO_SAVED_WIFI rc=-7 retry_ms=10000\n").encode()
    second = ("H2_LOADER_WIFI_TEMPORARY result=connected ssid=fixture\n"
              "H2_GIZCLAW_E2E_DEVKIT stage=time status=READY\n"
              "H2_GIZCLAW_BOOT board=zero_esp_v3_0 version=unit-r1 execution="
              + base.execution + "\n").encode() + next(
                  line + b"\n" for line in body.splitlines()
                  if line.startswith(b"H2_GIZCLAW_E2E stage=ble-binding-ready ")) + frame(body)
    raw = first + second
    capture = {**base.capture, "schema": 3, "sha256": hashlib.sha256(raw).hexdigest(),
               "boot_observed_utc": "2020-01-01T00:00:00.8Z",
               "precase_observed_utc": "2020-01-01T00:00:00.2Z"}
    del capture["argv"]
    def process(argv, pid, start, finish, elapsed, output, offset=0):
        return dict(argv=argv, pid=pid, started_utc=start, finished_utc=finish,
                    elapsed_ms=elapsed, exit_code=130, executable_sha256="e" * 64,
                    offset=offset, bytes=len(output), sha256=hashlib.sha256(output).hexdigest())
    capture["segments"] = [
        process(prefix + ["reboot", "upgrade", "--monitor"], 100,
                capture["started_utc"], "2020-01-01T00:00:00.5Z", 500, first),
        process(prefix + ["wifi", "connect", "fixture", "fixture-password", "--temporary", "--monitor"], 101,
                "2020-01-01T00:00:00.7Z", capture["finished_utc"], 29300, second, len(first))]
    capture["segments"][0]["monitor_stop"] = "precase-awaiting-temporary-wifi"
    capture["segments"][1]["monitor_stop"] = "complete-qualified-ledger"
    def metadata(phase):
        parts = dict(board=expected["board"], target="esp32s3", device_uid=expected["uid"],
                     active_role="app", active_version="unit-r1" if phase != "before" else "old-r1",
                     active_checksum="c" * 64 if phase != "before" else "f" * 64,
                     active_image_size="200", running_partition="2", last_result="0")
        p1 = dict(valid="1", package_checksum="a" * 64, package_size="50",
                  image_checksum="b" * 64, image_size="60", role="loader",
                  version="loader-r1", board=expected["board"], target="esp32s3")
        p2 = dict(valid="1", package_checksum="d" * 64, package_size="100",
                  image_checksum=parts["active_checksum"], image_size="200", role="app",
                  version=parts["active_version"], board=expected["board"], target="esp32s3")
        stage = dict(valid="0", package_checksum="-", package_size="0", image_checksum="-",
                     image_size="0", role="unknown", version="-", board="-", target="-")
        if phase == "precase": stage = {**p2}
        for label, values in [("partition_1", p1), ("partition_2", p2), ("stage", stage)]:
            parts.update({label + "_" + k: v for k, v in values.items()})
        return "H2_LOADER_STATUS " + " ".join(k + "=" + v for k, v in parts.items()) + "\n"
    def fact(tail, output, index, phase):
        from datetime import datetime, timedelta, timezone
        origin = datetime(2019, 12, 31, 23, 59, 50, tzinfo=timezone.utc)
        if phase == "precase": origin = datetime(2020, 1, 1, 0, 0, 0, 525000, tzinfo=timezone.utc)
        if phase == "after": origin = datetime(2020, 1, 1, 0, 0, 31, tzinfo=timezone.utc)
        start = origin + timedelta(milliseconds=index * 100)
        return dict(argv=prefix + tail, pid=200 + index + (10 if phase == "after" else 0),
                    exit_code=0, executable_sha256="e" * 64, stdout=output,
                    sha256=hashlib.sha256(output.encode()).hexdigest(), elapsed_ms=50,
                    started_utc=start.isoformat(), finished_utc=(start + timedelta(milliseconds=50)).isoformat())
    device = {"expected": expected}
    for phase in ["before", "precase", "after"]:
        group = {"status": fact(["status"], metadata(phase), 0, phase)}
        if phase != "before":
            wifi = "H2_LOADER_WIFI_STATUS result=OK saved=0 saved_code=0 saved_ssid_hex=-\n"
            group["wifi"] = fact(["wifi", "status"], wifi, 1, phase)
        if phase != "precase":
            dump = "H2_LOADER_COREDUMP_STATUS result=OK code=0 partition=coredump bytes=65536 stored_bytes=0 blank=1\n"
            group["coredump"] = fact(["coredump", "status"], dump, 2, phase)
        device[phase] = group
    capture["device"] = device
    return raw, capture


class TemporaryWifiCapture(unittest.TestCase):
    verify = BlePeerAdmission.verify

    def setUp(self):
        BlePeerAdmission.setUp(self)
        self.raw, self.capture = temporary_bundle(self, self.body)

    def test_actual_split_capture_and_typed_device_gate(self):
        self.assertTrue(self.verify()["valid"])

    def reseal(self, first=None, second=None):
        cut = self.capture["segments"][0]["bytes"]
        chunks = [self.raw[:cut] if first is None else first,
                  self.raw[cut:] if second is None else second]
        raw = b"".join(chunks)
        capture = json.loads(json.dumps(self.capture))
        offset = 0
        for segment, chunk in zip(capture["segments"], chunks):
            segment.update(offset=offset, bytes=len(chunk), sha256=hashlib.sha256(chunk).hexdigest())
            offset += len(chunk)
        capture["sha256"] = hashlib.sha256(raw).hexdigest()
        return raw, capture

    def test_old_frozen_replay_before_actual_reboot_ack_cannot_supply_current_cases(self):
        cut=self.capture["segments"][0]["bytes"]
        raw,capture=self.reseal(first=frame(self.body, "3"*32)+self.raw[:cut])
        self.assertTrue(self.verify(raw_log=raw,capture=capture)["valid"])

    def test_real_process_and_segment_integrity_are_required(self):
        for key, value in [("pid", 0), ("exit_code", -9), ("elapsed_ms", 900000),
                           ("offset", True), ("sha256", "0" * 64), ("executable_sha256", "0" * 64),
                           ("monitor_stop", "host-deadline")]:
            for index in range(2):
                capture = json.loads(json.dumps(self.capture))
                capture["segments"][index][key] = value
                with self.subTest(index=index, key=key), self.assertRaises(ValueError): self.verify(capture=capture)
        for change in (dict(schema=True), dict(schema=2), dict(segments=[]), dict(device={}),
                       dict(precase_observed_utc="2020-01-01T00:00:00.6Z"),
                       dict(boot_observed_utc="2020-01-01T00:00:00.6Z")):
            with self.subTest(change=change), self.assertRaises(ValueError): self.verify(capture={**self.capture, **change})
        for index, old, new in [(0, "upgrade", "loader"), (1, "--temporary", "--save"),
                                (0, "--no-ble", "--ble"), (1, "/unit/zero", "/unit/another"),
                                (1, "iostreamikcp", "bleikcp"), (1, "--monitor", "--save")]:
            capture = json.loads(json.dumps(self.capture));argv=capture["segments"][index]["argv"]
            argv[argv.index(old)] = new
            with self.subTest(index=index, old=old), self.assertRaises(ValueError): self.verify(capture=capture)
        capture = json.loads(json.dumps(self.capture));capture["segments"][1]["pid"] = capture["segments"][0]["pid"]
        with self.assertRaises(ValueError): self.verify(capture=capture)

    def test_rehashed_logs_still_require_real_preferred_boot_and_first_ledger(self):
        cut = self.capture["segments"][0]["bytes"];first, second=self.raw[:cut],self.raw[cut:]
        bad_first = [first.replace(b"result=accepted", b"result=fail"),
                     first.replace(b"H2_GIZCLAW_PRECASE", b"LOST_PRECASE"),
                     first.replace(b"state=precase", b"state=running"),
                     first.replace(self.execution.encode(), b"2" * 32),
                     first + b"H2_GIZCLAW_E2E stage=coverage-begin case=resource\n",
                     first + b"Rebooting...\n"]
        bad_second = [second.replace(b"H2_LOADER_WIFI_TEMPORARY", b"H2_LOADER_WIFI"),
                      second.replace(self.execution.encode(), b"2" * 32),
                      b"H2_GIZCLAW_E2E stage=coverage-begin case=resource\n" + second,
                      b"rst:0x1 (POWERON)\n" + second,
                      b"H2_GIZCLAW_SETUP_FAIL stage=clock rc=-4\n" + second,
                      second.replace(self.body, b"", 1),
                      second.replace(frame(self.body), frame(self.body, admitted=0) + frame(self.body))]
        for first_value in bad_first:
            raw,capture=self.reseal(first=first_value)
            with self.subTest(first=first_value[:50]), self.assertRaises(ValueError): self.verify(raw_log=raw,capture=capture)
        for second_value in bad_second:
            raw,capture=self.reseal(second=second_value)
            with self.subTest(second=second_value[:50]), self.assertRaises(ValueError): self.verify(raw_log=raw,capture=capture)

    def test_typed_device_receipts_cannot_replace_or_relax_hardware_gates(self):
        corruptions = [("after", "status", "device_uid=7c4fadbc2cea", "device_uid=000000000000"),
            ("after", "status", "partition_1_package_checksum=" + "a" * 64, "partition_1_package_checksum=" + "f" * 64),
            ("after", "status", "partition_2_package_checksum=" + "d" * 64, "partition_2_package_checksum=" + "f" * 64),
            ("precase", "status", "active_checksum=" + "c" * 64, "active_checksum=" + "f" * 64),
            ("after", "status", "last_result=0", "last_result=-7"),
            ("after", "status", "stage_valid=0", "stage_valid=1"),
            ("precase", "wifi", "saved=0", "saved=error"),
            ("after", "wifi", "saved=0", "saved=1"),
            ("after", "coredump", "stored_bytes=0", "stored_bytes=16"),
            ("after", "coredump", "blank=1", "blank=0")]
        for phase, name, old, new in corruptions:
            capture=json.loads(json.dumps(self.capture));fact=capture["device"][phase][name]
            self.assertIn(old, fact["stdout"]);fact["stdout"]=fact["stdout"].replace(old,new)
            fact["sha256"]=hashlib.sha256(fact["stdout"].encode()).hexdigest()
            with self.subTest(phase=phase,name=name,old=old), self.assertRaises(ValueError): self.verify(capture=capture)
        for phase,name in [("before","status"),("precase","wifi"),("after","coredump")]:
            for key,value in [("pid",0),("exit_code",1),("sha256","0"*64),
                              ("started_utc","2020-01-01T00:00:01Z")]:
                capture=json.loads(json.dumps(self.capture));capture["device"][phase][name][key]=value
                with self.subTest(phase=phase,name=name,key=key), self.assertRaises(ValueError): self.verify(capture=capture)

    def test_matching_blank_dumps_require_actual_partition_and_capacity(self):
        for old, new in [(" partition=coredump", ""), (" bytes=65536", ""),
                         (" partition=coredump bytes=65536", ""),
                         ("partition=coredump", "partition=-"),
                         ("bytes=65536", "bytes=0"), ("bytes=65536", "bytes=-1"),
                         ("bytes=65536", "bytes=65536.0"),
                         ("bytes=65536", "bytes=unknown")]:
            capture = json.loads(json.dumps(self.capture))
            for phase in ("before", "after"):
                fact = capture["device"][phase]["coredump"]
                self.assertIn(old, fact["stdout"])
                fact["stdout"] = fact["stdout"].replace(old, new)
                fact["sha256"] = hashlib.sha256(fact["stdout"].encode()).hexdigest()
            with self.subTest(old=old, new=new), self.assertRaises(ValueError):
                self.verify(capture=capture)


class VerifierCliAdmission(unittest.TestCase):
    """Synthetic whole-CLI regressions; these never qualify physical hardware."""

    def setUp(self):
        BlePeerAdmission.setUp(self)
        from projects.e2e.apps.gizclaw import api_coverage as coverage
        lines = []

        def record(**fields):
            lines.append("H2_GIZCLAW_E2E " + " ".join(f"{key}={value}" for key, value in fields.items()) + "\n")

        def case(name):
            record(stage="coverage-begin", case=name)
            if name == "device-api":
                lines.append(self.body.decode())
            for rule in coverage.requirements():
                if rule.case == name:
                    for symbol in rule.calls:
                        record(symbol=symbol, stage="call", result="PASS", rc=0)
                    record(symbol=rule.assertion_symbol, stage=rule.assertion_stage, result="PASS", rc=0)
            if name == "rpc":
                for domain in sorted(coverage.RPC_CASES): case("rpc/" + domain)
            record(stage="coverage-end", case=name, status="PASS", rc=0, cleanup_rc=0)

        for name in sorted(coverage.TOP_CASES): case(name)
        record(stage="summary", endpoint="edge-bj-01.e2e.gizclaw.com:9821", backend="h2peer",
               profile="default", platform="zero_esp_v3_0", suite="all", selected=8,
               terminal=8, **{"pass": 8}, fail=0, error=0, blocked=0, cancelled=0,
               first_failure_case="-", first_failure_rc=0, cleanup_rc=0,
               retained_resources=0, complete="true", exit_code=0)
        self.body = "".join(lines).encode()

    def invoke(self, *, ledger_execution="1" * 32, launcher=False, previous=None, old_236=False, temporary=False):
        self.assertIsNotNone(VERIFIER)
        peer = subprocess.run([str(PRODUCER)], capture_output=True, check=True).stdout
        raw = ("H2_GIZCLAW_BOOT board=zero_esp_v3_0 version=unit-r1 execution="
               + self.execution + "\n").encode()
        if launcher: raw += b"H2_GIZCLAW_E2E_DEVKIT stage=launcher status=READY\n"
        body = self.body
        if old_236:
            body = b"".join(line for line in body.splitlines(keepends=True)
                            if b"symbol=h2_gizclaw_session_refresh_with_tags " not in line)
        raw += frame(body, ledger_execution)
        capture = {**self.capture, "sha256": hashlib.sha256(raw).hexdigest()}
        if temporary: raw, capture = temporary_bundle(self, body)
        process = {**self.process, "receipt_sha256": hashlib.sha256(peer).hexdigest()}
        with tempfile.TemporaryDirectory(prefix="gizclaw-verifier-cli-") as temporary:
            directory = Path(temporary)
            for name, data in {"uart.log": raw, "peer.json": peer,
                               "capture.json": json.dumps(capture).encode(),
                               "process.json": json.dumps(process).encode()}.items():
                (directory / name).write_bytes(data)
            command = [str(VERIFIER), "--log", str(directory / "uart.log"),
                       "--version", "unit-r1", "--execution", self.execution,
                       "--ble-peer-receipt", str(directory / "peer.json"), "--ble-peer-exit-code", "0",
                       "--capture-receipt", str(directory / "capture.json"),
                       "--ble-peer-command-receipt", str(directory / "process.json"),
                       "--endpoint", "edge-bj-01.e2e.gizclaw.com:9821", "--profile", "default",
                       "--platform", "zero_esp_v3_0", "--output", str(directory / "result.json")]
            if previous: command.extend(["--previous-execution", previous])
            result = subprocess.run(command, capture_output=True, check=False)
            path = directory / "result.json"
            report = json.loads(path.read_bytes()) if path.exists() else None
            return result, report

    def test_same_boot_complete_historical_bundle_passes_real_cli(self):
        result, report = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(report["qualified"])
        self.assertTrue(report["wrapper_observed"])
        self.assertTrue(report["api_coverage"]["valid"])
        self.assertEqual(report["api_coverage"]["covered"], 237)
        self.assertEqual(report["execution"], report["ble_binding_peer"]["execution"])

    def test_temporary_wifi_whole_cli_retains_all_237_rules_and_previous_boot_gate(self):
        result, report = self.invoke(temporary=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(report["qualified"])
        self.assertTrue(report["ble_binding_peer"]["typed_device_valid"])
        self.assertEqual(report["api_coverage"]["covered"], 237)
        result, report = self.invoke(temporary=True, old_236=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(report["qualified"])
        result, report = self.invoke(temporary=True, previous=self.execution)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(report is None or not report["qualified"])

    def test_complete_historical_236_bundle_does_not_qualify_new_inventory(self):
        result, report = self.invoke(old_236=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(report["qualified"])
        self.assertTrue(report["ble_binding_peer"]["valid"])
        audit = report["api_coverage"]
        self.assertFalse(audit["valid"])
        self.assertEqual((audit["required"], audit["covered"], audit["missing"]), (237, 236, 1))
        self.assertEqual({row["symbol"] for row in audit["functions"] if row["status"] == "missing"},
                         {"h2_gizclaw_session_refresh_with_tags"})

    def test_later_launcher_and_lost_wrapper_cannot_bind_another_ledger_to_old_boot(self):
        result, report = self.invoke(launcher=True, ledger_execution="2" * 32)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(report is None or not report["qualified"])

    def test_later_launcher_and_lost_wrapper_cannot_borrow_old_nonce_replay(self):
        result, report = self.invoke(launcher=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(report is None or not report["qualified"])

    def test_previous_execution_replay_is_rejected_by_real_cli(self):
        result, report = self.invoke(previous=self.execution)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(report is None or not report["qualified"])


if __name__ == "__main__":
    PRODUCER = Path(sys.argv.pop(1))
    VERIFIER = Path(sys.argv.pop(1))
    unittest.main()
