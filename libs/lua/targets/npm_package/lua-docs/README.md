# GizOS Lua Runtime documentation data

`@gizclaw/lua-docs` is a static npm data package for browser documentation tools, editors and build-time extraction into services such as Go MCP servers. It contains JSON, Markdown, Lua examples and license text. There is no JavaScript runtime, install hook, network lookup, inference service or Node.js requirement for consuming the installed files. Node/npm is only needed for npm installation or the optional npm consumption smoke test.

## Canonical sources

GizOS owns this public platform documentation. `catalog.json` selects the library-owned Markdown in `libs/lua/docs/` and the Display/numeric public-header contracts. The Host, Display and numeric narratives were moved from the former Guides tree, preserving their explanations beside the Runtime rather than restoring the retired website. `libs/lua/docs/runtime.md` supplies the core scripting signatures; the Host guide owns setup, App storage, audio framing and BLE lifecycle. Edit these canonical sources, not a copied manual or generated package. Header file comments are rendered directly, without Doxygen, into the same data package. Explicitly named Lua examples are extracted verbatim from canonical Markdown fences. `guides/` remains limited to the repository's AI development constraints.

`package.json` here is the package identity/export template. Its explicit `files` list is generated during assembly. Publish the **assembled package or release tarball**, never this source/tool directory.

## Files and schema version 1

All JSON uses UTF-8. All artifact paths are case-sensitive POSIX paths relative to the package root, without absolute paths or traversal. IDs are stable across package versions; a consumer must scope its cache by package version as content and line numbers can change. Ignore unknown object fields; reject unsupported `schemaVersion` values. Existing field meanings and stable IDs/paths must not be repurposed in a compatible release.

| File | Contract |
| --- | --- |
| `manifest.json` | Root object: `schemaVersion`, npm `name`/`version`, `runtime`, `index`, `documents`, `examples`, `sources`, `files`. |
| `index.json` | Root object: `schemaVersion`, `modules`, `symbols`. |
| `docs/<document-id>.md` | Standalone canonical contract/narrative text; includes are resolved. Links to other packaged docs remain relative. References outside the package use explicit URLs pinned to the Runtime revision. |
| `examples/<example-id>.lua` | Text-only snippets with their prerequisites in the originating document; they are not firmware packages. |
| `README.md`, `LICENSE`, `package.json` | Consumer instructions, Apache-2.0 license and npm identity/exports. |

`manifest.runtime` records `repository`, a full immutable Git `revision`, `profileId` (`runtime.lua.gizos`), `luaVersion`, `sourceScope`, a `sourceContentId` and `files` (repository-relative source path to SHA-256). The scope is **only `libs/lua/include/` and `libs/lua/src/` C/header files**. `sourceContentId` hashes the sorted concatenation of `path + NUL + lowercase_sha256 + newline`. It is not the existing portable C source archive's dependency-closure content ID, a build ID or a firmware ID. The checked-in `runtime-pin.json` is generated from Git objects at `revision`; assembly rejects changed, added or removed Runtime C/header files.

`manifest.documents` is an ordered array of `{id, path, source, sourceSha256, title, language, modules}` records, with optional `format: "header"` for rendered header comments. `source` is the canonical repository path, not an artifact path. The canonical Markdown may be newer than the pinned Runtime: `sourceSha256` and `sources` identify its exact bytes without claiming it existed at the Runtime revision. `manifest.examples` contains `{id, path, documentId, line}`; `line` points to the named example marker in the packaged document.

`manifest.sources` maps canonical documentation and assembler/configuration input paths to SHA-256. `manifest.files` maps every artifact file except the manifest itself to `{sha256, size}` (UTF-8/binary bytes). The manifest does not hash itself. The release bundle's checksums cover the whole compressed tarball.

`index.modules` contains `{id, name, requireable, profileStatus, documents}`. The built-in preload names and pinned ESP-Claw status inventory are extracted from real Runtime C registrations. `requireable` means a preload exists; device acquisition can still fail. `component-adapted` Button is acquired through `runtime.components.get`, not `require("button")`. Unavailable profile modules are included so clients can explain unsupported requests.

`index.symbols` contains `{id, name, module, kind, locations}`. A location is `{signature, documentId, path, line, summary}` with a one-based artifact line. `kind` is `function` or `method`. Method IDs use the constructor/type name, e.g. `vmath.buffer:get`, while `signature` preserves the canonical receiver spelling (`b:get(index)`). Search `name`, module and location summaries, then load the referenced document for the complete parameters, return/error semantics and constraints. No search engine or ranking algorithm is imposed. The index covers **explicit signature declarations**, not all prose calls, fields, constants, standard Lua APIs or every registered advanced function. Absence from the symbol index alone does not mean an API is unavailable; consult the full contracts and module inventory. Do not infer signatures from example argument values.

