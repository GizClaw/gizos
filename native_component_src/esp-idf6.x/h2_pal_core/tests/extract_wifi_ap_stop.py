"""Expose the complete production AP-stop function for host SDK injection."""
from pathlib import Path
import re
import sys


source = Path(sys.argv[1]).read_text(encoding="utf-8")
match = re.search(r"^static int h2_esp_wifi_ap_stop\([^;{]*\)\s*\{", source, re.M)
if match is None:
    raise RuntimeError("production AP-stop entry is missing")
depth, end = 1, match.end()
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
Path(sys.argv[2]).write_text(source[match.start():end] + "\n", encoding="utf-8")
