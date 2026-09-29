import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path("projects/e2e/apps/pal-wifi").resolve()))
from evidence_validator import verify_boot


class LedgerRejection(unittest.TestCase):
    def setUp(self):
        self.ids = ["case-a", "restore-original-settings-network"]
        key = dict(boot=2, nonce=99)
        self.rows = [
            ("BOOT", dict(key, board="devkit", version="v", persistence=1)),
            ("CASE", dict(key, id="settings-restart-persistence", status="PASS", rc=0)),
            *[("CASE", dict(key, id=id, status="PASS", rc=0)) for id in self.ids],
            ("REPORT", dict(key, board="devkit", version="v", operations=21, passed=3, failed=0,
                            blocked=0, persistence=1, qualified=1, rc=0)),
            ("RESTORE", dict(key, cleanup=0, saved_restored=1, network_restored=1,
                             backup_cleared=1, retained=0)),
            ("STA_EVENTS", dict(key, connecting=1, connected=1, got_ip=1, lost_ip=1, disconnected=1,
                                route_changed=1, invalid=0)),
            ("AP_EVENTS", dict(key, started=1, stopped=1, joined=3, left=3,
                               lease_granted=3, lease_released=3)),
            ("CLIENT", dict(key, mac="020000000001", ip4=3232236546)),
        ]

    def verify(self, rows):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"boot.log"
            path.write_text("\n".join("H2_WIFI_"+tag+" "+json.dumps(row)
                                      for tag, row in rows)
                            +"\nH2_WIFI_READY board=devkit rc=0 confirm=0\n", encoding="utf-8")
            return verify_boot(path, self.ids, "devkit", "v")

    def test_valid(self):
        self.verify(self.rows)

    def test_partial_duplicate_replay_and_failure(self):
        variants = [self.rows[:2]+self.rows[3:], self.rows+self.rows[:1]]
        for index, field, value in ((2,"nonce",88),(2,"status","BLOCKED"),
                                    (4,"qualified",0),(5,"network_restored",0),
                                    (5,"retained",1),(5,"backup_cleared",0),
                                    (6,"lost_ip",0),(7,"lease_granted",2),
                                    (7,"lease_released",0)):
            rows = [(tag,dict(row)) for tag,row in self.rows]
            rows[index][1][field] = value
            variants.append(rows)
        for rows in variants:
            with self.assertRaises(AssertionError): self.verify(rows)


if __name__ == "__main__": unittest.main()
