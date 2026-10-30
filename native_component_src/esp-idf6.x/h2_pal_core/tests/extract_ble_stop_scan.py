"""Compile a complete production BLE entry against host SDK stubs."""
from pathlib import Path
import re
import sys


source = Path(sys.argv[1]).read_text(encoding="utf-8")
entry = sys.argv[3] if len(sys.argv) > 3 else "h2_esp_ble_stop_scan"
match = re.search(
    rf"^static h2_pal_result_t {re.escape(entry)}\([^;{{]*\)\s*\{{",
    source,
    re.M,
)
if match is None:
    raise RuntimeError(f"production BLE entry {entry} is missing")
depth, end = 1, match.end()
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
Path(sys.argv[2]).write_text(source[match.start():end] + "\n", encoding="utf-8")
