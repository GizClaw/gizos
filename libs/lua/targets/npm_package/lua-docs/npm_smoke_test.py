#!/usr/bin/env python3
"""Pack/install the real npm artifact into an isolated offline consumer."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

import package as docs


NODE_CHECK = r"""
import manifest from '@gizclaw/lua-docs' with {type: 'json'};
import index from '@gizclaw/lua-docs/index.json' with {type: 'json'};
import {readFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
const root = new URL('./node_modules/@gizclaw/lua-docs/', import.meta.url);
if (manifest.schemaVersion !== 1 || index.schemaVersion !== 1) throw Error('schema');
const symbol = index.symbols.find(s => s.name === 'runtime.sleep');
if (!symbol) throw Error('symbol search');
const location = symbol.locations[0];
const text = readFileSync(new URL(location.path, root), 'utf8');
if (!text.split('\n')[location.line - 1].includes('runtime.sleep(ms)')) throw Error('document retrieval');
const example = manifest.examples.find(e => e.id === 'cooperative-tasks');
if (!readFileSync(new URL(example.path, root), 'utf8').includes('runtime.join(child)')) throw Error('example');
for (const [path, record] of Object.entries(manifest.files)) {
  const data = readFileSync(new URL(path, root));
  if (data.length !== record.size || createHash('sha256').update(data).digest('hex') !== record.sha256) throw Error(path);
}
console.log(`npm consumer: ${manifest.name}@${manifest.version}, ${index.modules.length} modules, ${index.symbols.length} symbols, ${manifest.examples.length} examples`);
"""

GO_CHECK = r"""package main
import (
    "embed"
    "encoding/json"
    "fmt"
)
//go:embed data
var data embed.FS
func main() {
    raw, err := data.ReadFile("data/manifest.json"); if err != nil { panic(err) }
    var manifest struct {
        SchemaVersion int `json:"schemaVersion"`
        Index string `json:"index"`
        Documents []struct { ID string `json:"id"`; Path string `json:"path"` } `json:"documents"`
    }
    if err := json.Unmarshal(raw, &manifest); err != nil { panic(err) }
    if manifest.SchemaVersion != 1 { panic("schema") }
    raw, err = data.ReadFile("data/" + manifest.Index); if err != nil { panic(err) }
    var index struct { Symbols []struct { Name string `json:"name"` } `json:"symbols"` }
    if err := json.Unmarshal(raw, &index); err != nil { panic(err) }
    found := false
    for _, symbol := range index.Symbols { if symbol.Name == "runtime.sleep" { found = true } }
    if !found { panic("symbol search") }
    for _, doc := range manifest.Documents {
        if _, err := data.ReadFile("data/" + doc.Path); err != nil { panic(err) }
    }
    fmt.Printf("Go embed consumer: %d documents, %d symbols, no Node.js runtime\n", len(manifest.Documents), len(index.Symbols))
}
"""


def run(command, cwd, env=None):
    return subprocess.check_output(command, cwd=cwd, env=env, text=True)


def main():
    root = Path(__file__).resolve().parents[5]
    expected = docs.assemble(docs.Inputs(root))
    with tempfile.TemporaryDirectory(prefix="gizos-lua-docs-") as tmp:
        temp = Path(tmp)
        package = temp / "assembled"
        docs.write_package(expected, package)
        packed = json.loads(run(["npm", "pack", "--json", "--ignore-scripts", "--offline", "--pack-destination", str(temp)], package))
        archive = temp / packed[0]["filename"]
        consumer = temp / "consumer"
        consumer.mkdir()
        (consumer / "package.json").write_text('{"name":"lua-docs-consumer","version":"0.0.0","private":true,"type":"module"}\n')
        run(["npm", "install", "--offline", "--ignore-scripts", "--no-audit", "--no-fund", "--package-lock=false", str(archive)], consumer)
        installed = consumer / "node_modules/@gizclaw/lua-docs"
        actual = {p.relative_to(installed).as_posix(): p.read_bytes() for p in installed.rglob("*") if p.is_file()}
        if actual != expected:
            raise AssertionError("npm-installed files differ from the assembled package")
        docs.validate(actual)
        (consumer / "check.mjs").write_text(NODE_CHECK)
        print(run(["node", "check.mjs"], consumer).strip())
        if shutil.which("go") is None:
            raise RuntimeError("Go is required to verify build-time extraction/embedding")
        go_consumer = temp / "go-consumer"
        go_consumer.mkdir()
        shutil.copytree(installed, go_consumer / "data")
        (go_consumer / "main.go").write_text(GO_CHECK)
        env = dict(os.environ, GOWORK="off", GOTOOLCHAIN="local")
        print(run(["go", "run", "main.go"], go_consumer, env).strip())
        print(f"Verified npm pack/install and all {len(actual)} installed file digests")


if __name__ == "__main__":
    main()
