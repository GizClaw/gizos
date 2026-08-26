# Bazel CI dependency graph

This directory owns the native Bazel build/ownership graph used to compile
stable host C/C++ and Go packages and every supported firmware platform.
Native packages use real `cc_library`, `cc_binary`, `cc_test`, `go_library`,
`go_binary`, `go_test`, and Apple application targets. All maintained ESP-IDF
launchers use the local-only `esp_idf_firmware` external rule and all maintained
BK7258 launchers use `bk7258_firmware`, and the three maintained BK3633 entries
use `bk3633_firmware`; fixed non-build endpoints may still use `filegroup`.
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

The three runners turn this same graph into invocation-local native manifests.
ESP-IDF receives ordered component directories/names and direct source lists,
BK7258 receives isolated AP and CP component directories/source lists, and BK3633
receives ordered first-party sources, include roots, and archives. Native CMake or
Make retains generated SDK configuration, SDK/vendor source selection, compiler
and linker policy, bootloader/partition/Stack/BIM/merge mechanics. It must not
scan the repository or fall back to a handwritten first-party inventory. Graph
validation fixes the maintained endpoint inventory at 53 ESP, 17 BK7258, and 3
BK3633 entries.

## Local usage

Use the exact Bazel version from `.bazelversion`:

```sh
bazel test //tools/bazel/...
bazel test //tools/bazel:bk7258_runner_test //tools/bazel:bk3633_runner_test //tools/bazel:esp_idf_runner_test //projects/h2loader/tools/bazel:h2loader_tar_zlib_runner_test
make bazel-build BAZEL_CONFIG=linux_x86_64
make bazel-test BAZEL_CONFIG=linux_x86_64
```

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

Source `.env/devenv` before requesting an ESP build. The sibling
`firmware-devenv` provider exports `IDF_PATH` and
`IDF_TOOLS_PATH` for Bazel. The tools repository derives the unique
`python_env/idf6.0_py*_env` installation from that tools root, validates it
against the pinned ESP-IDF constraints, and materializes versioned SDK/tool
locators. The native action receives those locator files instead of the
machine paths or caller `PATH`.

```sh
REPO_ROOT=$PWD
export REPO_ROOT
. .env/devenv
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

The action copies only the selected launcher project into an invocation-local
temporary tree, points it at the read-only repository source, and invokes the
native `idf.py build` followed by `idf.py merge-bin`. It is explicitly
unsandboxed and ineligible for remote execution, while its declared outputs may use the local disk cache or the
authenticated remote cache. The runner constructs a fixed subprocess
environment from the validated locators, pinned SDK version, and the
`FirmwareVersionInfo` selected by the firmware target. Product-owned final
firmware should declare `firmware_version(name, value)` and pass its label with
the public `version` attribute. Firmware declarations that omit `version`
continue to use the `//tools/bazel:firmware_version` compatibility build
setting. A firmware target may explicitly opt in to the allowlisted
`H2LOADER_WIFI_CREDENTIALS` JSON object containing exact `ssid` and `password`
fields; the runner validates it and exposes the two values only to that
target's ESP-IDF subprocess. Unrelated ESP-IDF subprocesses never receive them. The
runner neither activates the SDK nor performs
flash, erase, monitor, reset, serial, network, or installation operations.

## BK7258 external builds

Source `.env/devenv` before requesting a BK7258 build. The canonical provider
exports only `BK7258_PATH`. A repository
rule validates SDK commit `aa5df964b0f64924ee6d0d2ffd6c3ca6ed59f9ca` and a
clean tracked checkout. Another repository rule downloads Arm GNU
10.3-2021.10, verifies the committed archive and expanded-tree SHA-256 values,
and exposes compiler version `10.3.1` through a locator. The action receives
those locators and a Bazel Python runtime; it does not inherit SDK, toolchain,
archive, Python, or `PATH` variables from the caller.

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

Every package below `projects/<project>/targets/h2loader_tar_zlib/<app>/<board>/`
declares an internal `:firmware` target using `h2loader_esp_idf_firmware` or
`h2loader_bk7258_firmware`, followed by the public `:package` target using
`h2loader_tar_zlib`. The H2Loader wrappers select partition and recovery inputs
from the declared `target` and `board`, then delegate native compilation to the
generic rules in this directory. The package rule consumes `FirmwareInfo` or `Bk7258FirmwareInfo`, writes
the target-specific image path, installs optional `package_data` relative to
`package_data_root`, and produces the single `.update.tar.zlib` plus
`.firmware.json`, both named `<project>-<app>-<board>` directly by the rule. The
chip target remains in metadata instead of being repeated in the filename. For
Loader role it creates a same-stem recovery bundle from standard native outputs and owns
`FirmwareReleaseInfo`, `DefaultInfo`, and the `release` output group for the
deliverable. CI and release tags bind to `:package`, so release builds always
traverse the native `:firmware` dependency before packaging. Consumers copy
these canonical outputs unchanged and do not parse labels or rename assets.

