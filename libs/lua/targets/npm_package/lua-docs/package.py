#!/usr/bin/env python3
"""Assemble static Lua docs from canonical sources; no third-party dependencies."""

from __future__ import annotations

import argparse
import hashlib
import json
import posixpath
from pathlib import Path
import re
import subprocess
from urllib.parse import urlsplit


PACKAGE = "libs/lua/targets/npm_package/lua-docs"
RUNTIME_ROOTS = ("libs/lua/include/", "libs/lua/src/")
REPOSITORY = "https://github.com/GizClaw/gizos"
LINK = re.compile(r"(?<!!)\[([^\]\n]+)\]\(([^\s)]+)\)")
SIGNATURE = re.compile(r"\b([a-z][a-z0-9_]*(?:[.:][a-z][a-z0-9_]*)+)\(([^()]*)\)")
EXAMPLE = re.compile(r"<!-- example: ([a-z0-9-]+) -->\s*```lua\n(.*?)\n```", re.S)


def encoded(value: object) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def safe_path(name: str) -> str:
    if (not isinstance(name, str) or not name or name.startswith("/")
            or any(p in {"", ".", ".."} for p in name.split("/"))
            or any(c in name for c in "\\\x00\r\n?#")):
        raise ValueError(f"invalid relative path: {name!r}")
    return name


class Inputs:
    def __init__(self, root: Path, mapping: dict[str, str] | None = None):
        self.root = root
        self.mapping = mapping

    def read(self, name: str) -> bytes:
        safe_path(name)
        return Path(self.mapping[name]).read_bytes() if self.mapping is not None else (self.root / name).read_bytes()

    def json(self, name: str) -> dict:
        return json.loads(self.read(name))

    def runtime_paths(self) -> list[str]:
        if self.mapping is not None:
            return sorted(n for n in self.mapping if n.startswith(RUNTIME_ROOTS) and n.endswith((".h", ".c")))
        return sorted(p.relative_to(self.root).as_posix() for base in RUNTIME_ROOTS
                      for p in (self.root / base).rglob("*") if p.is_file() and p.suffix in {".h", ".c"})


def git(root: Path, *args: str) -> bytes:
    return subprocess.check_output(["git", *args], cwd=root)


def revision_pin(root: Path, revision: str) -> dict:
    revision = git(root, "rev-parse", "--verify", f"{revision}^{{commit}}").decode().strip()
    names = git(root, "ls-tree", "-r", "--name-only", revision, "--", *RUNTIME_ROOTS).decode().splitlines()
    files = {n: sha256(git(root, "show", f"{revision}:{n}")) for n in names if n.endswith((".h", ".c"))}
    if not files:
        raise ValueError("runtime revision has no Lua sources")
    return {"repository": REPOSITORY, "revision": revision, "files": files}


def source_content_id(files: dict[str, str]) -> str:
    return sha256(b"".join(n.encode() + b"\0" + files[n].encode() + b"\n" for n in sorted(files)))


def validate_pin(inputs: Inputs, pin: dict) -> str:
    if pin.get("repository") != REPOSITORY or not re.fullmatch(r"[0-9a-f]{40}", pin.get("revision", "")):
        raise ValueError("runtime pin must identify an immutable GizOS commit")
    paths = inputs.runtime_paths()
    if not paths or paths != sorted(pin["files"]):
        raise ValueError("runtime source file set differs from runtime-pin.json; update the pin and docs together")
    for name in paths:
        if sha256(inputs.read(name)) != pin["files"][name]:
            raise ValueError(f"runtime source differs from pinned revision: {name}")
    # Same filename/NUL/hex-digest/newline construction as source_content_id.py;
    # this scope is Lua include/src only, NOT the portable C dependency closure.
    return source_content_id(pin["files"])


def header_contract(data: str, title: str) -> str:
    match = re.search(r"/\*\*(.*?)\*/", data, re.S)
    if match is None:
        raise ValueError("canonical header is missing its file contract")
    lines = [re.sub(r"^\s*\* ?", "", line) for line in match[1].splitlines()]
    lines = [line for line in lines if not line.strip().startswith("@file ")]
    body = "\n".join(lines).strip().replace("@brief ", "")
    return f"# {title}\n\n{body}\n"


