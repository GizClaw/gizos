"""Preserve the Net/TLS browser assertions and add a separate Host receipt."""
import hashlib
import importlib
import json
import os
from pathlib import Path
import runpy
import sys

archive, runner, *sources = sys.argv[1:]
# The preserved runner prepends cwd/tools; preload its declared owner helpers
# so external-module invocation also uses GizOS's browser harness and server.
helper_root = Path(__file__).absolute().parents[5] / "tools/bazel"
sys.path.insert(0, str(helper_root))
importlib.import_module("web_archive_browser_test")
saved = sys.argv
sys.argv = [runner, archive]
try:
    runpy.run_path(runner, run_name="__main__")
finally:
    sys.argv = saved
output = Path(os.environ["TEST_UNDECLARED_OUTPUTS_DIR"])
boundary = json.loads((output / "boundary.json").read_text())
identities = {name: hashlib.sha256(Path(path).read_bytes()).hexdigest()
              for name, path in (value.split("=", 1) for value in sources)}
assert len(identities) == len(sources)
record = {"schema": 1, "new_physical_run_claimed": False,
          "test_label": "//projects/e2e/targets/pkg_tar/pal-net-tls:wasm_pal_net_tls_boundary_test",
          "input_scope": "Owned Application Host migration inputs; other Net/TLS sources retain their historical qualification hashes.",
          "source_sha256": identities,
          "source_identity_sha256": hashlib.sha256(json.dumps(identities, sort_keys=True, separators=(",", ":")).encode()).hexdigest(),
          "boundary": boundary}
(output / "application_host_boundary.json").write_text(json.dumps(record, indent=2) + "\n")
