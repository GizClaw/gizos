import unittest

from verify import verify

REGISTRY = '''H2_PAL_DISK_CASE(PARTITIONS, "pal.disk.partitions")
H2_PAL_DISK_CASE(SEQUENTIAL, "pal.disk.sequential-erase-write-read")
H2_PAL_DISK_CASE(CONCURRENT, "pal.disk.concurrent-read-write")
H2_PAL_DISK_CASE(CLEANUP, "pal.disk.cleanup")
'''


def ledger():
    values = ("rc=0 cleanup_rc=0 output_rc=0 passed=4 failed=0 blocked=0 "
              "writer_rounds=512 reader_rounds=1000 writer_stage=4 reader_stage=4 "
              "cores=1,2 cores_verified=1 workers_joined=1")
    rows = []
    for phase in ("boot", "management_ready", "region", "sequential", "concurrent_begin"):
        rows.append(f"H2_PAL_DISK_E2E contract=1 phase={phase} {values}")
    for name in ("partitions", "sequential-erase-write-read", "concurrent-read-write", "cleanup"):
        rows.append(f"H2_PAL_DISK_CASE id=pal.disk.{name} status=PASS rc=0")
    for phase in ("result", "run_returned", "release"):
        rows.append(f"H2_PAL_DISK_E2E contract=1 phase={phase} {values}")
    return "\n".join(rows) + "\n"


class VerifyTest(unittest.TestCase):
    def test_complete(self):
        result = verify(ledger(), REGISTRY, 512, [1, 2])
        self.assertTrue(result["passed"])
        self.assertEqual(len(result["cases"]), 4)

    def test_missing_case_or_terminal(self):
        for fragment in ("id=pal.disk.cleanup", "phase=result", "phase=release"):
            text = "\n".join(line for line in ledger().splitlines() if fragment not in line)
            with self.assertRaises(AssertionError):
                verify(text, REGISTRY, 512, [1, 2])

    def test_failed_or_weakened_result(self):
        for old, new in [
            ("status=PASS", "status=BLOCKED"),
            ("cleanup_rc=0", "cleanup_rc=-1"),
            ("writer_rounds=512", "writer_rounds=511"),
            ("reader_rounds=1000", "reader_rounds=0"),
            ("cores=1,2", "cores=1,1"),
            ("cores_verified=1", "cores_verified=0"),
            ("workers_joined=1", "workers_joined=0"),
        ]:
            with self.subTest(new=new), self.assertRaises(AssertionError):
                verify(ledger().replace(old, new), REGISTRY, 512, [1, 2])

    def test_extra_boot_fault_duplicate_and_changed_receipt(self):
        samples = [
            ledger() + ledger().splitlines()[0] + "\n",
            ledger() + "CP MemFault\n",
            ledger().replace("id=pal.disk.cleanup", "id=pal.disk.partitions"),
            ledger().replace("phase=release rc=0", "phase=release rc=-1"),
        ]
        for text in samples:
            with self.assertRaises(AssertionError):
                verify(text, REGISTRY, 512, [1, 2])


if __name__ == "__main__":
    unittest.main()
