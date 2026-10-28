# Bazel CI dependency graph

This directory owns the native Bazel build/ownership graph used to compile
stable host C/C++ and Go packages and every supported firmware platform.
Native packages use real `cc_library`, `cc_binary`, `cc_test`, `go_library`,
`go_binary`, `go_test`, and Apple application targets. All maintained ESP-IDF
launchers use the local-only `esp_idf_firmware` external rule and all maintained
BK7258 launchers use `bk7258_firmware`; fixed non-build endpoints may still
use `filegroup`.
H2Loader ESP/BK7258 delivery targets use the project-owned wrappers and
platform-neutral package rule in `projects/h2loader/tools/bazel/` on top of
those native firmware providers.
Web artifacts use the official Emscripten `wasm_cc_binary` transition and
`rules_pkg` `pkg_tar`; both rules participate in graph validation.
ESP-IDF CMake and BK SDK Make/CMake remain the native compiler
owners beneath the Bazel entry.

## One firmware graph, two compilation boundaries

Every maintained firmware endpoint declares one `graph` rooted at a package-local
`firmware_native_component`. Portable libraries that do not consume final SDK-generated
configuration remain ordinary `cc_library` targets. Each firmware entry owns exactly
one `firmware_lib_component`, which collects every Bazel library selected by that
image into the single generated native component `h2_firmware_lib`. Its physical
archives remain separate and are linked as one rescan group. SDK-dependent components,
boards, and launchers declare direct
sources, headers, include roots, metadata, dependencies, and an optional AP/CP
execution unit through `firmware_native_component`; that rule describes ownership but
does not precompile those sources.

The runners turn this same graph into invocation-local native manifests.
ESP-IDF receives ordered component directories/names and direct source lists,
BK7258 receives isolated AP and CP component directories/source lists. Native CMake or
Make retains generated SDK configuration, SDK/vendor source selection, compiler
and linker policy, bootloader/partition/Stack/BIM/merge mechanics. It must not
scan the repository or fall back to a handwritten first-party inventory. Graph
validation covers every maintained firmware endpoint.

## Local usage

Use the exact Bazel version from `.bazelversion`:

```sh
bazel test //tools/bazel/...
bazel test //tools/bazel:bk7258_runner_test //tools/bazel:esp_idf_runner_test //projects/h2loader/tools/bazel:h2loader_tar_zlib_runner_test
make bazel-build BAZEL_CONFIG=linux_x86_64
make bazel-test BAZEL_CONFIG=linux_x86_64
```

## Test scope

Source changes are checked in code review. Do not add tests whose verdict is
based only on scanning repository source text or directories for particular
strings, private function names, file inventories, or implementation order.

Preserve executable unit tests, including C fixtures driven by Python build/run
harnesses: their verdict comes from compiled behavior, not source-string
assertions. Removing or changing a harness must not silently remove its unit
coverage or fixtures. Prefer compiling and linking the real source directly
when adding or migrating such tests.

Python may also test Python tools through their callable or command-line
interfaces and validate actual build outputs. Output validation includes golden
files, generated code and metadata, package contents, ELF headers and section
layout, and device or browser E2E results.

Historical evidence may name retired source-scanning test targets; those records
describe past runs and are not the current test inventory.

## Windows compatible graph

The native Windows x86_64 baseline uses the `rules_cc`
`cc_configure_extension` to expose `@local_config_cc`. On a native Windows
host, `--config=windows_x86_64` selects
`//tools/bazel/platforms:windows_x86_64`, the matching local MSVC ABI
toolchain, and compiler parameter files. It does not provide a cross toolchain
for macOS or Linux. Globally registered embedded toolchain repositories remain
loadable on Windows but expose incompatible toolchains when their execution
host is unsupported, so they cannot preempt unrelated Windows analysis.

The required `windows-compatible-graph` CI job runs Build and Test sequentially
on the 8-core Windows Server 2025 Larger Runner selected by
`FIRMWARE_WINDOWS_RUNNER` and reuses the same Bazel server and output base. It requests the complete graph with
`bazel build --config=windows_x86_64 //...` and
`bazel test --config=windows_x86_64 //...`; `target_compatible_with` selects
the applicable targets and the repository-wide test filter excludes only
`manual` tests. Apple/mobile artifact packages are the narrow exception: their
rules transition before compatibility can skip them, so the Windows config
deletes those packages instead of maintaining a target allowlist.