## BK3633 external builds

Final BK3633 targets use
`projects/<project>/targets/bk3633_firmware/<app>/<board>`. The rule keeps the
native `app.bin`, ELF, map, merge image, and manifest, and additionally declares
`<project>-<app>-<board>.bin` as the canonical release image.

Source the sibling `firmwares-devenv/export.sh` before requesting a BK3633
build. The provider exports only `BK3633_PATH`; Bazel validates it and supplies
the same downloaded, content-verified Arm toolchain locator used by BK7258. The
runner verifies SDK commit
`c963da8e73440400ee6839b4bfd20ca6e6ec7908`, ARM GCC `10.3.1`, required
allroles Stack/BIM inputs, and unchanged SDK state. The native Makefile keeps
compile, link, stack, layout, BIM, and merge semantics, while Bazel injects the
exec-configured `//tools/bk3633_binconverter:binconverter` executable.
The rule's logical graph contributes declared direct sources, public include
roots, metadata, and graph-reachable Bazel archives to the native action key.
The runner writes those values to an action-local Make include; shared Make keeps
only SDK source selection and final build mechanics, not a second first-party
source or archive list.

```sh
. ../firmwares-devenv/export.sh
bazel build --config=bk3633 //...
```

`Bk3633FirmwareInfo`, `DefaultInfo.files`, and the `release` output group expose
the same five files under each target's `firmware/` output directory:
`firmware.elf`, `app.bin`, `firmware.map`, `merge-crc.bin`, and normalized
`manifest.json`. Only `merge-crc.bin` is a complete direct-flash recovery image;
the rule does not return `FirmwareReleaseInfo` or create an H2Loader package.
The action is unsandboxed, ineligible for remote execution, locally or remotely
cacheable, invocation-isolated, and does not perform flash, reset, serial,
network, or device operations.

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

Web `pkg_tar` owners can expose a local preview target with
`web_archive_serve(name = "serve", archive = ":<archive>")` from
`//tools/bazel:web_archive.bzl`. The runner extracts the archive into a temporary
directory, rejects unsafe tar members, applies the archive's `_headers` policy,
serves WASM with `application/wasm`, and removes the temporary directory when it
exits. H2Loader is now distributed as `@gizclaw/h2loader`; its product frontend
and local preview belong to `GizClaw/www`, not a GizOS `pkg_tar` target.

CI targets use platform compatibility to expose the graph that belongs to the selected execution class. CI does not compute an affected target list. Each static platform job directly invokes:

```sh
make bazel-build BAZEL_CONFIG=<config>
make bazel-test BAZEL_CONFIG=<config>
```

Host Tool and Artifact compatibility are the platform source of truth. `generate_graph.py --check` verifies BUILD ownership, rule kinds, firmware artifact identity, and public command wiring; it does not maintain tag-based target inventories or decide which changed targets CI should run.

## Full graph CI and native parallelism

Same-repository pull requests, `main` pushes, and manual runs use the same complete static platform matrix and GCS Writer cache. Pull-request `opened` and `synchronize` events trigger CI directly, so each distinct pull-request head receives one matrix as normal checks; reopening an unchanged head does not rerun it. Ownership approval remains an independent merge gate and never delays or dispatches the build. Linux, macOS, Android, and Windows each run Build followed by Test on one runner so Test reuses the same Bazel server and output base. Android package validation switches the top-level target platform to Linux x86_64, so it creates a second configured graph, but compatible dependency actions still reuse content-addressed results from the preceding Android Build. iOS retains independent parallel Build and Test tasks. All complete Build tasks use minimal remote output downloads; focused validation explicitly materializes any artifact it reads. Target compatibility skips other platforms, Bazel's special `manual` tag keeps hand-run tests out of wildcard target patterns, and content-addressed action keys decide which completed outputs can be reused.

ESP32-S3, ESP32-P4, BK7258, and BK3633 configs automatically select all firmware endpoints for that platform. Each native firmware action declares 4 CPU and 4096 MiB to Bazel and passes a four-job limit to the underlying ESP-IDF or BK build. Bazel therefore schedules one action on a four-core runner and more independent launcher actions as a runner gains CPU and memory. Increasing a single runner size improves firmware throughput without creating one GitHub matrix entry per launcher.

Repository variables select the provisioned organization Larger Runner for each
execution class:

- `FIRMWARE_LINUX_RUNNER`: 2-core Linux for ESP32-P4, BK3633, and the CI required aggregator;
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

ESP32-S3, ESP32-P4, BK7258, and BK3633 native actions can use a second cache
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
`ccache/esp` for S3/P4 and `ccache/bk` for BK7258/BK3633, while
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
