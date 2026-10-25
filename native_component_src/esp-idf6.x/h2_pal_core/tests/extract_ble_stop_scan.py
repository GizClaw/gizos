"""Compile the complete production scan-stop entry against host SDK stubs."""
from pathlib import Path
import re
import sys


source = Path(sys.argv[1]).read_text(encoding="utf-8")
match = re.search(
    r"^static h2_pal_result_t h2_esp_ble_stop_scan\([^;{]*\)\s*\{",
    source,
    re.M,
)
if match is None:
    raise RuntimeError("production BLE scan-stop entry is missing")
depth, end = 1, match.end()
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
Path(sys.argv[2]).write_text(source[match.start():end] + "\n", encoding="utf-8")
