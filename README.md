# GizOS

GizOS is a portable firmware and application platform for embedded devices, desktop systems, mobile platforms, and WebAssembly. It provides reusable runtime, platform abstraction, application, board-support, build, packaging, and validation foundations without coupling the public platform to a specific commercial product.

## Status

GizOS is being migrated into this repository in independently buildable waves. The current public baseline contains the Bazel workspace, platform abstraction contracts and unsupported implementations, the portable Runtime, and the first target-independent utility library.

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

Public packages must build and test without access to private repositories, credentials, services, product assets, or board definitions. Product-specific applications and hardware integrations remain in their owning repositories and may depend on released GizOS interfaces; GizOS does not depend on them.

## Build system

GizOS uses Bazel as the source of truth for supported build graphs, tests, tools, and artifacts. Bazel 9.2.0 is selected by `.bazelversion`; the top-level Make targets provide the stable repository command surface.

```sh
make help
make bazel-build BAZEL_CONFIG=linux_x86_64
make bazel-test BAZEL_CONFIG=linux_x86_64
```

The Linux, macOS, and Windows CI jobs analyze and build the complete compatible graph, and run every compatible automatic test. Fork pull requests run without private credentials. Same-repository CI additionally verifies read-only access to the private SDK development environment used by future firmware build classes.

The public Bazel graph must pass from a clean clone without private repositories, credentials, services, product assets, or private board definitions.

## License

GizOS is licensed under the [Apache License 2.0](LICENSE).