The pinned Bazel 9.2.0 Windows binary runs with lockfile access disabled because
an unrelated `emsdk` transitive digest differs between Windows and the existing
hosts. Existing hosts retain strict committed lockfile validation. The complete
Build uses minimal remote output downloads, then a focused build materializes the declared `windows_exe_smoke.exe`, checks its PE32+ x86_64
headers, and executes it from a short output root. The
corresponding `windows_exe_smoke_test` ensures that the Test graph executes a
real Win32 binary. The Windows tasks run in parallel with the existing matrix,
and all results converge through `ci-required`.

The compatible graph does not itself provide Windows PAL, Runtime, Desktop
application, or packaging. Issue #835 owns Windows PAL integration.

## ESP-IDF external builds

The sibling `firmware-devenv` provides one pinned ESP-IDF 6.2 checkout and
GCC 16.1 tools for all ESP targets. Install with `make idf-tools` and source
its `export.sh`; both use the baseline fixed by that devenv revision.
Consumers choose another SDK baseline by pinning another devenv commit.
CI and Release pin the same compatible provider revision and verify its SDK
gitlink against the GizOS version manifest. The tool root is
`.tools/esp-idf-6.2`, keeping old compiler installations out of the current
compiler namespace. The tools repository derives
the unique `python_env/idf6.2_py*_env` installation and validates it
against the pinned ESP-IDF constraints, and materializes versioned SDK/tool
locators. The native action receives those locator files instead of the
machine paths or caller `PATH`.

```sh
REPO_ROOT=$PWD
export REPO_ROOT
make -C ../firmwares-devenv idf-tools IDF_TARGETS=esp32s3,esp32p4,esp32c5
. ../firmwares-devenv/export.sh
bazel build --config=esp32s3  //projects/example/targets/h2loader_tar_zlib/ble-broadcaster/devkit:package
bazel build --config=esp32p4  //projects/example/targets/h2loader_tar_zlib/display/waveshare_esp32p4_wifi6_touch_lcd_4_3:package
```

The rule keeps its project-local `srcs` and explicit out-of-package support
files as action inputs and dependency edges. `FirmwareInfo`
exposes the target, ELF, map, application image, bootloader,
partition-table image, `combined_factory.bin`, normalized flash metadata, the
complete referenced flash file tree, the selected version, and a stable depset
of those outputs. The combined factory image is produced by ESP-IDF
`merge-bin` from the same build's flash arguments and is flashed at `0x0`. The
internal `:firmware` target exposes no H2Loader identity or package provider;
its `DefaultInfo.files` contains only native outputs;
CMake/Ninja intermediates and device-operation executables are not outputs.

The action copies only the selected launcher project into an invocation-local temporary tree, points it at the read-only repository source, and invokes the native `idf.py build` followed by `idf.py merge-bin`. It is explicitly unsandboxed and ineligible for remote execution, while its declared outputs may use the local disk cache or the authenticated remote cache. The runner constructs a fixed subprocess environment from the validated locators, pinned SDK version, and the final `FirmwareVersionInfo` selected by the firmware target.

An App manually maintains its shared base with `app_version(name, value)` from `//tools/bazel:app_version.bzl`. Each board independently maintains an integer build number with `board_version(name, build)` from `//tools/bazel:board_version.bzl` in `boards/<board>/BUILD.bazel`. Both are version metadata; board versions have no source, BSP, file-collection, or platform dependencies. Existing filegroups and chip BSP/component targets keep their original responsibilities. Each firmware target declares `firmware_version`, passes its base declaration as `main_version` and its numeric counter declaration as `build`, and may set optional `variant` and `build_metadata` strings. The rule owns SemVer composition and validation; the meaning of these strings belongs to the caller. Non-empty `build_metadata` is appended after a plus and does not affect precedence. A non-empty variant and the numeric board build use separate SemVer prerelease identifiers, so standard SemVer comparison orders board builds numerically. An omitted or empty variant keeps the numeric board build as the sole prerelease identifier; callers supply the variant name without a separator. The `main_version` input requires the provider and declaration marker emitted by `app_version`, including aliases forwarding them; the `build` input requires `BoardVersionInfo`. Native firmware rules consume the final label through their existing `version` attribute, and package and release metadata inherit that value. Legacy complete `firmware_version(value = ...)` declarations, Lua App versions, and the diagnostic compatibility setting remain supported.