def rewrite_links(text: str, source: str, documents: dict[str, str], revision: str) -> str:
    def replace(match: re.Match) -> str:
        label, target = match.groups()
        parsed = urlsplit(target)
        if parsed.scheme or parsed.netloc or target.startswith("#"):
            return match[0]
        if target.startswith("/"):
            raise ValueError(f"canonical document links must be relative or external: {source} -> {target}")
        resolved = posixpath.normpath(posixpath.join(posixpath.dirname(source), parsed.path))
        if not posixpath.splitext(resolved)[1]:
            resolved += ".md"
        if resolved in documents:
            target = "./" + documents[resolved].split("/", 1)[1]
            if parsed.fragment:
                target += "#" + parsed.fragment
        else:
            # References outside the package remain explicit, revision-pinned
            # repository links rather than broken local files.
            target = f"{REPOSITORY}/blob/{revision}/{resolved}"
            if parsed.fragment:
                target += "#" + parsed.fragment
        return f"[{label}]({target})"
    return LINK.sub(replace, text)


def symbol_locations(text: str, doc: dict, modules: set[str], receivers: dict) -> list[dict]:
    results = []
    fenced = False
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if line.startswith("```"):
            fenced = not fenced
            continue
        if fenced:
            continue
        # Only declared API bullets and the first cell of signature tables.
        # Prose calls and example invocations are not signature declarations.
        if line.startswith("| `"):
            declaration = line.split("|", 2)[1]
        elif re.match(r"^\s*-\s*`?(?:[a-z]|#)", line):
            declaration = line.strip()
            cursor = index + 1
            while cursor < len(lines) and lines[cursor].startswith("  "):
                declaration += " " + lines[cursor].strip()
                cursor += 1
        elif doc.get("format") == "header" and line.startswith("vmath."):
            declaration = line
        else:
            continue
        for match in SIGNATURE.finditer(declaration):
            raw, parameters = match.groups()
            name = raw
            if ":" in raw:
                receiver, method = raw.split(":", 1)
                if receiver not in receivers:
                    continue
                name = receivers[receiver] + ":" + method
            module = name.split(".", 1)[0].split(":", 1)[0]
            if module not in modules:
                continue
            results.append({
                "id": name, "name": name, "module": module,
                "kind": "method" if ":" in name else "function",
                "signature": f"{raw}({parameters})",
                "documentId": doc["id"], "path": doc["path"], "line": index + 1,
                "summary": re.sub(r"\s+", " ", line.strip("| "))[:600],
            })
    return results


def module_index(inputs: Inputs, documents: list[dict]) -> list[dict]:
    profile = inputs.read("libs/lua/src/modules/h2_lua_esp_claw.c").decode()
    inventory = dict(re.findall(r'\{"([a-z0-9_]+)", H2_LUA_ESP_CLAW_MODULE_([A-Z_]+)\}', profile))
    registrations = inputs.read("libs/lua/src/modules/h2_lua_modules.c").decode()
    builtins = set(re.findall(r'add_preload\(state, "([a-z0-9_]+)"', registrations))
    if not builtins or not inventory:
        raise ValueError("cannot extract Runtime preload/profile inventory")
    return [{"id": name, "name": name, "requireable": name in builtins,
             "profileStatus": inventory.get(name, "GIZOS_BUILTIN").lower().replace("_", "-"),
             "documents": [d["id"] for d in documents if name in d["modules"]]}
            for name in sorted(set(inventory) | builtins)]


