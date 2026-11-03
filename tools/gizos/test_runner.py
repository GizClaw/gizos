import os
from pathlib import Path
import subprocess
import sys

runtime, *tests = [str(Path(item).resolve()) for item in sys.argv[1:]]
environment = {**os.environ, "BUN_TELEMETRY_DISABLE": "1"}
raise SystemExit(subprocess.call(
    [runtime, "test", "--timeout", "120000", *tests], env=environment,
))
