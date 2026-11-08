# GizOS Claude instructions

@AGENTS.md

Read only relevant files from `guides/index.md`. Batch related edits and perform only the validation requested by the user.

## Directory structure constraints

Keep naming, nesting and file-placement rules here. Each directory entry below states its purpose, then lists included and excluded code or document content.

- Board paths use `boards/<manufacturer>/<model>/<chip-or-target>/`. Keep all chips and AP/CP cores under one real board; do not create compile-only chip boards.
- PAL headers live directly under `pal/include/{application,core,hal,net,os}/`, with umbrella headers at `pal/include/`. Use includes such as `application/h2_pal_http.h`; do not add an extra `h2/pal` namespace layer. Default implementations live directly in `pal/src/unsupported_<capability>.c`, without an `unsupported/` subdirectory.
- Group providers under `pal/providers/{platforms,backends,devices}/`. The pthread atomic backend belongs to `pal/providers/platforms/posix/atomic/`.
- Group libraries by capability: `net`, `sync`, `memory`, `container`, `archive`, `audio`, `video`, `image`, `ui`, `clients`, `drivers` and `math`. Retain established `encoding`, `crypto`, `command`, `semver`, `lua`, `runtime` and `h2loader` ownership; clearly named standalone packages may remain directly under `libs/`. Each leaf owns public `include/`, private `src/`, local `tests/`, resources and explicit BUILD inputs. Do not create catch-all `utils` packages or legacy forwarding paths.
- Native SDK components may have execution-unit subdivisions where required. SDK compilation alone does not change source ownership.
- H2Loader host and build tooling lives directly in `tools/bazel/h2loader/`, including package/recovery writers, Bazel rules and release assembly. `projects/h2loader/` owns its Apps, project libraries and final artifact entries; do not nest a second tooling owner there.
- H2Loader Apps live directly in `projects/h2loader/apps/{headless,mfg}/`, each with its own `include/`, `src/`, `tests/` and BUILD. Manufacturing helpers belong to `projects/h2loader/libs/{mfg-peer,mfg-aec-source}/`, and MFG final entries to `projects/h2loader/targets/<artifact-kind>/mfg/`. A named or role App is already an App root: use `iperf/{client_app,server_app,libs}` and `lua_link/{host_app,join_app,libs}`, without another nested `app/`. Shared repository command logic belongs to `scripts/common/`.
- The first directory under every `targets/` names the final artifact kind, such as `cc_binary`, `cc_test`, `h2_loader_tar_zlib`, `android_binary`, `ios_application`, `pkg_tar` or `npm_package`. Internal ESP/BK/JieLi compilation rules remain dependencies inside the entry BUILD file. MFG `profile.c` belongs beside its concrete board entry. Both package formats share one entry directory and native image graph; do not add separate source trees or legacy entry aliases.
- Keep patches owned by `third_party/` under `third_party/<package>_patch/` with a package-local BUILD and explicit labels. Related compatibility changes for one pinned upstream may share a patch file; do not scatter patch files at the `third_party/` root.
- Keep source tests with their owner and create only directories with owned content. Caches, SDK installations and generated outputs are not source dependencies.

## Directory ownership

Describe each directory with its responsibility and purpose, followed by exactly two points: included content and excluded content.

### `boards/<manufacturer>/<model>/<chip-or-target>/`

Owns one physical board so wiring and hardware choices have a single source of truth.

1. **Include:** Physical pin and bus definitions, peripheral configuration, power sequencing, defaults, BSP interfaces, layout geometry and local tests.
2. **Exclude:** Portable algorithms, App business logic, Runtime lifecycle management and final artifact policy.

### `pal/`

Owns the platform abstraction boundary so contracts and concrete implementations remain distinct.

1. **Include:** Public capability contracts, canonical unsupported behavior, concrete capability implementations and their build declarations.
2. **Exclude:** App business logic, physical board wiring and concrete implementation dependencies in the public contract package.

### `pal/include/`

Defines stable platform capabilities so portable callers do not need implementation details.

1. **Include:** Public capability interfaces, opaque types, result semantics, ownership and lifetime contracts.
2. **Exclude:** SDK-specific types, provider-private data structures or implementation logic, board wiring definitions and business policy.

### `pal/src/`

Provides canonical unsupported implementations so unavailable capabilities have explicit, consistent behavior.