def assemble(inputs: Inputs) -> dict[str, bytes]:
    config = inputs.json(f"{PACKAGE}/catalog.json")
    package = inputs.json(f"{PACKAGE}/package.json")
    pin = inputs.json(f"{PACKAGE}/runtime-pin.json")
    content_id = validate_pin(inputs, pin)
    documents = [dict(d, path=f"docs/{d['id']}.md") for d in config["documents"]]
    if len({d["id"] for d in documents}) != len(documents):
        raise ValueError("duplicate document ID")
    paths = {d["source"]: d["path"] for d in documents}
    files = {"LICENSE": inputs.read("LICENSE"), "README.md": inputs.read(f"{PACKAGE}/README.md")}
    sources = {f"{PACKAGE}/{n}" for n in ("package.py", "package.json", "catalog.json", "runtime-pin.json", "README.md")}
    sources.add("LICENSE")
    modules = module_index(inputs, documents)
    module_names = {m["id"] for m in modules}
    entries = {}
    examples = []
    for doc in documents:
        safe_path(doc["path"])
        source = inputs.read(doc["source"])
        sources.add(doc["source"])
        text = source.decode("utf-8")
        if doc.get("format") == "header":
            text = header_contract(text, doc["title"])
        text = rewrite_links(text, doc["source"], paths, pin["revision"])
        files[doc["path"]] = text.encode()
        doc["title"] = doc.get("title") or text.splitlines()[0].removeprefix("# ")
        doc["sourceSha256"] = sha256(source)
        for found in symbol_locations(text, doc, module_names, config["receivers"]):
            key = found["id"]
            location = {k: found[k] for k in ("signature", "documentId", "path", "line", "summary")}
            if key not in entries:
                entries[key] = {k: found[k] for k in ("id", "name", "module", "kind")}
                entries[key]["locations"] = []
            if location not in entries[key]["locations"]:
                entries[key]["locations"].append(location)
        for match in EXAMPLE.finditer(text):
            example_id, code = match.groups()
            filename = f"examples/{example_id}.lua"
            if filename in files:
                raise ValueError(f"duplicate example ID: {example_id}")
            files[filename] = (code + "\n").encode()
            examples.append({"id": example_id, "path": filename, "documentId": doc["id"],
                             "line": text[:match.start()].count("\n") + 1})
    index = {"schemaVersion": 1, "modules": modules, "symbols": [entries[k] for k in sorted(entries)]}
    files["index.json"] = encoded(index)
    package["files"] = sorted([*files, "manifest.json"])
    files["package.json"] = encoded(package)
    manifest = {
        "schemaVersion": 1, "name": package["name"], "version": package["version"],
        "runtime": {"repository": pin["repository"], "revision": pin["revision"],
                    "profileId": "runtime.lua.gizos", "luaVersion": "5.5",
                    "sourceContentId": content_id, "sourceScope": list(RUNTIME_ROOTS),
                    "files": pin["files"]},
        "index": "index.json", "documents": documents, "examples": examples,
        "sources": {n: sha256(inputs.read(n)) for n in sorted(sources)},
        "files": {n: {"sha256": sha256(data), "size": len(data)} for n, data in sorted(files.items())},
    }
    files["manifest.json"] = encoded(manifest)
    validate(files)
    return files


def anchors(text: str) -> set[str]:
    result = set(re.findall(r'<a\s+(?:id|name)="([^"]+)"', text))
    counts = {}
    fenced = False
    for line in text.splitlines():
        if line.startswith("```"):
            fenced = not fenced
        if fenced or not line.startswith("#"):
            continue
        title = re.sub(r"^#+\s+", "", line).strip().lower()
        title = re.sub(r"[^\w\s-]", "", title)
        title = re.sub(r"\s+", "-", title)
        suffix = counts.get(title, 0)
        counts[title] = suffix + 1
        result.add(title + (f"-{suffix}" if suffix else ""))
    return result


