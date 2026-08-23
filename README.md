# GizOS

GizOS is a portable firmware and application platform for embedded devices, desktop systems, mobile platforms, and WebAssembly. It provides reusable runtime, platform abstraction, application, board-support, build, packaging, and validation foundations without coupling the public platform to a specific commercial product.

## Status

This repository is being bootstrapped. The initial commit establishes the public ownership roots before source code is imported and validated. Build and contribution instructions will be added together with the first independently buildable source snapshot.

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

GizOS uses Bazel as the source of truth for supported build graphs, tests, tools, and artifacts. The repository will not advertise build commands until the imported public graph passes from a clean clone without private dependencies.

## License

The source license has not been selected yet. A license file will be added before the first source-code release. Until then, publication of this repository does not grant permission to use, modify, or redistribute future source imports.