A firmware target may explicitly opt in to the allowlisted `H2LOADER_WIFI_CREDENTIALS` JSON object containing exact `ssid` and `password` fields; the runner validates it and exposes the two values only to that target's ESP-IDF subprocess. Unrelated ESP-IDF subprocesses never receive them. The runner neither activates the SDK nor performs flash, erase, monitor, reset, serial, network, or installation operations.

## BK7258 external builds

Source `.env/devenv` before requesting a BK7258 build. The canonical provider exports only `BK7258_PATH`. A repository rule validates the SDK commit in [`native_versions/bk7258_sdk_commit.txt`](native_versions/bk7258_sdk_commit.txt) and a clean tracked checkout. Another repository rule downloads Arm GNU 10.3-2021.10, verifies the committed archive and expanded-tree SHA-256 values, and exposes compiler version `10.3.1` through a locator. The action receives those locators and a Bazel Python runtime; it does not inherit SDK, toolchain, archive, Python, or `PATH` variables from the caller.

```sh
REPO_ROOT=$PWD
export REPO_ROOT
. .env/devenv
bazel build --config=bk7258  //projects/example/targets/h2loader_tar_zlib/display/bk7258_v3_202405:package
```

`Bk7258FirmwareInfo` exposes target-owned AP and CP ELF,
map, and application images, the managed `app_ab_crc.rbl` image, the combined
`all-app.bin` recovery image, and a partition metadata directory containing
`partitions.json`, `bk_ota_partitions.json`, `bk_package.json`, and
`configurationab.json`, plus the selected version. The internal `:firmware`
target exposes no H2Loader identity or package provider and its
`DefaultInfo.files` contains only native outputs. Every file must be non-empty, every metadata document
must be a JSON object, and resolved sources must stay inside the invocation's
native build directory. Alternate update archives, SDK intermediates, and
device-operation executables are not outputs.

The action copies the selected launcher into an invocation-local source tree,
passes the original repository only as read-only `H2_REPO_ROOT`, writes the
fixed internal firmware version to a temporary file, and calls the SDK `make`
entry directly with an action-owned `BUILD_DIR`. It does not invoke a launcher
Makefile. The same native action creates the optional Loader recovery output,
but does not create the H2Loader package or release metadata and performs no
install, flash, reset, serial, network, or device operation. The action is
unsandboxed, ineligible for remote execution, isolated per invocation, and
cacheable through the local disk cache or authenticated remote cache. CI
validates this contract with a fake SDK runner and graph tests; a real build
remains a canonical local devenv check. Committed SDK and toolchain identity
inputs bind the stable external execution environment into the action key.

## H2Loader package builds

Every package below `projects/<owner>/targets/h2loader_tar_zlib/<image>/<board>/`
declares an internal `:firmware` target using `h2loader_esp_idf_firmware` or
`h2loader_bk7258_firmware`, followed by the public `:package` target using
`h2loader_tar_zlib`. The H2Loader wrappers select partition and recovery inputs
from the declared `target` and `board`, then delegate native compilation to the
generic rules in this directory. The package rule consumes `FirmwareInfo` or `Bk7258FirmwareInfo`, writes
the target-specific image path, installs optional `package_data` relative to
`package_data_root`, and produces the single `.update.tar.zlib` plus
`.firmware.json`. For Loader role it creates recovery from standard native outputs and owns
`FirmwareReleaseInfo`, `DefaultInfo`, and the `release` output group for the
deliverable. CI and release tags bind to `:package`, so release builds always
traverse the native `:firmware` dependency before packaging.

Every build/ownership unit declares its sources and dependency edges in its own
`BUILD.bazel`. Add or update that local package when a source boundary or
dependency changes. The validator checks directory coverage, rule kinds, and
artifact identity, but it does not generate dependency edges.

## Linux test coverage report

