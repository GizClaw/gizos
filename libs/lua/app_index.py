"""Generate the runtime preinstalled index from public lua_app manifests."""
import argparse
import json
from pathlib import Path, PurePosixPath


def build_index(specification: dict) -> dict:
    root = specification["root"]
    if not isinstance(root, str) or not root or len(root.encode()) >= 192:
        raise ValueError("Invalid preinstalled root")
    parts = root[1:].split("/") if root.startswith("/") else root.split("/")
    if (str(PurePosixPath(root)) != root or "\\" in root or ":" in root
            or any(not part or part.startswith(".") for part in parts)
            or any(ord(char) < 32 or ord(char) == 127 for char in root)):
        raise ValueError("Invalid preinstalled root")
    apps = []
    ids = set()
    for source in specification["manifests"]:
        manifest = json.loads(Path(source).read_text(encoding="utf-8"))
        if manifest["format"] != 1 or manifest["type"] != "lua-app":
            raise ValueError("Unexpected manifest type")
        app_id = manifest["app_id"]
        if app_id in ids:
            raise ValueError("Duplicate preinstalled app: " + app_id)
        ids.add(app_id)
        apps.append({"root": root, "manifest": manifest})
    if len(apps) > 32:
        raise ValueError("Too many preinstalled apps")
    result = {"format": 1, "apps": apps}
    if len(json.dumps(result, ensure_ascii=False, separators=(",", ":")).encode()) > 256 * 1024:
        raise ValueError("Preinstalled index exceeds 256 KiB")
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--specification", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    specification = json.loads(args.specification.read_text(encoding="utf-8"))
    args.output.write_text(
        json.dumps(build_index(specification), ensure_ascii=False, sort_keys=True,
                   separators=(",", ":")) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
