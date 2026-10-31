<div align="center">

# GizOS

**A portable firmware and application platform for embedded devices, desktop, mobile, and WebAssembly.**

[![CI](https://github.com/GizClaw/gizos/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/GizClaw/gizos/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/GizClaw/gizos)](LICENSE)
[![Bazel](https://img.shields.io/badge/build-Bazel%209.2-43A047?logo=bazel&logoColor=white)](https://bazel.build/)
[![C](https://img.shields.io/badge/core-C-00599C?logo=c&logoColor=white)](libs/)

![Embedded](https://img.shields.io/badge/target-Embedded-6A5ACD)
![Desktop](https://img.shields.io/badge/target-Desktop-455A64)
![Mobile](https://img.shields.io/badge/target-Mobile-00897B)
![WebAssembly](https://img.shields.io/badge/target-WebAssembly-654FF0?logo=webassembly&logoColor=white)

</div>

GizOS provides a shared foundation for building portable firmware and applications without tying reusable code to one board, operating system, or product. It combines platform abstractions, runtimes, libraries, board support, build rules, packaging, and validation in one Bazel workspace.

## What GizOS provides

- **Portable application runtime** — compose applications from reusable libraries and platform-independent interfaces.
- **Platform Abstraction Layer (PAL)** — consistent contracts for tasks, networking, storage, audio, display, input, and other system capabilities.
- **Reusable libraries** — networking, media, protocols, UI, game runtime, command routing, serialization, security, and utilities.
- **Board and SDK integration** — public board packages and native components for ESP, Beken, JieLi, and embedded Linux targets.
- **Applications and tooling** — H2Loader, examples, E2E programs, PIXA games, showcases, packaging tools, and generated API documentation.
- **Reproducible builds** — Bazel is the source of truth for dependency resolution, cross-platform builds, tests, artifacts, and releases.

## Supported targets

| Family | Targets |
| --- | --- |
| Embedded | ESP32-S3, ESP32-P4, ESP32-C5, BK7258 AP/CP, AC695N, AC791N |
| Desktop | Linux x86_64, macOS arm64, Windows x86_64 |
| Mobile | iOS Simulator arm64, Android arm64 |
| Web | WebAssembly via Emscripten |

Target compatibility is declared in the Bazel graph, so each configuration builds and tests only the packages supported by that platform.

## Architecture

```text
Applications
    │
Runtime and reusable libraries
    │
Platform Abstraction Layer
    │
Platform providers and native components
    │
Boards, operating systems, and SDKs
```

Product-specific code lives outside GizOS and consumes its public interfaces. This keeps the platform reusable while allowing each product repository to own its hardware composition, configuration, assets, and release policy.

## Quick start

Requirements vary by target. For a host build, install Bazel 9.2.0 or a compatible Bazel launcher, a C/C++ toolchain, and the platform dependencies required by the selected configuration.

```sh
git clone https://github.com/GizClaw/gizos.git
cd gizos

make help
make bazel-build BAZEL_CONFIG=linux_x86_64
make bazel-test BAZEL_CONFIG=linux_x86_64
```

On Apple Silicon, use `BAZEL_CONFIG=macos_arm64`. Other supported configurations and native SDK setup are documented in the [Bazel guide](tools/bazel/README.md).

To verify that public targets also work when GizOS is consumed as a dependency:

```sh
make bazel-test-downstream-consumer
```

Run `make format` before committing. It formats changes since `origin/main` with pinned tools: clang-format 22.1.5 for C/C++ (changed lines only; paths in `.clang-format-ignore` are skipped), StyLua 2.5.2 for Lua and buildifier 10.0.1 for Bazel files (whole changed files). StyLua and buildifier are downloaded and checksum-verified on first use. `make format-check` checks changes without modifying files. CI runs `scripts/config/format-check.sh --all` on the full source tree and requires it through `CI required`.

The command entries are `scripts/config/format.sh` and `scripts/config/format-check.sh`, with shared logic in `scripts/common/format.sh`. Both commands fail on tool errors while still checking the other languages. A successful C/C++ rewrite is accepted in fix mode; Lua fixes must stabilize within five passes. Run `python3 scripts/common/test_format.py` for the isolated failure-path regression tests; they need no formatter downloads.

Keep embedded JavaScript in `EM_JS` / `EM_ASM` calls between `/* clang-format off */` and `/* clang-format on */` guards. A one-time full reformat uses `scripts/config/format.sh --all`, followed by `scripts/config/format-check.sh --all`. Long C/C++ string literals remain unbroken to avoid array string-concatenation warnings. Use `git blame --ignore-revs-file .git-blame-ignore-revs <path>` to look past the one-time formatting commit.

## Shared SDK toolbox

The TypeScript/Bun package `@gizclaw/gizos` requires Bun 1.3.9 or newer and
manages SDK versions and shared toolchains. Its installed command is `gizos`;
all GizOS worktrees share installation records. Each SDK and tool version can
use its own configured installation paths.

Configure the GizClaw registry in `~/.npmrc` (or `$XDG_CONFIG_HOME/.npmrc` when
`XDG_CONFIG_HOME` is set) to use the toolbox from any directory:

```ini
@gizclaw:registry=https://static-volc.gizclaw.com/npm/
```

After the package has been published through Deploy's registry import:

```sh
bunx @gizclaw/gizos setup esp-idf 6.2
bunx @gizclaw/gizos setup bk7258 3.1.1
bunx @gizclaw/gizos install adb 35.0.2
bunx @gizclaw/gizos install h2loader
bunx @gizclaw/gizos install h6loader
bunx @gizclaw/gizos env bk7258
bun add --global @gizclaw/gizos
gizos upgrade
```

`env` displays SDK, toolchain, Python, loader and ADB installation paths.
BK setup automatically installs GizClaw's source-built BK Loader 4.1.7 for the host,
verifies its checksums and adds it to PATH when activated. Individual tools
use `gizos install`; a separate BK Loader install is optional. Releases come
from `GizClaw/bk_loader` and need no Python installation or vendor browser
verification. Use `gizos install bk_loader 4.1.7 --archive <release-archive>`
to import a previously downloaded archive. `env --shell`
prints shell activation statements, and `run` executes a command with the
selected environment.
`upgrade` updates the toolbox while preserving installed SDKs.

Use `gizos config esp_idf 6.2 --sdk /shared/sdks/esp-idf-6.2 --tools /shared/toolchains/esp-idf`
to save independent paths for an SDK version, or
`gizos config bk-loader --path /shared/tools/bk-loader` for a standalone tool.
Omitting the version uses the selected version, falling back to the catalog default.
`setup` applies saved paths and selects the prepared version;
`env` shows actual installations. The shared root supplies default locations
and keeps records and download caches. Compatible compilers remain shared by
default.

`gizos setup esp_idf 6.2` selects the cached 6.2 environment; setting up 6.0.3
switches the selected paths while retaining 6.2. Explicit versions in `run`
or `shell` leave that selection unchanged. `env` marks the selected version.
Use `gizos remove esp_idf 6.0.3` to remove one version and `gizos prune --dry-run`
to preview unused managed caches. Cleanup preserves adopted payloads and
shared dependencies.

The project catalog owns stable SDK/chip names and their allowed source aliases.
Run `gizos list` or `gizos list bk_idk` to see supported chips, source aliases,
repositories, defaults, current selections and versions. Use
`gizos describe esp_idf` for an English explanation, usage examples and
documentation links. BK and ESP-IDF default to GizClaw forks;
`gizos config bk7258 GizClaw`, `gizos config bk7259 GizClaw` and
`gizos config bk_idk GizClaw` select the source for each entry. `bk_idk`
manages one SDK for multiple chips. `gizos config esp_idf Espressif` selects
the listed ESP-IDF upstream. Arbitrary URLs and unlisted sources are rejected.
Omit the source alias to inspect
the setting, or use `--reset` to restore the default. Existing SDKs and
toolchains remain shared, and the catalog commit pins still apply.

GizOS Release builds the npm archive, index and checksums; Deploy imports the
validated release into `https://static-volc.gizclaw.com/npm/`. For development
before registry publication, run `bun tools/gizos/src/cli.ts env` directly.
See [the toolbox README](tools/gizos/README.md) for SDK setup, multi-version
storage, environment activation and the release flow. Optional vendor assets
remain explicit external inputs.

## Use GizOS from Bzlmod

Add GizOS as a module override in the consuming repository and depend on public targets through `@gizos//...`:

```starlark
bazel_dep(name = "gizos", version = "0.0.0")

git_override(
    module_name = "gizos",
    commit = "<gizos-commit>",
    remote = "https://github.com/GizClaw/gizos.git",
)
```

The consuming repository owns its root platform selection and product composition. GizOS owns portable libraries, PAL providers, native components, and public build interfaces.

## Repository layout

```text
boards/                 Board definitions and target-specific composition
guides/                 Architecture, development, usage, and API documentation
libs/                   Portable libraries, runtimes, and PAL providers
native_component_src/   Components compiled by native platform SDKs
projects/               Applications, examples, games, and E2E programs
scripts/                Stable repository command implementations
third_party/            Bazel overlays, patches, and integration metadata
tools/                  Build, packaging, code generation, and support tools
```

## Documentation

- [Project documentation](guides/index.md)
- [Development guide](guides/zh/guide.md)
- [Bazel and platform builds](tools/bazel/README.md)
- [API references](guides/references/index.md)
- [Application documentation](guides/apps/index.md)

Build the documentation locally with:

```sh
make guides-build
make guides-preview
```

## License

GizOS is available under the [Apache License 2.0](LICENSE).
