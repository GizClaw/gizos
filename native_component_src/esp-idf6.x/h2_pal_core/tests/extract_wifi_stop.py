"""Expose production Wi-Fi stop and teardown adapters for host SDK injection."""
from pathlib import Path
import re
import sys


source = Path(sys.argv[1]).read_text(encoding="utf-8")
entries = (
    "h2_esp_wifi_teardown_dhcp_stop",
    "h2_esp_wifi_teardown_stop",
    "h2_esp_wifi_teardown_deinit",
    "h2_esp_wifi_teardown_map_error",
    "h2_esp_wifi_teardown_driver",
    "h2_esp_wifi_release_driver",
    "h2_esp_platform_wifi_stop",
)
functions = []
for entry in entries:
    match = re.search(
        rf"^(?:static )?int {re.escape(entry)}\([^;{{]*\)\s*\{{",
        source,
        re.M,
    )
    if match is None:
        raise RuntimeError(f"production Wi-Fi entry {entry} is missing")
    depth, end = 1, match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    functions.append(source[match.start():end])
Path(sys.argv[2]).write_text("\n\n".join(functions) + "\n", encoding="utf-8")