1. **Include:** Canonical fallback implementations that explicitly report unavailable capabilities through the public PAL contract.
2. **Exclude:** Native OS operations, device-specific behavior and success responses that conceal missing capabilities.

### `pal/providers/platforms/<platform>/`

Assembles a platform's usable PAL capabilities so OS details stay behind public contracts.

1. **Include:** Native OS/runtime adapters, platform capability assembly, OS synchronization primitives and reusable backend selection. Absent capabilities remain explicitly unsupported.
2. **Exclude:** Physical board wiring, App policy, target-owned Runtime lifetime and copies of the same backend for each OS.

### `pal/providers/backends/<library>/`

Adapts reusable libraries and protocols to PAL so platforms can share one implementation.

1. **Include:** Backend adapters, private implementation headers, upstream integration and local tests.
2. **Exclude:** Board wiring, App business logic, final artifact assembly and OS-specific copies of reusable backend logic.

### `pal/providers/devices/<family>/`

Adapts a concrete device family to PAL so module behavior can be reused across boards.

1. **Include:** Modem, NFC and similar device protocols, adapters and local tests using injected transport capabilities.
2. **Exclude:** Physical pin assignments, board power wiring, product business logic and final artifact policy.

### `libs/<domain>/`

Groups reusable packages by capability so callers can find their actual owner.

1. **Include:** Portable networking, synchronization, memory, container, archive, media, UI, service-client, driver and numeric capabilities.
2. **Exclude:** Native OS/SDK adapters, concrete PAL provider implementations, board wiring, App business policy and unrelated cross-domain helpers.

### `libs/<domain>/<package>/`

Owns reusable, platform-independent code so the same package works across targets.

1. **Include:** Public interfaces, portable implementations, local tests, package resources and declared build inputs. Depend only on public PAL contracts, portable libraries and portable third-party code; pure algorithms need no artificial PAL dependency.
2. **Exclude:** Concrete PAL provider implementations or production dependencies on them, native OS/SDK calls, board wiring, project-specific business logic and platform-bound browser bindings.

### `libs/testing/`

Shares portable test infrastructure so source owners can reuse test support.

1. **Include:** Reusable assertions, case runners and test support. Use explicit `testonly` facades when library tests need real providers.
2. **Exclude:** Runnable DUT acceptance programs, manufacturing Apps, board profiles and external-service test programs.

### `native_component_src/<sdk-family>/`

Owns shared native SDK components so compilation and registration integrate with the SDK in one place.

1. **Include:** Native PAL implementations, SDK startup and registration code, and shared native SDK integration.
2. **Exclude:** Portable algorithms, App business logic, physical board wiring and final artifact policy.

### `projects/<owner>/`

Owns a product or test suite so its behavior, integrations and artifacts stay together.

1. **Include:** Apps, project-specific integrations, resources, named shared libraries, local tests and final artifact entries.
2. **Exclude:** Copies of public PAL, portable libraries, shared native SDK components and reusable physical BSPs.

### App directories: `apps/<name>/`, `app/`, `client_app/`, `server_app/`, `host_app/`, `join_app/`

Own portable App behavior so final entries can compose it for different targets.

1. **Include:** App entry points, contracts, headers, implementation and local tests using Runtime and portable public libraries.
2. **Exclude:** SDK-specific handles and operations, physical board wiring, Runtime construction and target-specific task policy.

### Project and suite `libs/<package>/`

Shares named code within its project or suite so related Apps reuse one implementation.

1. **Include:** Project-owned shared logic, service composition, bindings and resources. Examples include MFG helpers and the H2Loader Device service.
2. **Exclude:** Unrelated business logic, copies of public library implementations and physical board wiring.

### `projects/<owner>/targets/`

Groups final artifacts by kind so entry locations are predictable.

1. **Include:** Build declarations composing host executables, firmware packages, mobile Apps and web or release artifacts.
2. **Exclude:** App business logic, reusable algorithms, native peripheral implementations and physical board wiring.

### `projects/<owner>/targets/<artifact-kind>/.../`

Composes one final artifact so startup, policy and packaging have an explicit owner.

1. **Include:** Runtime lifetime, startup/exit, logical-to-physical mapping, task policy, native configuration, board-specific MFG profiles and packaging declarations.
2. **Exclude:** Portable App business logic, reusable driver implementations and physical board wiring.