def validate(files: dict[str, bytes]) -> None:
    for name in files:
        safe_path(name)
    manifest = json.loads(files["manifest.json"])
    package = json.loads(files["package.json"])
    index = json.loads(files[manifest["index"]])
    if manifest["schemaVersion"] != 1 or index["schemaVersion"] != 1:
        raise ValueError("unsupported documentation schema")
    if (manifest["name"], manifest["version"]) != (package["name"], package["version"]):
        raise ValueError("manifest/package identity mismatch")
    runtime = manifest["runtime"]
    if (runtime["repository"] != REPOSITORY
            or not re.fullmatch(r"[0-9a-f]{40}", runtime["revision"])
            or runtime["profileId"] != "runtime.lua.gizos"
            or runtime["sourceScope"] != list(RUNTIME_ROOTS)
            or not runtime["files"]
            or any(not n.startswith(RUNTIME_ROOTS) or not re.fullmatch(r"[0-9a-f]{64}", h)
                   for n, h in runtime["files"].items())
            or runtime["sourceContentId"] != source_content_id(runtime["files"])):
        raise ValueError("invalid Runtime source provenance")
    if set(files) != {"package.json", *package["files"]} or set(manifest["files"]) != set(files) - {"manifest.json"}:
        raise ValueError("package/manifest file inventory mismatch")
    for name, record in manifest["files"].items():
        if record != {"sha256": sha256(files[name]), "size": len(files[name])}:
            raise ValueError(f"package content digest mismatch: {name}")
    documents = {d["id"]: d for d in manifest["documents"]}
    if len(documents) != len(manifest["documents"]):
        raise ValueError("duplicate document ID")
    for doc in documents.values():
        if doc["path"] != f"docs/{doc['id']}.md" or doc["sourceSha256"] != manifest["sources"][doc["source"]]:
            raise ValueError("invalid document path/source provenance")
        text = files[doc["path"]].decode()
        if "<!--@include:" in text:
            raise ValueError(f"unexpanded document include: {doc['path']}")
        for match in LINK.finditer(text):
            target = match[2]
            parsed = urlsplit(target)
            if parsed.scheme or parsed.netloc:
                continue
            resolved = posixpath.normpath(posixpath.join(posixpath.dirname(doc["path"]), parsed.path)) if parsed.path else doc["path"]
            safe_path(resolved)
            if resolved not in files:
                raise ValueError(f"broken package link: {doc['path']} -> {target}")
            if parsed.fragment and parsed.fragment not in anchors(files[resolved].decode()):
                raise ValueError(f"broken package anchor: {doc['path']} -> {target}")
    ids = set()
    module_ids = {m["id"] for m in index["modules"]}
    for module in index["modules"]:
        if any(d not in documents for d in module["documents"]):
            raise ValueError("unknown module document")
    for symbol in index["symbols"]:
        if symbol["id"] in ids or symbol["module"] not in module_ids or not symbol["locations"]:
            raise ValueError("invalid symbol index")
        ids.add(symbol["id"])
        for location in symbol["locations"]:
            doc = documents[location["documentId"]]
            lines = files[doc["path"]].decode().splitlines()
            if location["path"] != doc["path"] or not 1 <= location["line"] <= len(lines):
                raise ValueError("invalid symbol location")
            declaration = "\n".join(lines[location["line"] - 1:location["line"] + 15])
            if re.sub(r"\s+", "", location["signature"]) not in re.sub(r"\s+", "", declaration):
                raise ValueError("symbol signature does not match its document location")
    example_ids = set()
    for example in manifest["examples"]:
        if example["id"] in example_ids or example["documentId"] not in documents:
            raise ValueError("invalid example ID/document")
        example_ids.add(example["id"])
        code = files[example["path"]]
        if not code or b"\0" in code or code.startswith(b"\x1bLua"):
            raise ValueError("invalid Lua text example")
        text = files[documents[example["documentId"]]["path"]].decode()
        marker = f"<!-- example: {example['id']} -->"
        if (example["path"] != f"examples/{example['id']}.lua"
                or not 1 <= example["line"] <= len(text.splitlines())
                or text.splitlines()[example["line"] - 1] != marker
                or dict(EXAMPLE.findall(text)).get(example["id"], "").encode() + b"\n" != code):
            raise ValueError("example differs from its document location")


def write_package(files: dict[str, bytes], output: Path) -> None:
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ValueError(f"output must be a new or empty directory: {output}")
    for name, data in files.items():
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["build", "validate", "pin", "verify-revision"])
    parser.add_argument("--repository-root", type=Path, default=Path(__file__).resolve().parents[5])
    parser.add_argument("--inputs", type=Path, help="Bazel's repository-relative input mapping")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--package-json-output", type=Path)
    parser.add_argument("--revision")
    args = parser.parse_args()
    inputs = Inputs(args.repository_root, json.loads(args.inputs.read_text()) if args.inputs else None)
    if args.command == "pin":
        if not args.revision:
            parser.error("pin requires --revision")
        (args.repository_root / PACKAGE / "runtime-pin.json").write_bytes(encoded(revision_pin(args.repository_root, args.revision)))
    elif args.command == "verify-revision":
        pin = inputs.json(f"{PACKAGE}/runtime-pin.json")
        if pin != revision_pin(args.repository_root, pin["revision"]):
            raise ValueError("runtime-pin.json does not match its immutable Git revision")
        validate_pin(inputs, pin)
        print(f"Verified Lua sources at {pin['revision']}")
    elif args.command == "build":
        if args.output is None:
            parser.error("build requires --output")
        files = assemble(inputs)
        write_package(files, args.output)
        if args.package_json_output:
            args.package_json_output.write_bytes(files["package.json"])
        print(f"Built {len(files)} static files in {args.output}")
    else:
        if args.output is None:
            parser.error("validate requires --output")
        files = {p.relative_to(args.output).as_posix(): p.read_bytes() for p in args.output.rglob("*") if p.is_file()}
        validate(files)
        print(f"Validated {len(files)} static files")


if __name__ == "__main__":
    main()
