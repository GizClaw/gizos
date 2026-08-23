# GizOS

GizOS is a portable firmware and application platform for embedded devices, desktop systems, mobile platforms, and WebAssembly. It provides reusable runtime, platform abstraction, application, board-support, build, packaging, and validation foundations without coupling the public platform to a specific commercial product.

## Migration checklist

Each unchecked item represents exactly one future migration push. The item is checked in the same push that adds its code.

- [x] Create the Apache-2.0 repository, Bazel workspace, Make entry points, and CI framework.
- [x] Migrate the PAL contracts, Runtime, foundational libraries, host providers, portable drivers, and current public board/target packages.
- [x] Add the remaining pinned third-party sources and Bazel overlays.
- [x] Add the remaining portable libraries and non-composition PAL providers.
- [x] Complete Desktop PAL composition and host platform support.
- [ ] Add ESP-IDF 6.x Bazel runners and shared native components.
- [ ] Add BK7258 Bazel runners and shared AP/CP native components.
- [ ] Complete the existing public board targets and add AMOLED, DevKit, and SZP.
- [ ] Migrate the complete public H2Loader core, CLI, Loader, Web SDK, and package flow.
- [ ] Migrate public Example and reusable E2E Apps with public launchers.
- [ ] Migrate PIXA Games and Showcase with public Desktop artifacts.
- [ ] Complete public tools, guides, release checks, and the final source-boundary audit.

H106, H200/H200 V2, Tiga, Zero, TapDoki, Lucky Kitty, and every board, target, asset, test, guide, or release entry owned by those products remain private.

## Repository layout

```text
.
├── boards/                 # Public physical-board support packages
├── guides/                 # Architecture, development, usage, and review guides
├── libs/                   # Target-independent reusable libraries and PAL providers
├── native_component_src/   # Components compiled by native platform SDKs
├── projects/               # Portable applications and their public artifacts
├── scripts/                # Stable repository command implementations
├── third_party/            # Pinned upstream sources and integration metadata
└── tools/                  # Host-side build, packaging, and support tools
```

Public source packages must not depend on private product repositories, credentials, services, assets, or board definitions. Product-specific applications and hardware integrations remain in their owning repositories and may depend on released GizOS interfaces; GizOS does not depend on them. Native SDK/toolchain access is a separate build-environment dependency and does not permit private product source to enter this repository.

## Build system

GizOS uses Bazel as the source of truth for supported build graphs, tests, tools, and artifacts. Bazel 9.2.0 is selected by `.bazelversion`; the top-level Make targets provide the stable repository command surface.

```sh
make help
make bazel-build BAZEL_CONFIG=linux_x86_64
make bazel-test BAZEL_CONFIG=linux_x86_64
```

The Linux, macOS, and Windows CI jobs analyze and build the complete compatible graph, and run every compatible automatic test. Fork pull requests run without private credentials. Same-repository CI additionally verifies read-only access to the private SDK development environment used by future firmware build classes.

The host-compatible public Bazel graph must pass from a clean clone without private source repositories, credentials, services, product assets, or private board definitions. Native firmware CI obtains SDK and toolchain inputs through the read-only development-environment integration.

## License

GizOS is licensed under the [Apache License 2.0](LICENSE).
