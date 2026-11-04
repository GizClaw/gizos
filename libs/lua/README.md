# Lua Runtime

`libs/lua` owns the restricted Lua VM, the Runtime Host, built-in scripting modules, Lua App packaging and a versioned static documentation artifact. The embedding app owns Runtime configuration, native extensions, device capability selection and firmware validation.

## Documentation

- [Scripting reference](docs/runtime.md): core module signatures, parameters, results, constraints and named examples.
- [Host guide](docs/host.md): the existing Chinese Host configuration, lifecycle, scheduling, storage, audio and BLE Link explanations, relocated from the former Guides tree.
- [Numeric usage](docs/numeric_guide.md) and [Display usage](docs/display_guide.md): existing narratives and examples, relocated beside their source.
- [Display public contract](include/h2_lua_display.h) and [numeric public contract](include/h2_lua_numeric.h): authoritative advanced API contracts rendered directly into the package.
- [Documentation data package](targets/npm_package/lua-docs/README.md): schema, provenance, local assembly, browser/Go consumption and release procedure for `@gizclaw/lua-docs`.

These module documents have one maintained source each. The npm artifact derives its contents and index from them; it does not add an API website under `guides/`.

## Build and validation

Use the repository's host configuration (`macos_arm64` on Apple Silicon, `linux_x86_64` on Linux). Discover other targets from the live Bazel graph.

```sh
bazel test --config=macos_arm64 //libs/lua:lua_test
bazel test --config=macos_arm64 //libs/lua/targets/npm_package/lua-docs:package_test
```

The documentation test validates an artifact, not a device. Pin the package's recorded Runtime source revision against the embedding firmware's GizOS dependency, and verify its optional capabilities separately. Current platform source does not establish compatibility with released product firmware.
