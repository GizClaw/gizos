# @gizclaw/gizos

A TypeScript + Bun command-line manager for shared SDK and toolchain installations.
Source and version catalogs live in the GizOS repository. Users run the npm
package; no separate devenv checkout is required.

## Registry and installation

The npm package is `@gizclaw/gizos`, using the lowercase spelling of the GizClaw
scope. Its installed command is `gizos`. Packages are distributed through
GizClaw's registry at `https://static-volc.gizclaw.com/npm/`.

Add this scope mapping to `~/.npmrc` so Bun can resolve the package from any
working directory. When `XDG_CONFIG_HOME` is set, use
`$XDG_CONFIG_HOME/.npmrc`:

```ini
@gizclaw:registry=https://static-volc.gizclaw.com/npm/
```

Replace an existing `@gizclaw:registry` entry rather than adding a duplicate.
Use the global mapping for `bunx`. Only the `@gizclaw` scope uses this registry;
other dependencies keep their configured registry. Public package downloads
require no token.

The registry serves package metadata and tarballs. SDK sources, compilers and
Python environments are installed separately by `setup` and shared across
GizOS worktrees.

## Commands

```sh
bunx @gizclaw/gizos setup idf 6.2
bunx @gizclaw/gizos setup bk 3.1.1
bunx @gizclaw/gizos list
bunx @gizclaw/gizos env
bunx @gizclaw/gizos env idf 6.2
bunx @gizclaw/gizos env bk
```

These registry commands become available after the first GizOS Release has
been imported into the registry by Deploy. For development inside GizOS:

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
bunx @gizclaw/gizos config set root /path/to/shared/gizos
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
eval "$(bunx @gizclaw/gizos env idf 6.2 --shell zsh)"
eval "$(bunx @gizclaw/gizos env bk 3.1.1 --shell bash)"
bunx @gizclaw/gizos shell idf 6.2
bunx @gizclaw/gizos run idf 6.2 -- bazel build --config=esp32s3 //path:target
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
bunx @gizclaw/gizos adopt idf 6.2 --sdk /path/to/idf --tools /path/to/idf-tools
bunx @gizclaw/gizos adopt bk 3.1.1 --sdk /path/to/bk-sdk --tools /path/to/gnu-arm
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
bun add --global @gizclaw/gizos
gizos upgrade
gizos upgrade 2026.10.30-r012345
```

`upgrade` installs/updates the global npm command using Bun. It does not change
shared SDKs or tools, and explicitly selects the GizClaw registry. For ephemeral
bunx use, request `bunx @gizclaw/gizos@latest env`; pin the package version in CI.

## Release and registry publication

Release packages use `YYYY.M.D-rHHMMSS`, derived from the corresponding
`vYYYYMMDD-HHMMSS` GizOS release. Publication follows the existing GizOS npm
release flow:

1. GizOS builds `gizclaw-gizos-<version>.tgz` with its executable `gizos` entry.
2. The complete GitHub Release includes that exact archive, `npm-index.json`
   and `SHA256SUMS`, alongside the other GizOS npm packages.
3. Deploy's `terraform/releases/npm/gizclaw-gizos` owner validates the selected
   release and imports its immutable archives and cumulative package metadata
   into `https://static-volc.gizclaw.com/npm/`.
4. Bun resolves `@gizclaw/gizos` through the scope mapping and verifies the
   archive integrity supplied by the registry.

Deploy must select a complete Devtools snapshot with Deploy Provider/helpers
0.2.28 or later. That importer registers `@gizclaw/gizos` as a GizOS-owned
package alongside `@gizclaw/h2loader` and `@gizclaw/firmware`.
A GitHub Release being available does not by itself make a package installable
from the registry. The registry is a static read-only distribution endpoint;
publication is performed through Deploy's existing Release flow.

## Development

```sh
cd tools/gizos
bun install --frozen-lockfile
bun run typecheck
bun run test
bun run build
```

The tests use local Git repositories and synthetic tools. Package tests execute
the bundled entry from another directory. No device is accessed.
