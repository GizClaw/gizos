# H2Loader libraries

Shared H2Loader code is organized by the side of the connection that consumes
it:

```text
libs/h2loader/
├── host/       # Device discovery, commands, package transfer and lifecycle
└── device/     # Device protocol, package/boot policy and portable session engine
```

The native CLI and its OS adapters belong to
[`tools/gizloader`](../../tools/gizloader). Loader App entry, firmware startup,
SDK composition and package artifacts belong to
[`projects/h2loader`](../../projects/h2loader). E2E orchestration and fixtures
belong to [`projects/testing`](../../projects/testing).

## Host

`host` provides device discovery, typed commands, authoritative status,
package validation/writing and managed lifecycle over the H2Loader protocol. Its
BLE client can also be used by a factory peer acting as the Host. Native CLI
and browser bindings share this policy; neither owns another copy.

The main Bazel library is `//libs/h2loader/host`. Transport and operating-system
capabilities are injected through PAL. The public C interface remains in
`host/include`.

## Device

`device` owns package validation, Stage, image identity, boot intent,
confirmation, rollback, commands, diagnostics and a portable serial/KCP session
engine. Timing, admission, allocator, package-entry and recovery requirements
are configured through capabilities and policy. Runtime and Loader/App
orchestration belong to the project service described below.

The core `//libs/h2loader/device` uses PAL and portable libraries without a
Runtime or project dependency. Runtime/Loader App composition belongs to
[`projects/h2loader/libs/device_service`](../../projects/h2loader/libs/device_service).
Caller-owned service state and borrowed providers must outlive tasks and pending
callbacks. Firmware targets own product and task policy.
They consume `Runtime.io_stream` and `Runtime.mem_external`; board and native
providers select and configure the physical transport and optional external heap.

Native SDK operations stay in platform PAL providers under
[`native_component_src`](../../native_component_src). Providers do not import
H2Loader roles, manifests, Stage or preference keys. Board wiring and partition
geometry stay in [`boards`](../../boards). SDK builds use the shared
`:firmware_sources` and `:firmware_api` inventory through the generic
`firmware_lib_component` rule. It compiles portable sources with the SDK C runtime
and keeps archive ABI validation enabled; no per-chip Loader wrapper is needed.
BK's board recovery profile retains its separate SD work area and retry policy;
manufacturing can extend that profile with business providers before Runtime init.

Image PAL distinguishes stored artifact bytes from optional native executable
views. ESP image bytes, BK encoded physical bytes and JieLi UFW shadow bytes
retain their existing verification domains. Unknown exact length remains
unknown. Accepted H2 roles map to requested execution windows above PAL; native
providers independently validate the executable and enforce Flash guards.

Updates write and finish a prepared candidate, verify destination bytes, commit
H2 metadata, prepare native boot material, persist required evidence, then select
and reboot. Finish never selects. Explicit abort remains valid after finish.
Internal cleanup before a new write preserves any published recovery header;
selecting a confirmed running recovery slot is a verified barrier before
canonical erasure.

Legacy persistent records remain compatibility codecs in the device library.
The 112-byte JieLi pending record retains its exact byte layout and SHA binding;
providers receive only versioned opaque native evidence. This compatibility
format does not create a separate device service.

Stable identity, image access, boot state, heap observations, filesystem
preparation, watchdog feeding, stream configuration/release, synchronization,
tasks and timing use public PAL APIs. SHA-256 uses the shared portable
implementation. Platforms requiring watchdog cooperation retain positive PAL
sleep delays between digest chunks.

Key core files are [`h2_loader_boot.c`](device/src/h2_loader_boot.c),
[`h2_loader_package.c`](device/src/h2_loader_package.c), and
[`h2_loader_pal.h`](device/include/h2_loader_pal.h).

## Web

Browser JS/runtime/WASM bindings belong to
[`projects/h2loader/libs/web`](../../projects/h2loader/libs/web), outside these
portable libraries. Its runtime and JavaScript are packaged together as
`@gizclaw/h2loader` by `projects/h2loader/targets/npm_package/h2loader`. The
product Batch Loader UI belongs to `GizClaw/www`; native CLI code remains in
`tools/gizloader`.

## Validation

Run Host and Device tests using the build machine's host configuration, for
example:

```bash
bazel test --config=macos_arm64 //libs/h2loader/host:all //libs/h2loader/device:all
```

Browser protocol and port-registry tests are in `web`; native provider fixtures
are beside their platform components. Full firmware builds require a configured
vendor SDK/toolchain and supported native host. Host tests and build-graph
analysis do not establish hardware acceptance.