## Selecting compatible documentation

Pin an exact npm version in the consumer lockfile and persist its `manifest.runtime.revision` beside the firmware's declared GizOS dependency. Use docs whose Runtime revision matches that dependency, or explicitly audit the Lua source scope and the embedding app's configuration before accepting a different revision. A matching scoped content ID establishes identical scoped source bytes, not identical providers, optional modules or native extensions. If no matching artifact exists, build one from the firmware's GizOS checkout and record the result, rather than silently selecting `latest`.

There is no automatic SemVer compatibility inference between this documentation package, GizOS release batches and downstream firmware versions. In particular, current GizOS source **does not establish compatibility with released H106 firmware**. Product repositories own their dependency pins, device capabilities, private API documentation and device validation. No private product source is included here.

## Build and validate

From the GizOS repository root, using Python 3.10+ for the standalone path:

```sh
python3 libs/lua/targets/npm_package/lua-docs/package.py verify-revision
python3 libs/lua/targets/npm_package/lua-docs/package.py build --output build/lua-docs
python3 libs/lua/targets/npm_package/lua-docs/package.py validate --output build/lua-docs
python3 libs/lua/targets/npm_package/lua-docs/package_test.py
```

Build output must be new or empty; assembly never recursively deletes an existing output. For the normal Bazel graph and existing deterministic npm release packer:

```sh
bazel build --config=macos_arm64 //libs/lua/targets/npm_package/lua-docs:release_tarball
bazel test --config=macos_arm64 //libs/lua/targets/npm_package/lua-docs:package_test
```

Use `linux_x86_64` on Linux. The test validates canonical inputs, repeatable assembly, links/anchors, symbol/example locations, source digests, file inventories and the exact Bazel-packed tarball. It exercises source drift and malformed artifact rejection. The host CI's existing build/test graph includes these public targets. The optional npm smoke test packs, installs into a new temporary consumer with scripts disabled, searches a symbol, reads its Markdown/example and compares installed bytes:

```sh
python3 libs/lua/targets/npm_package/lua-docs/npm_smoke_test.py
```

These are artifact/consumer checks. They do not execute the examples on a device, validate firmware, exercise BLE/audio hardware or establish target frame rates.

## npm and browser consumption

This package follows the repository's `@gizclaw` GitHub Packages convention. Configure the registry and, if your account/registry requires it, authentication:

```ini
@gizclaw:registry=https://npm.pkg.github.com
```

After an authorized publication:

```sh
npm install --save-exact @gizclaw/lua-docs@0.1.0
```

A bundler can import the two JSON entry points:

```js
import manifest from '@gizclaw/lua-docs/manifest.json';
import index from '@gizclaw/lua-docs/index.json';
const hits = index.symbols.filter(s => s.name.includes('runtime.sleep'));
```

Copy files selected by `manifest.documents` and `manifest.examples` into the browser's static assets, preserving relative paths, or use your bundler's raw file imports (e.g. `@gizclaw/lua-docs/docs/runtime.md?raw` in Vite). JSON module syntax outside bundlers depends on the importing environment. `manifest.json` is also the package's root export. Browser clients fetch only their own application's bundled assets; this package adds no runtime CDN dependency.

For Go, extract the tarball's `package/` files during the service build into a local directory and use `//go:embed` with `encoding/json` and `fs.ReadFile`. Resolve every `path` relative to that directory. Node.js is not needed in the running service. Verify the release tarball checksum before extraction and file hashes after extraction. A consumer can implement list/search/get-document/ get-example operations over this same contract without a GizOS-specific Node server or private product dependency.

## Versioning and release

The npm version is independent of Runtime source and starts at `0.1.0`. Bump it for every published artifact, including documentation-only fixes; npm versions are immutable. Review changes to the schema or stable IDs as consumer contracts. For a Runtime update, first update/audit the canonical docs against that Runtime, then regenerate its pin from an immutable commit:

```sh
python3 libs/lua/targets/npm_package/lua-docs/package.py pin --revision <gizos-commit>
python3 libs/lua/targets/npm_package/lua-docs/package.py verify-revision
```

The working Runtime sources must match that commit. Commit the new pin, template version and canonical docs together; run package tests and the npm smoke test. For the first artifact, the new core reference complements the existing public contracts at the recorded Runtime revision.

`//tools/bazel:npm_release_bundle` includes this package's deterministic `release_tarball`. The existing **GizOS Release** npm-packages slice therefore includes it in `npm-index.json` and checksum-protected GitHub release assets. No separate publication workflow is added. An authorized maintainer can publish the validated release tarball with `npm publish <tarball> --registry https://npm.pkg.github.com`, or import it through the existing registry release process. Creating a PR or building the bundle does not execute npm publication.
