"""Fault quarantine deliberately lasts until child-process teardown."""
import subprocess
import sys
from pathlib import Path

for mode in range(1, 7):
    subprocess.run([str(Path(sys.argv[1]).resolve()), "--display-fault", str(mode)],
                   check=True, timeout=20)
