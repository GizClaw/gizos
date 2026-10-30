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
bunx @gizclaw/gizos setup esp-idf 6.2
bunx @gizclaw/gizos setup bk7258 3.1.1
bunx @gizclaw/gizos setup adb 35.0.2
bunx @gizclaw/gizos env
```

Before the initial publication, run the same commands from the source checkout:

```sh
bun tools/gizos/src/cli.ts --help
bazel run //tools/gizos:gizos -- env
```

## Supported environments

The project catalog gives each SDK a stable name: `esp-idf`, `bk7258`, `bk7259`,
`bk_idk`, `jieli-ac695n`, `jieli-ac707n` and `jieli-ac791n`. BK entries follow
chip names when the SDK line targets one chip, and use the SDK name for a
multi-chip repository such as `bk_idk`. Each entry has a maintained list of
source aliases, supported chips and exact version/commit pins. `env` displays the
selected repository together with its installation paths. Existing family
aliases (`bk`, `idf`, `ac695n`, `ac707n`, `ac791n`), `esp_idf` and the previous
JieLi repository basenames remain accepted for older scripts. An ambiguous
family/version combination requires an explicit SDK name. The old
`bk_avdk_smp <version>` selector remains available for versioned commands;
repository configuration now uses `bk7258` or `bk7259` explicitly.

| Resource | Preparation | Environment contract |
| --- | --- | --- |
| ESP-IDF | `setup esp-idf 6.2` or `setup esp-idf 6.0.3` | SDK-selected compilers, Python and tools; `IDF_PATH`, `IDF_TOOLS_PATH`, `IDF_PYTHON_ENV_PATH`, `H2LOADER_IDF_PATH` |
| BK7258 | `setup bk7258 3.1.1` or `setup bk7258 3.1.1-rd789d71` | SMP SDK, shared GNU Arm 10.3.1 and BK Loader; `BK7258_PATH`, `BK_SDK_DIR`, `COMPILER_TOOLCHAIN_PATH`, `GIZOS_BK_TOOLCHAIN_ROOT`, `BK_LOADER_BIN` |
| BK7259 | `setup bk7259 4.0.1` | BK7259 source revision, shared GNU Arm 14.3.1 and BK Loader; `BK7259_PATH` and `BK_SDK_DIR` |
| BK IDK | `setup bk_idk 2.0.1-r650e754` | One SDK snapshot for BK7236/BK7258, shared GNU Arm and BK Loader; `BK_IDK_PATH` and `BK_SDK_DIR` |
| BK Loader | Installed with BK setup, or `setup bk-loader 1.1.0.2.post2` | Checksummed host executable from the public `GizClaw/bk_loader` Release; `BK_LOADER_BIN` and `bk_loader` on PATH |
| ADB | `setup adb 35.0.2` | Checksummed official Platform Tools; `ADB` and `adb` on PATH |
| JieLi SDKs | `setup jieli-ac695n`, `setup jieli-ac707n` or `setup jieli-ac791n` with the exact version from `list` | `JIELI_AC695N_SDK_PATH`, `JIELI_AC707N_SDK_PATH`, `JIELI_AC791N_SDK_PATH` |
| JieLi tools | Installed with a JieLi SDK, or `setup jieli-toolchain 20250805.1` and `setup jieli-postbuild 20260728.1` | `JIELI_TOOLCHAIN_ROOT`, `JIELI_POSTBUILD_ROOT` |

`list` shows each SDK's source aliases, repositories, supported chips, default/current selection, catalog
versions and installation status. `list <sdk>` filters the catalog. `env` displays
installation directories, executable paths and PATH entries; it never installs
anything. `env bk7258` also shows the selected loader. `--json` provides the same
information to scripts. Individual resources can be inspected with
`gizos env bk-loader` or `gizos env adb`.

SDK setup supports macOS and Linux. It needs Git, Bash, a host Python supported
by the selected SDK, and the corresponding archive tools. GNU Arm on Apple
Silicon requires Rosetta 2 for the 10.3 compiler used by BK7258 and BK IDK.
BK7259 uses GNU Arm 14.3, available for macOS arm64 and Linux arm64/x86_64;
its catalog has no Intel macOS distribution. Unsupported hosts can prepare
SDK sources with `--sources-only`. ADB supports macOS and Linux x86_64. JieLi compiler
and post-build executables run on Linux x86_64; other hosts can prepare and
export their paths for a Linux container without adding them to the host PATH.

The AC707N SDK defaults to Jieli's public GitLab repository at the same pinned
commit as the existing installation. The selected AC695N/AC791N snapshots and
JieLi tool archives still require access to their maintained asset repositories.
Git LFS is required where the catalog marks it. The toolbox retrieves
only the selected assets, checks their commits/checksums and keeps them in the
shared store. Vendor binaries are not embedded in the npm package.
Compiler archives are downloaded with the host Python HTTPS client, which is
already a prerequisite for SDK setup. TLS verification remains enabled, and
the manager checks the pinned archive checksum before extraction.

## SDK catalog and repository selection

The repository owns the list in `src/catalog.json`. Discover SDK names,
source aliases, repositories and versions before choosing a source:

```sh
gizos list
gizos list esp-idf
gizos list bk7258
gizos list bk_idk
gizos list jieli-ac707n --json
```

| SDK name | Default source alias and repository | Other source aliases |
| --- | --- | --- |
| `esp-idf` | `GizClaw` -> `GizClaw/esp-idf` | `Espressif` -> `espressif/esp-idf` |
| `bk7258` | `GizClaw` -> `GizClaw/bk_avdk_smp` (3.1.1 line) | `Beken` -> `bekencorp/bk_avdk_smp` |
| `bk7259` | `GizClaw` -> `GizClaw/bk_avdk_smp` (4.0.1 line) | `Beken` -> `bekencorp/bk_avdk_smp` |
| `bk_idk` | `GizClaw` -> `GizClaw/bk_idk` (BK7236/BK7258) | `Beken` -> `bekencorp/bk_idk` |
| `jieli-ac695n` | `h2vivi` -> `h2vivi/AC695N_Soundbox_SDK` | None at the current pinned version |
| `jieli-ac707n` | `Jieli` -> `e_badge/e_badge_707_sdk_200` (JieLi GitLab) | `h2vivi` -> `h2vivi/e_badge_707_sdk_200` (GitHub) |
| `jieli-ac791n` | `h2vivi` -> `h2vivi/fw-AC791N_SDK` | None at the current pinned version |

`config` takes a source alias from this list. Aliases are case insensitive and
resolve to the exact maintained repository URL for the selected SDK or chip.
Unlisted sources and arbitrary URLs are rejected; adding a supported source
requires updating the project catalog. Previously listed full repository IDs
remain accepted for compatibility.

```sh
gizos config esp_idf
gizos config esp_idf Espressif
gizos config bk7258 GizClaw
gizos config bk7259 GizClaw
gizos config bk_idk GizClaw
gizos config jieli-ac707n Jieli
```

Without a source alias, the command displays the current/default aliases,
repository IDs, URLs and available choices. With an alias, it stores the selection for every catalog
version of that SDK. `gizos config` shows the shared root, configuration file
and selected source aliases. `list --json` includes a `repositories` array
containing the same choices alongside version and installation information.
Settings are shared across worktrees in the existing `devenv.json` file;
changing the shared root preserves repository selections and vice versa.

Restore the catalog default with:

```sh
gizos config bk7258 --reset
gizos config esp_idf --reset
```

Selecting another listed repository changes where future downloads come from.
The SDK name, pinned commit and compiler requirements stay the same. Each
listed source must provide the exact catalog commit; setup does not substitute
a branch or latest tag.

Already installed matching commits are reused without copying SDKs or changing
their Git remotes. `env` shows the configured URL and, when different, the
source URL recorded for the installation. Configuration and discovery commands
do not clone or install SDKs.

BK7258 and BK7259 keep independent source selections and version lists, even
when their sources use the same repository name. `bk_idk` keeps one source
selection and one installation for its supported chips. Existing
`bk_avdk_smp` records, cached sources and repository settings are read as
migration inputs without moving payloads. Changing one chip's selection
preserves the other chip's inherited setting.

The BK7259 and BK IDK entries manage SDKs and host tools. The existing GizOS
BK7258 firmware build remains pinned to the SMP SDK; these entries do not
introduce new boards or replace that build's SDK contract.
The pinned BK IDK snapshot reads `CONFIG_TOOLCHAIN_PATH` from the consuming
project's SDK configuration. For direct SDK builds, set that value to the
toolbox's exported `COMPILER_TOOLCHAIN_PATH`; the vendor snapshot otherwise
defaults to a compiler directory under `/opt`.

JieLi's public upstream projects are:

| SDK | Public upstream | Current catalog |
| --- | --- | --- |
| AC695N | https://gitlab.zh-jieli.com/soundbox/novisualization/ac695n_soundbox_sdk | Keep the existing snapshot; add this source to the selectable catalog after verifying a compatible version |
| AC707N | https://gitlab.zh-jieli.com/e_badge/e_badge_707_sdk_200 | Public default; exact commit `d0167685d032d745d88fe50233302edd46941622` and anonymous Git access verified |
| AC791N | https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK | Keep the existing snapshot; add this source to the selectable catalog after verifying a compatible version |

A public upstream URL does not make a different snapshot's commit available
there. Only the verified AC707N source URL changes by default; compiler and
post-build tool distribution is managed separately.

## SDK descriptions

`describe` explains an SDK in English without installing anything:

```sh
gizos describe esp_idf
gizos describe bk7258
gizos describe bk_idk --json
```

The output includes its purpose, catalog-supported chips, source aliases and
their repository URLs, exact version/commit pins, usage examples, notes and
documentation links. SDK aliases such as `esp_idf` work here too. `--json`
returns the same descriptive data for scripts.

## BK Loader installation

`setup bk7258 <sdk-version>`, `setup bk7259 <sdk-version>` and
`setup bk_idk <sdk-version>` also install the default BK Loader. The toolbox
selects the macOS arm64/x86_64 or Linux arm64/x86_64 archive from the public
`GizClaw/bk_loader` Release and verifies both archive and executable SHA-256
against its bundled catalog. Repeated setup and different BK SDK versions reuse
the same installed loader. `--sources-only` and `adopt` do not download it.

```sh
bunx @gizclaw/gizos setup bk7258 3.1.1
bunx @gizclaw/gizos env bk-loader
bunx @gizclaw/gizos run -- bk_loader --version
bunx @gizclaw/gizos run -- bk_loader tools selftest
```

The loader is written in Python. Its own Release freezes the interpreter,
dependencies, flash database and runtime configuration into one executable;
end users do not need pip or a separate loader Python environment. That Release
builds macOS arm64/x86_64, Linux arm64/x86_64 and Windows x86_64 artifacts.
GizOS SDK setup currently supports macOS and Linux. The loader's upstream
version banner remains `bk_loader.py,v1.1.0.2`; the Release/package revision
`1.1.0.2.post2` is reported by `tools selftest`.

For source development the previous `bk-loader 1.1.0.2.post1` catalog entry
remains available at its immutable source commit and installs into a private
Python environment. It is not the default distributed loader.

The maintained source covers ordinary BK7258 CLI operations. The newer vendor reference
application has additional secure-debug and key-injection orchestration that
has not been reconstructed. Its macOS arm64 installation can be explicitly
adopted as `bk-loader 4.0.2+260129055`; the entire directory, including
`_internal`, must remain present. Binary self-tests and command discovery do
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
a new version/revision. The existing GizOS `bk7258 3.1.1` pin is retained. The newer
Firmwares commit `d789d71d89d11584c3350ca67600ff9455a7f57a` is available as
`3.1.1-rd789d71`; select it explicitly when the consumer requires that revision.
Consumers independently validate SDK/toolchain compatibility.

Source revisions and installation records are keyed by stable SDK name and
version. Legacy repository/family records and cached sources are still read;
setup registers their existing paths under the SDK name without moving or
copying payloads. Changing a selected source does not duplicate an installed
matching revision. Different SDKs can use the same version label independently.
IDF versions share compiler downloads
and family/version directories, with separate versioned Python environments.
BK7258 and BK IDK share GNU Arm 10.3; BK7259 uses a separate GNU Arm 14.3
pool. Compiler pools are keyed by release and host, so different SDKs with
the same compiler requirements reuse one installation. Legacy BK7259 records
using the old 10.3 compiler are preserved but require setup to prepare the
correct 14.3 tools. Cross-process locks serialize
installations; modified SDKs and invalid existing tools are preserved and
rejected rather than overwritten.

## Activate or run

```sh
eval "$(bunx @gizclaw/gizos env --shell zsh)"
eval "$(bunx @gizclaw/gizos env esp-idf 6.2 --shell zsh)"
bunx @gizclaw/gizos shell bk7258 3.1.1
bunx @gizclaw/gizos run esp-idf 6.2 -- bazel build --config=esp32s3 //path:target
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
BK activation exports `BK7258_PATH`, `BK7259_PATH` or `BK_IDK_PATH` for the
selected entry and clears stale BK locators. Combined default activation
exports every installed named locator; generic `BK_SDK_DIR` uses the first
installed BK entry in catalog order. Use an explicit SDK with `run` or `shell`
when multiple BK environments are installed.

## Reuse existing installations

Adopt existing payloads without moving or downloading them:

```sh
bunx @gizclaw/gizos adopt esp-idf 6.2 --sdk /existing/idf --tools /existing/idf-tools
bunx @gizclaw/gizos adopt bk7258 3.1.1 --sdk /existing/bk --tools /existing/gnu-arm
bunx @gizclaw/gizos adopt adb 35.0.2 --path /existing/android-platform-tools-35.0.2
bunx @gizclaw/gizos adopt jieli-toolchain 20250805.1 --path /existing/jieli-toolchain
bunx @gizclaw/gizos adopt jieli-postbuild 20260728.1 --path /existing/jieli-postbuild
```

`--sdk` is the Git checkout root; an SDK's internal `SDK/` directory is selected
by the catalog. `--path` is a standalone tool's complete installation directory.
For the native loader it is the extracted archive root containing
`bin/bk_loader`; for the legacy source loader it is the Python environment
directory. JieLi SDK adoption
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
