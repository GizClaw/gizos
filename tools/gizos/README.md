# gizos

A TypeScript + Bun command-line manager for shared SDK and toolchain installations.
Source and version catalogs live in the GizOS repository. Users run the npm
package; no separate devenv checkout is required.

## Commands

```sh
bunx gizos setup idf 6.2
bunx gizos setup bk 3.1.1
bunx gizos list
bunx gizos env
bunx gizos env idf 6.2
bunx gizos env bk
```

The package is published by GizOS Release. These registry commands become
available after the initial npm publication. For development inside GizOS:

```sh
bun tools/gizos/src/cli.ts --help
bazel run //tools/gizos:gizos -- env
```

`env` displays installation paths and never installs dependencies. `--json`
provides the same information to scripts. Inspecting `env bk` shows SDK,
toolchain and optional loader locations.

## Shared storage and versions

Select a shared location once, including a location on an external development
disk:

```sh
bunx gizos config set root /path/to/shared/gizos
```

`GIZOS_DEVENV_HOME` or `--root` overrides that setting. Defaults use the platform
data directory (`~/Library/Application Support/gizos/devenv` on macOS and
`$XDG_DATA_HOME/gizos/devenv`, or `~/.local/share/gizos/devenv`, on Linux).
State never depends on the invoking worktree or bunx's package cache.
The existing `devenv.json` configuration, storage layout and
`GIZOS_DEVENV_HOME` variable remain compatible with earlier installations.

The manager owns `src/catalog.json`: exact SDK commits and tool distributions.
`setup` rejects unknown versions instead of falling back to master or latest.
Currently the catalog contains IDF 6.0.3, the GizOS IDF 6.2 baseline, and BK7258
SDK 3.1.1. GizOS independently checks whether a selected SDK is compatible with
its build.
Catalog SDK versions are immutable: a different source commit needs a new
catalog version/revision. Updating the manager never silently retargets an
already-installed SDK.

Source revisions coexist under `sdks/`. IDF versions share one host-specific
`IDF_TOOLS_PATH`, letting the official installer reuse its compiler
family/version directories, downloads and versioned Python environments.
GNU Arm is likewise shared across BK SDK versions. Install operations use
cross-process locks; existing modified SDKs are rejected and preserved.

## Activate or run

```sh
eval "$(bunx gizos env idf 6.2 --shell zsh)"
eval "$(bunx gizos env bk 3.1.1 --shell bash)"
bunx gizos shell idf 6.2
bunx gizos run idf 6.2 -- bazel build --config=esp32s3 //path:target
```

`--shell` prints quoted activation statements. `shell` starts a child shell in
the current terminal; `exit` returns to the parent environment. `run` activates
only its child command and forwards its exit status. Neither operation downloads
or installs missing environments.
Without an SDK argument, `env --shell sh` activates installed catalog defaults;
missing optional families are skipped. The repository's operation-environment
adapter uses this mode and retains the local legacy adapter as a fallback.

## Reuse existing installations

Register existing SDKs and tools without moving, downloading or modifying them:

```sh
bunx gizos adopt idf 6.2 --sdk /path/to/idf --tools /path/to/idf-tools
bunx gizos adopt bk 3.1.1 --sdk /path/to/bk-sdk --tools /path/to/gnu-arm
```

`--python /path/to/python` resolves an ambiguous existing IDF Python environment.
Adopted paths remain external and must be retained while used by the manager.
The first registered tool pool is also reused for newly installed SDK versions,
so adoption does not force another copy of an existing compiler.
For SDK sources only, `setup ... --sources-only` avoids installing compilers.

The CLI requires Bun 1.3.9 or newer. SDK setup supports macOS and Linux and uses
Git, Bash, a host Python supported by IDF, and tar for GNU Arm. IDF's own
installer provisions its compiler distributions and Python environment.
GNU Arm on Apple Silicon requires Rosetta 2.

BK Loader, private JieLi SDKs/tools and CedarX assets remain explicit external
inputs. `BK_LOADER_BIN` is preserved when activating BK; the manager does not
publish or download that vendor package.

## Upgrade and releases

```sh
bun add --global gizos
gizos upgrade
gizos upgrade 2026.10.30-r012345
```

`upgrade` installs/updates the global npm command using Bun. It does not change
shared SDKs or tools. For ephemeral bunx use, request `gizos@latest`;
pin the package version in CI.

Release packages use `YYYY.M.D-rHHMMSS`, derived from the corresponding
`vYYYYMMDD-HHMMSS` GizOS release. Each release includes the npm tarball in its
validated asset bundle, then publishes that exact tarball to npm.
The publication job uses npm trusted publishing for this repository's
`.github/workflows/release.yml`, or the `NPM_TOKEN` repository secret. The initial
package publication needs ownership of the unscoped `gizos` package and a
publishing credential; subsequent releases can use the package's configured
trusted publisher.
An already-published version is accepted only when its integrity matches the
release tarball.

```sh
cd tools/gizos
bun install --frozen-lockfile
bun run typecheck
bun run test
bun run build
```

The tests use local Git repositories and synthetic tools. Package tests execute
the bundled entry from another directory. No device is accessed.