### `projects/h2loader/apps/headless/`

Owns the headless Loader App so device service composition can reuse one portable implementation.

1. **Include:** Loader App behavior, public contracts, command composition and App-local tests.
2. **Exclude:** Manufacturing acceptance, native SDK operations, physical wiring, Runtime construction and final task or package policy.

### `projects/h2loader/apps/mfg/`

Owns manufacturing acceptance as an H2Loader App so the common program can serve multiple board entries.

1. **Include:** Portable acceptance logic, test orchestration, public contracts and App-local source tests; named manufacturing helpers remain in H2Loader project libraries.
2. **Exclude:** Board-specific profiles, physical wiring, final artifact assembly and private product manufacturing business logic.

### `projects/testing/unit/`

Checks one DUT's correctness so device-level behavior can be qualified independently.

1. **Include:** Single-DUT programs and their controller, AP or local service fixtures.
2. **Exclude:** Source-level unit tests, cooperating multiple-DUT suites, mandatory external-service tests and standalone benchmarks.

### `projects/testing/e2e/`

Checks cooperating DUTs so cross-device behavior is exercised end to end.

1. **Include:** Cooperating multiple-DUT test Apps, iPerf client/server behavior, Lua host/join behavior, shared suite logic and orchestration.
2. **Exclude:** Single-DUT checks whose only peers are a controller, AP or local fixture; those helpers do not add a DUT.

### `projects/testing/integration/`

Checks real external systems so service and network integration can be verified.

1. **Include:** Programs requiring real services such as GizClaw, OneNET, Lingxi or external DNS/TLS endpoints.
2. **Exclude:** Offline unit tests, local fixtures with no external dependency and hardware-only multiple-DUT suites.

### `projects/testing/benchmark/`

Measures standalone performance so benchmarks have a distinct purpose and entry.

1. **Include:** Standalone throughput, latency, resource-use and media performance programs.
2. **Exclude:** Functional correctness checks and behavior requiring cooperating multiple DUTs, including the paired iPerf benchmark.

### `tools/`

Owns host and build tooling so development automation stays outside firmware libraries.

1. **Include:** Host CLIs, environment management, generators, native image transformations, shared build rules and platform-bound tool support.
2. **Exclude:** Firmware App business logic, physical board wiring and duplicated portable library implementations.

### `tools/bazel/h2loader/`

Owns H2Loader host tooling so portable libraries and final entries share package and release support.

1. **Include:** H2Loader Bazel integration, package and recovery writers, canonical release assembly, host command support and their local tests/fixtures.
2. **Exclude:** Firmware App business logic, physical wiring, Runtime lifetime, final task policy and native SDK driver implementations.

### `scripts/`

Owns repository command entry points so common workflows have thin, reusable orchestration.

1. **Include:** Repository command entry points and reusable workflow orchestration.
2. **Exclude:** A second build graph, product business logic and duplicated source inventories.

### `third_party/`

Integrates pinned upstream dependencies so external code remains identifiable and reproducible.

1. **Include:** Upstream source integration, build overlays, immutable dependency pins, licenses and necessary patches.
2. **Exclude:** First-party business logic, BSP wiring and external-repository dependencies back into first-party GizOS owners.

### `guides/`

Records concise AI development constraints so agents can find the relevant rules quickly.

1. **Include:** A reading-condition index and concise ownership, dependency, coding, build and review constraints.
2. **Exclude:** App prototypes, Product IA, architecture diagrams, tutorials, product documentation, API mirrors and session reports.

## Migration rules

Firmware entries use one `firmware_lib_component` and explicit `firmware_native_component` inputs. Do not maintain a second source inventory.

Keep implementation headers and SDK handles private. Target/BSP/platform assembly selects providers and injects stable public capabilities; production libraries do not select concrete providers.

Keep source tests with their owner. Preserve public C/C++ APIs, logical archive/component names, wire/package board, image, role and version identities. Update current includes, labels, scripts and documentation together; retain immutable historical evidence, qualification bytes/hashes and fixed-revision recipes.

Check real artifacts and known downstream consumers before deleting libraries or resources. Own tests alone do not establish a production consumer; shared fonts and downstream-used libraries are not unused. Preserve existing published artifacts and upstream source style.