Run `make bazel-coverage-report` from the repository root on Linux x86_64. The
entry queries the `test_coverage` configured graph for every compatible
automatic test and every compatible `cc_library` and `cc_binary` target,
then runs `bazel coverage` against those compatible test labels. The
instrumentation filter is derived from the source directories declared by those
targets, so there is no per-target coverage list. A new target participates by
following the existing compatibility, `manual` test-tag, and local
`srcs`/`hdrs` contracts. Coverage is reported separately under each target's
`cc_library/<name>` or `cc_binary/<name>` identity; the Bazel package is included
to distinguish repeated target names.

Instrumented cache-safe Bazel actions retain the configured local or remote
action cache. Test results never do: the coverage command sets
`--cache_test_results=no`, and the report rejects any cached test reported by
the Build Event Protocol stream.

Successful output is written under `build/coverage/test/`:

- `tests.json` reconciles every expected test with its terminal result, runs,
  attempts, shards, duration, and cached-action count.
- `targets.json` lists every eligible C/C++ target as `measured`, `uncovered`,
  or reasoned `not-applicable`. Uncovered targets have unavailable
  metrics instead of a fabricated zero percentage.
- `coverage.dat` and `report/index.html` provide the combined machine and HTML
  C/C++ production report.
- `summary.md` presents test totals, target-accounting totals, per-target
  coverage, and line, function, and available branch rates for the measured scope.

GitHub Actions appends the summary to the private run and uploads the directory
as the private `test-coverage` artifact even when the job fails. A partial
artifact is diagnostic only. The report is Linux Host evidence; it does not
claim macOS, Windows, firmware, device, network, audio, or rendered UI coverage.

## Web archive preview

Web `pkg_tar` owners can expose a local preview target with `web_archive_serve(name = "serve", archive = ":<archive>")` from `//tools/bazel:web_archive.bzl`. The runner extracts the archive into a temporary directory, rejects unsafe tar members, applies the archive's `_headers` policy, serves WASM with `application/wasm`, and removes the temporary directory when it exits. Development-only cross-origin requests can opt into the same-origin path `/_h2/http-proxy?url=<encoded-url>` by passing one `--http-proxy-origin <scheme://host:port>` flag for every exact allowed upstream origin. The proxy accepts GET, HEAD, POST, PUT, PATCH, and DELETE, uses a 60-second upstream timeout, never follows upstream redirects, and limits each request and response body to 64 MiB. It strips connection, length, cookie, origin, referrer, host, transfer-encoding, and `Sec-*` transport/browser headers as applicable while forwarding other request and response headers. The proxy is disabled without an allowlist and rejects credentials or fragments in target URLs. An artifact must explicitly select that path through a value such as `Module.h2WebHttpProxyUrl = "/_h2/http-proxy?url="`; production hosting should provide the required CORS policy or an equivalent trusted proxy. H2Loader is now distributed as `@gizclaw/h2loader`; its product frontend and local preview belong to `GizClaw/www`, not a GizOS `pkg_tar` target.

CI targets use platform compatibility to expose the graph that belongs to the selected execution class. CI does not compute an affected target list. Each static platform job directly invokes:

```sh
make bazel-build BAZEL_CONFIG=<config>
make bazel-test BAZEL_CONFIG=<config>
```

Host Tool and Artifact compatibility are the platform source of truth. `generate_graph.py --check` verifies BUILD ownership, rule kinds, firmware artifact identity, and public command wiring; it does not maintain tag-based target inventories or decide which changed targets CI should run.

## Full graph CI and native parallelism

Same-repository pull requests, `main` pushes, and manual runs use the same complete static platform matrix and GCS Writer cache. Pull-request `opened` and `synchronize` events trigger CI directly, so each distinct pull-request head receives one matrix as normal checks; reopening an unchanged head does not rerun it. Ownership approval remains an independent merge gate and never delays or dispatches the build. Linux, macOS, Android, and Windows each run Build followed by Test on one runner so Test reuses the same Bazel server and output base. Android package validation switches the top-level target platform to Linux x86_64, so it creates a second configured graph, but compatible dependency actions still reuse content-addressed results from the preceding Android Build. iOS retains independent parallel Build and Test tasks. All complete Build tasks use minimal remote output downloads; focused validation explicitly materializes any artifact it reads. Target compatibility skips other platforms, Bazel's special `manual` tag keeps hand-run tests out of wildcard target patterns, and content-addressed action keys decide which completed outputs can be reused.

