"""Generate only the firmware transport; no object SDK or codec is linked."""
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--protocol", type=Path, required=True)
parser.add_argument("--template", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
protocol = json.loads(args.protocol.read_text())
if set(protocol) != {"channel", "version"} or not isinstance(protocol["channel"], str) or type(protocol["version"]) is not int:
    raise ValueError("invalid firmware transport identity")
source = args.template.read_text()
marker = "/* FIRMWARE_PROTOCOL */"
if source.count(marker) != 1:
    raise ValueError("transport template requires one identity insertion point")
args.output.write_text(source.replace(marker, "const protocol = " + json.dumps(protocol, sort_keys=True) + ";"))
