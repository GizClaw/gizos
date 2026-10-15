"""Fixed Mosaico identity; reuse the unchanged shared fresh-boot verifier."""
import argparse
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[6]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = ROOT / "projects/e2e/libs/pal-net-tls-device/verify_device.py"
    spec = importlib.util.spec_from_file_location("net_tls_device", source)
    shared = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(shared)
    result = shared.parse(args.log.read_text(errors="replace"), args.version,
                          "esp_mosaico")
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
