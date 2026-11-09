"""Exercise managed stage control against real SQLite across fresh processes."""
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import tempfile


binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="pal-storage-device-", dir=os.environ.get("TEST_TMPDIR")) as directory:
    root = Path(directory)
    (root / "files").mkdir()
    # A contract 1 completion marker for the same image must reseed, rather
    # than being accepted as completion of the strengthened contract.
    with sqlite3.connect(root / "preferences.sqlite") as database:
        database.execute("CREATE TABLE h2_pref (namespace TEXT NOT NULL, key TEXT NOT NULL, "
                         "type INTEGER NOT NULL, value BLOB NOT NULL, updated_at_ms INTEGER NOT NULL, "
                         "PRIMARY KEY(namespace, key))")
        database.executemany("INSERT INTO h2_pref VALUES ('h2storectl', ?, ?, ?, 0)",
                             (("version", 2, b"storage-contract-test"),
                              ("phase", 3, (3).to_bytes(4, "little")),
                              ("nonce", 3, (42).to_bytes(4, "little"))))

    def launch():
        process = subprocess.run([binary, "1", root, "42", "storage-contract-test"],
                                 capture_output=True, text=True, timeout=180)
        print(process.stdout, end="")
        exit_state = [json.loads(line.removeprefix("H2_STORAGE_DEVICE_EXIT "))
                      for line in process.stdout.splitlines() if line.startswith("H2_STORAGE_DEVICE_EXIT ")]
        assert len(exit_state) == 1 and exit_state[0]["balanced"], process.stdout + process.stderr
        return process, exit_state[0]["pid"]

    pids = set()
    nonces = set()
    for phase, count in ((1, 31), (2, 3), (3, 2), (4, 0), (4, 0)):
        process, pid = launch()
        assert process.returncode == 0 and pid not in pids, process.stdout + process.stderr
        pids.add(pid)
        boot = re.search(rf"H2_STORAGE_BOOT contract=2 version=storage-contract-test phase={phase} nonce=(\d+)", process.stdout)
        assert boot, process.stdout
        nonces.add(int(boot.group(1)))
        assert len(nonces) == 1
        cases = [json.loads(line.removeprefix("H2_STORAGE_CASE "))
                 for line in process.stdout.splitlines() if line.startswith("H2_STORAGE_CASE ")]
        assert len(cases) == count * 2 and cases[:count] == cases[count:], cases
        assert all(row["status"] == "PASS" and row["rc"] == 0 and row["phase"] == phase for row in cases)
        if phase == 4:
            assert "H2_STORAGE_ALREADY_COMPLETE no_new_run=1 empty=1 rc=0" in process.stdout
            assert "H2_STORAGE_PHASE " not in process.stdout
        else:
            state = [json.loads(line.removeprefix("H2_STORAGE_PHASE "))
                     for line in process.stdout.splitlines() if line.startswith("H2_STORAGE_PHASE ")][0]
            assert state["contract"] == 2 and state["passed"] == count
            assert state["failed"] == state["blocked"] == state["cleanup"] == state["rc"] == state["control"] == 0
    with sqlite3.connect(root / "preferences.sqlite") as database:
        database.execute("INSERT INTO h2_pref VALUES ('h2storea', 'counter', 3, ?, 0)", ((999).to_bytes(4, "little"),))
    process, _ = launch()
    assert process.returncode != 0
    assert "H2_STORAGE_ALREADY_COMPLETE no_new_run=1 empty=0" in process.stdout
    assert "H2_STORAGE_CASE " not in process.stdout
    print("PAL_STORAGE_DEVICE_LIFECYCLE fresh phases=1/2/3/4/4 PASS; completed-state leftovers rejected")
