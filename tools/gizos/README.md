# @gizclaw/gizos

A TypeScript + Bun toolbox for shared SDKs and host tools. The source and version
catalog live in GizOS. All firmware worktrees use the same configurable store;
installing or upgrading the npm command does not duplicate SDKs or toolchains.

## Registry and installation

The package is `@gizclaw/gizos`; its installed command is `gizos`.
Bun 1.3.9 or newer is required. Configure the GizClaw registry in `~/.npmrc`
(or `$XDG_CONFIG_HOME/.npmrc` when `XDG_CONFIG_HOME` is set):

```ini
@gizclaw:registry=https://static-volc.gizclaw.com/npm/
```

Replace an existing scope mapping rather than adding a duplicate. Use this
global mapping for `bunx`; other dependencies keep their configured registry.
Public package downloads require no token.

After a release has been imported into the registry by Deploy:

```sh
bunx @gizclaw/gizos setup idf 6.2
bunx @gizclaw/gizos setup bk 3.1.1
bunx @gizclaw/gizos setup bk-loader 1.1.0.2.post1
bunx @gizclaw/gizos setup adb 35.0.2
bunx @gizclaw/gizos env
```

Before the initial publication, run the same commands from the source checkout:

```sh
bun tools/gizos/src/cli.ts --help
bazel run //tools/gizos:gizos -- env
```

## Supported environments

| Resource | Preparation | Environment contract |
| --- | --- | --- |
| ESP-IDF | `setup idf 6.2` or `setup idf 6.0.3` | SDK-selected compilers, Python and tools; `IDF_PATH`, `IDF_TOOLS_PATH`, `IDF_PYTHON_ENV_PATH`, `H2LOADER_IDF_PATH` |
| BK7258 | `setup bk 3.1.1` or another exact catalog version | `BK7258_PATH`, `BK_SDK_DIR`, shared GNU Arm 10.3.1, `COMPILER_TOOLCHAIN_PATH`, `GIZOS_BK_TOOLCHAIN_ROOT` |
| BK Loader | `setup bk-loader 1.1.0.2.post1` | Public Python source from `GizClaw/bk_loader`; `BK_LOADER_BIN` and `bk_loader` on PATH |
| ADB | `setup adb 35.0.2` | Checksummed official Platform Tools; `ADB` and `adb` on PATH |
| JieLi SDKs | `setup ac695n`, `ac707n` or `ac791n` with the exact version from `list` | `JIELI_AC695N_SDK_PATH`, `JIELI_AC707N_SDK_PATH`, `JIELI_AC791N_SDK_PATH` |
| JieLi tools | Installed with a JieLi SDK, or `setup jieli-toolchain 20250805.1` and `setup jieli-postbuild 20260728.1` | `JIELI_TOOLCHAIN_ROOT`, `JIELI_POSTBUILD_ROOT` |

`list` shows all catalog versions and their installation status. `env` displays
installation directories, executable paths and PATH entries; it never installs
anything. `env bk` also shows the selected loader. `--json` provides the same
information to scripts. Individual resources can be inspected with
`gizos env bk-loader` or `gizos env adb`.

SDK setup supports macOS and Linux. It needs Git, Bash, a host Python supported
by the selected SDK, and the corresponding archive tools. GNU Arm on Apple
Silicon requires Rosetta 2. ADB supports macOS and Linux x86_64. JieLi compiler
and post-build executables run on Linux x86_64; other hosts can prepare and
export their paths for a Linux container without adding them to the host PATH.

Private JieLi sources and archives require existing Git authentication and Git
LFS access to their pinned vendor asset repositories. The toolbox retrieves
only the selected assets, checks their commits/checksums and keeps them in the
shared store. Vendor binaries are not embedded in the npm package.

## BK Loader source installation

The default loader comes from the public `GizClaw/bk_loader` repository at an
exact catalog commit. It is Python, installed with its pinned dependencies into
a shared Python environment. Flutter is not required. The source package's
flash database and configuration work from any calling directory.

```sh
bunx @gizclaw/gizos setup bk-loader 1.1.0.2.post1
bunx @gizclaw/gizos env bk-loader
bunx @gizclaw/gizos run -- bk_loader --version
```

The source covers ordinary BK7258 CLI operations. The newer vendor reference
application has additional secure-debug and key-injection orchestration that
has not been reconstructed. Its macOS arm64 installation can be explicitly
adopted as `bk-loader 4.0.2+260129055`; the entire directory, including
`_internal`, must remain present. Source-level tests and command discovery do
not establish physical-device flashing acceptance.

## Shared storage and versions

Choose a shared location once:

```sh
bunx @gizclaw/gizos config set root /path/on/shared/disk/gizos
```

