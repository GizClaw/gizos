"""Qualify actual FS/SQLite across seed, verify and repeated clean restarts."""
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
registry = Path(sys.argv[2])
expected = {name: int(phase) for name, phase in re.findall(r'H2_PAL_STORAGE_CASE\("([^"]+)", ([123])\)', registry.read_text())}
assert len(expected) == 36
with tempfile.TemporaryDirectory(prefix="pal-storage-", dir=os.environ.get("TEST_TMPDIR")) as directory:
    root = Path(directory)
    (root / "files").mkdir()
    sentinel = root / "unrelated"
    sentinel.write_bytes(b"must survive qualification")
    nonce = secrets.randbits(32)
    cases, phases = [], []
    completion_replay = None
    for phase in (1, 2, 3, 3):
        process = subprocess.run([binary, str(phase), root, str(nonce)], capture_output=True, text=True, timeout=180)
        print(process.stdout, end="")
        print(process.stderr, end="", file=sys.stderr)
        assert process.returncode == 0, f"phase {phase} failed: {process.returncode}"
        rows = [json.loads(line.removeprefix("H2_STORAGE_CASE ")) for line in process.stdout.splitlines() if line.startswith("H2_STORAGE_CASE ")]
        result = [json.loads(line.removeprefix("H2_STORAGE_PHASE ")) for line in process.stdout.splitlines() if line.startswith("H2_STORAGE_PHASE ")]
        assert len(result) == 1
        selected = {key for key, value in expected.items() if value == phase}
        assert len(rows) == len(selected) and {r["id"] for r in rows} == selected
        assert all(r["phase"] == phase and r["nonce"] == nonce and r["rc"] == 0 and r["status"] == "PASS" for r in rows)
        state = result[0]
        assert state["contract"] == 2 and state["phase"] == phase and state["nonce"] == nonce and state["passed"] == len(selected)
        assert state["failed"] == state["blocked"] == state["cleanup"] == 0 and state["balanced"]
        if len(phases) < 3:
            cases.extend(rows)
        else:
            completion_replay = rows
        phases.append(state)
    assert len({phase["pid"] for phase in phases}) == len(phases)
    assert not (root / "files/run").exists()
    assert sentinel.read_bytes() == b"must survive qualification"
    output = {"contract": 2, "qualified": True, "operations": 28, "passed": len(cases), "failed": 0, "blocked": 0, "cases": cases, "phases": phases, "completion_replay": completion_replay}
    if os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR"):
        (Path(os.environ["TEST_UNDECLARED_OUTPUTS_DIR"]) / "qualified.json").write_text(json.dumps(output, indent=2) + "\n")
    print(f"PAL_STORAGE_E2E contract=2 operations=28 passed={len(cases)} failed=0 blocked=0 restart=PASS cleanup_restart=PASS completion_replay=PASS qualified=1")
