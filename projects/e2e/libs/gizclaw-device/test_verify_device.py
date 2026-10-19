import unittest
import zlib
from verify_device import extract


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


if __name__ == "__main__":
    unittest.main()
