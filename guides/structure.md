# Code ownership and layout

Read before adding or moving code. Follow [development rules](development.md) for behavior and the [Bazel guide](../tools/bazel/README.md) for build integration.

| Root | Owns |
| --- | --- |
| `libs/<library>/` | Reusable capabilities without a specific project owner; public `include/`, private `src/`, local `tests/`, and `BUILD.bazel`. |
| `libs/pal/` | Public PAL contracts and canonical unsupported objects; no platform backends or test fakes. |
| `libs/pal/providers/<platform-or-library>/` | Reusable platform implementations compiled by Bazel, including third-party PAL adapters. |
| `native_component_src/<sdk-family>/` | Shared components compiled by the native SDK, including SDK configuration, registration, and lifecycle glue. |
| `boards/<board>/<chip-or-target>/` | Physical wiring, peripheral IDs, hardware defaults, BSP APIs, and hardware/partition layout inputs. |
| `projects/<project>/` | Apps, project-owned libraries and adapters, assets/data, policy, and final artifact entries. |
| `third_party/` | Upstream overlays, patches, and necessary vendored sources; immutable public dependency pins belong in `MODULE.bazel`. |
| `tools/` | Host tools, generators, and reusable build rules. |
| `scripts/` | Public Make target implementations; shared command logic belongs in `scripts/common/`. |
| `guides/` | Concise development constraints for AI changes; API contracts stay in public headers. |

## Apps, groups, and artifacts

- A standalone App uses `projects/<app>/app/{include,src}/`. A project group uses `projects/<group>/apps/<app>/app/{include,src}/`. App assets and package data stay with their owner.
- Project libraries use named `projects/<project>/libs/<library>/` packages with the same public/private/test boundaries as repository libraries. Family containers do not contain shared `include/` or `src/` trees.
- Keep reusable code with its explicit project owner, even when another project consumes its public API. Dependencies point from consumer to provider and must not form cycles.
- Final entries use `projects/<project>/targets/<artifact-rule>/<app>[/<variant>]/`. Name the root after the final delivery rule, not an intermediate compiler or packaging step.
- Add an artifact variant only when native builds need board isolation or distinct packages need the same local target name. Installing an artifact through another project does not transfer source ownership.
- The final entry selects App and platform, maps logical components to peripherals, owns Runtime lifetime, and supplies image configuration and task policy. Keep it thin; reusable behavior returns to its proper owner.
- Project-only SDK sources use `projects/<project>/native_component_src/<platform-or-target>/`; Bazel-compiled reusable code uses named `libs/` packages. Do not introduce a project `components/` ownership root.
- Keep all chips/cores of one physical board under the same board root. Native components may split by execution unit such as AP/CP. Board layouts do not own App policy or task scheduling.

## Platform and build boundaries

- Portable libraries do not own board wiring, firmware registration, or image policy. PAL backends implement capabilities; they do not choose an App or its startup policy. OS backends must not depend on another OS backend or Desktop composition.
- Use PAL providers for Bazel-compiled platform implementations and native components for SDK-owned compilation/registration. Do not create native components merely to forward a prebuilt archive.
- Each firmware entry owns one `firmware_lib_component` selecting its Bazel library closure. SDK-built sources use `firmware_native_component`; native build files consume that graph instead of scanning the checkout or maintaining another first-party inventory.
- Keep `libs/pal` one contract package. `//libs/pal:pal` is header-only; consumers of canonical unsupported accessors also select `//libs/pal:unsupported`. Atomic contracts/providers belong to `libs/atomic`, independently of PAL.
- A library's primary target matches its directory name. Split `_embed`, `_desktop`, `_mobile`, or `_web` targets only for real source, define, compatibility, or dependency differences; do not copy libraries per consumer.
- Upstream overlays must not depend back on first-party libraries, boards, components, projects, or tools. GizOS integration belongs to its first-party owner.
- Do not add generic root `src/`, `include/`, `common/`, or SDK-version source trees. Generated build/cache directories are not stable source or runtime inputs; checked-in generated contracts need an owner and reproducible generation.
