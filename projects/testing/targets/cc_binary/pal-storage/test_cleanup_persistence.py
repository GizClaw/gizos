"""Prove the clean-restart oracle rejects persisted leftovers on real SQLite/FS."""
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile


binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="pal-storage-oracle-", dir=os.environ.get("TEST_TMPDIR")) as directory:
    root = Path(directory)
    (root / "files").mkdir()

    def run(phase, success=True, failed_case=None):
        process = subprocess.run([binary, str(phase), root, "42"], capture_output=True, text=True, timeout=180)
        rows = [json.loads(line.removeprefix("H2_STORAGE_CASE "))
                for line in process.stdout.splitlines() if line.startswith("H2_STORAGE_CASE ")]
        assert (process.returncode == 0) == success, process.stdout + process.stderr
        if failed_case:
            assert any(row["id"] == failed_case and row["status"] == "FAIL" for row in rows), rows

    run(1)
    # Same-size chunk repetition must not pass the 16 KiB persistence oracle.
    with sqlite3.connect(root / "preferences.sqlite") as database:
        blob = database.execute("SELECT value FROM h2_pref WHERE namespace='h2storea' AND key='blob'").fetchone()[0]
        assert len(blob) == 16384 and blob[:256] != blob[-256:]
        database.execute("UPDATE h2_pref SET value=? WHERE namespace='h2storea' AND key='blob'",
                         (blob[:-256] + blob[:256],))
    run(2, False, "pal.storage.persist.pref")
    with sqlite3.connect(root / "preferences.sqlite") as database:
        database.execute("UPDATE h2_pref SET value=? WHERE namespace='h2storea' AND key='blob'", (blob,))
    run(2)
    run(3)
    for namespace in ("h2storea", "h2storeb"):
        with sqlite3.connect(root / "preferences.sqlite") as database:
            database.execute("INSERT INTO h2_pref VALUES (?, 'counter', 3, ?, 0)",
                             (namespace, (999).to_bytes(4, "little")))
        run(3, False, "pal.storage.cleanup.persist.pref")
        with sqlite3.connect(root / "preferences.sqlite") as database:
            database.execute("DELETE FROM h2_pref WHERE namespace = ?", (namespace,))
        run(3)
    (root / "files/run").mkdir()
    (root / "files/run/leftover").write_bytes(b"not cleaned")
    run(3, False, "pal.storage.cleanup.persist.fs")
    print("PAL_STORAGE_CLEANUP_ORACLE removed/cleared namespace and FS leftovers rejected")