ESP32-S3, ESP32-P4, and BK7258 configs automatically select all firmware endpoints for that platform. Each native firmware action declares 4 CPU and 4096 MiB to Bazel and passes a four-job limit to the underlying ESP-IDF or BK build. Bazel therefore schedules one action on a four-core runner and more independent launcher actions as a runner gains CPU and memory. Increasing a single runner size improves firmware throughput without creating one GitHub matrix entry per launcher.

Repository variables select the provisioned organization Larger Runner for each
execution class:

- `FIRMWARE_LINUX_RUNNER`: 2-core Linux for ESP32-P4 and the CI required aggregator;
- `FIRMWARE_LINUX_4CORE_RUNNER`: 4-core Linux for Android Build/Test, Test coverage, and KickPi K4B;
- `FIRMWARE_LINUX_8CORE_RUNNER`: 8-core Linux for Linux Build/Test, ESP32-S3, and BK7258;
- `FIRMWARE_MACOS_RUNNER`: 5-core macOS 15 arm64 for macOS Build/Test;
- `FIRMWARE_WINDOWS_RUNNER`: 8-core Windows Server 2025 for Windows Build/Test.

The 4-core and 8-core Linux runner groups must each allow at least three
concurrent jobs; the macOS and Windows groups each need one. Every variable
must resolve to a Ready runner before the workflow is pushed. There is no
standard-runner fallback.

These native SDK actions remain unsandboxed and execute only on the current
runner, while their declared repository inputs and firmware outputs participate
in the local disk cache and authenticated GCS remote cache. SDK and toolchain
paths are runtime locators, not stable external identities. ESP-IDF and both BK
repositories verify SDK commits against action-input version files.
`native_versions/bk_toolchain_archives.txt` binds each supported host URL to an
archive SHA-256 and expanded-tree SHA-256 before the downloaded toolchain is
published. The scrubbing config omits only the six generated locator files and
only for the three native firmware mnemonics; committed identities, sources,
configuration, archives, and outputs retain normal Bazel cache semantics.

## Native compiler cache

ESP32-S3, ESP32-P4, and BK7258 native actions can use a second cache
inside the SDK build. `H2_NATIVE_CCACHE_RUNTIME_ROOT` points a repository rule
at one runtime directory containing `runtime.json`, the ccache/helper binaries,
the cache directory, and an optional token file. CI and Release pin the
ccache and HTTPS storage-helper downloads in
`native_versions/native_ccache_tools.txt`; they do not use a runner package or
GitHub Actions cache snapshot for compiler objects.

GCS access is optional locally and complete-or-absent in `runtime.json`; none of
its individual paths or token fields is an action environment variable. The
base URL has the exact form
`https://storage.googleapis.com/<bucket>/ccache`. The runner derives
`ccache/esp` for S3/P4 and `ccache/bk` for BK7258, while
`CCACHE_NAMESPACE` keeps each target's compile keys logically separate. Bazel
continues to use the same bucket's independent `firmwares` prefix. Native
ccache never uses the Bazel layout or Bazel action metadata.

Each native action keeps its own temporary build tree for isolation. The
runner sets `CCACHE_BASEDIR` and disables CWD hashing so the random temporary
directory does not split otherwise identical compiler keys across launchers or
workflow runs. Compiler content, options, source, included configuration, and
generated headers remain part of ccache's key.

The token file must be an absolute, current-user-owned regular file with mode
`0600`. Its contents are read only inside the native action and are never
printed. Same-repository PR, `main`, manual, and Release jobs mint a fresh
Writer token immediately before firmware compilation. A build that receives
H2Loader Wi-Fi credentials sets ccache read-only. Cache misses and remote
storage errors fall back to the real compiler and cannot replace SDK,
toolchain, checkout-cleanliness, output, package, or Release validation.

## Mobile E2E

[Packaged mobile runners](mobile_e2e.md) share simulator/emulator lifecycle, artifact identity and failure evidence through a direct Python Bazel test. Suite fixtures, phase plans and qualification assertions remain suite-owned.
