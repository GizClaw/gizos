# Lua Cosmic Drift

`Lua Cosmic Drift` is a portable touch-first cosmic evolution game. The Lua script owns input, simulation, collisions, upgrades, particles, and rendering; the C App owns the Runtime, Lua host/job lifecycle, cancellation, and cleanup.

The Desktop target is `//projects/example/targets/cc_binary/lua-cosmic-drift:example-lua-cosmic-drift`. Click once to start, then move the pointer inside the window to steer without holding a mouse button. The AMOLED target is `//projects/example/targets/h2loader_tar_zlib/lua-cosmic-drift/amoled:package`; hold or drag on the touch surface to steer and press Boot to leave the App.

Consume cyan-ringed bodies, avoid red-ringed bodies, and choose an upgrade whenever the mass bar fills. Bodies arrive in drifting groups that share a slowly changing heading, while nearby prey flee, amber rivals strafe, and some larger red bodies hunt. Larger bodies also consume smaller bodies they collide with and grow, so the surrounding ecosystem keeps changing without player contact. Smaller bodies that enter the player's short capture range are pulled inward; Growth upgrades extend that range slightly. Hit amber rivals head-on at speed to break them before they can side-swipe you. Shield upgrades add visible orbiting satellites that absorb dangerous contact.

## Lifecycle

Both launchers borrow their Runtime for the complete blocking App call. Escape or window close stops the Desktop App, while Boot maps to Back on AMOLED. The App cancels and releases its Lua job, destroys its Lua Host, and returns without taking ownership of the caller's Runtime. The AMOLED image confirms through H2Loader only after Display and Touch are open and the first frame has been presented.

## Validation

Run the focused host tests and Desktop build from the repository root:

```sh
bazel test //libs/lua:lua_core_test //libs/lua:lua_test
bazel test //projects/example/apps/lua-cosmic-drift/app:lua_cosmic_drift_test
bazel test //projects/example/libs/desktop/cosmic-drift:pointer_touch_adapter_test
bazel build --config=macos_arm64 //projects/example/targets/cc_binary/lua-cosmic-drift:example-lua-cosmic-drift
```

With the repository-supported ESP-IDF environment exported, validate the AMOLED policy and build its independent package:

```sh
bazel test //projects/example/targets/h2loader_tar_zlib/lua-cosmic-drift/amoled:task_policy_test
bazel build --config=esp32s3 //projects/example/targets/h2loader_tar_zlib/lua-cosmic-drift/amoled:package
```

Finish with the ownership graph and repository checks:

```sh
bazel query //projects/example/apps/lua-cosmic-drift/...
bazel query //projects/example/libs/desktop/cosmic-drift/...
bazel query //projects/example/targets/cc_binary/lua-cosmic-drift/...
bazel query //projects/example/targets/h2loader_tar_zlib/lua-cosmic-drift/...
make guides-build
git diff --check
```