`GIZOS_DEVENV_HOME` or `--root` overrides that setting. Defaults use the platform
data directory (`~/Library/Application Support/gizos/devenv` on macOS;
`$XDG_DATA_HOME/gizos/devenv` or `~/.local/share/gizos/devenv` on Linux).
Existing `devenv.json` settings and installation records remain compatible.
State is independent of the invoking worktree and bunx's package cache.

The toolbox owns `src/catalog.json`. Unknown versions are rejected; setup never
falls back to master or latest. SDK labels are immutable: a changed commit gets
a new version/revision. The existing GizOS `bk 3.1.1` pin is retained. The newer
Firmwares commit `d789d71d89d11584c3350ca67600ff9455a7f57a` is available as
`3.1.1-rd789d71`; select it explicitly when the consumer requires that revision.
Consumers independently validate SDK/toolchain compatibility.

Source revisions coexist under `sdks/`. IDF versions share compiler downloads
and family/version directories, with separate versioned Python environments.
BK versions share the same GNU Arm installation. Cross-process locks serialize
installations; modified SDKs and invalid existing tools are preserved and
rejected rather than overwritten.

## Activate or run

```sh
eval "$(bunx @gizclaw/gizos env --shell zsh)"
eval "$(bunx @gizclaw/gizos env idf 6.2 --shell zsh)"
bunx @gizclaw/gizos shell bk 3.1.1
bunx @gizclaw/gizos run idf 6.2 -- bazel build --config=esp32s3 //path:target
bunx @gizclaw/gizos run -- bk_loader --help
```

Without a resource argument, activation combines installed catalog defaults.
Missing optional resources are skipped. `env --shell` emits quoted statements;
`eval` applies them to the caller. `shell` starts a child shell and `exit` returns
to the parent. `run` keeps the caller's working directory and forwards child
exit status/signals. None of these operations downloads missing tools.

The environment includes SDK/Python/compiler, ADB and loader PATH entries.
Repeated activation deduplicates entries. Switching environments removes paths
owned by the previous activation while retaining unrelated caller paths.
Old H2Loader Python aliases are cleared. JieLi compiler/post-build paths remain
explicit locators instead of selecting an arbitrary compiler family on PATH.

## Reuse existing installations

Adopt existing payloads without moving or downloading them:

```sh
bunx @gizclaw/gizos adopt idf 6.2 --sdk /existing/idf --tools /existing/idf-tools
bunx @gizclaw/gizos adopt bk 3.1.1 --sdk /existing/bk --tools /existing/gnu-arm
bunx @gizclaw/gizos adopt adb 35.0.2 --path /existing/android-platform-tools-35.0.2
bunx @gizclaw/gizos adopt jieli-toolchain 20250805.1 --path /existing/jieli-toolchain
bunx @gizclaw/gizos adopt jieli-postbuild 20260728.1 --path /existing/jieli-postbuild
```

`--sdk` is the Git checkout root; an SDK's internal `SDK/` directory is selected
by the catalog. `--path` is a standalone tool's complete installation directory.
For a source loader it is the Python environment directory. JieLi SDK adoption
uses the registered shared tools, or accepts `--tools` and `--postbuild` paths.
`--python /path/to/python` selects an existing IDF Python environment explicitly.
Adopted payloads remain external and must be retained while referenced.

The repository operation-environment adapter consumes `env --shell sh` and
never installs tools. An unadopted local `.env/devenv` remains a migration
fallback; after adoption, activation does not source the old provider.

## Upgrade and releases

```sh
bun add --global @gizclaw/gizos
gizos upgrade
gizos upgrade 2026.10.30-r012345
```

`upgrade` explicitly uses the GizClaw registry and only updates the global
package. SDK and tool records remain intact. Ephemeral bunx users can request
`bunx @gizclaw/gizos@latest env`; CI should pin a package version.

Release package versions are `YYYY.M.D-rHHMMSS`, corresponding to GizOS's
`vYYYYMMDD-HHMMSS` release. GizOS builds `gizclaw-gizos-<version>.tgz` and puts
it in the complete GitHub Release with `npm-index.json` and `SHA256SUMS`.
Deploy's existing `terraform/releases/npm/gizclaw-gizos` owner validates the
release and imports immutable archives and cumulative package metadata into
`https://static-volc.gizclaw.com/npm/`. Bun verifies archive integrity when
installing. The registry is a static distribution endpoint.

Deploy must select a complete Devtools snapshot containing Provider/helpers
0.2.28 or later to admit `@gizclaw/gizos` alongside H2Loader and firmware. A
GitHub Release alone does not make a package available in the registry.

## Development

```sh
cd tools/gizos
bun install --frozen-lockfile
bun run typecheck
bun run test
bun run build
```

Tests use local Git sources, synthetic executables and an HTTP archive fixture.
They cover shared pools, atomic/checksummed installation, adoption, host
compatibility, shell quoting, PATH switching and standalone package execution.
They do not open or modify a device.
