"""Check the real Bazel-generated fixture contract without service credentials."""
import json
from pathlib import Path
import sys


def main():
    contract = json.loads(Path(sys.argv[1]).read_text())
    expected = {
        "app_config_key": "media.songs.en-us",
        "runtime_profile": "fixture-profile",
        "app_config_value": "fixture-value",
        "time_server": "",
        "physical_audio": False,
    }
    if contract != expected:
        raise AssertionError("generated fixture contract changed its configured input")


if __name__ == "__main__":
    main()
