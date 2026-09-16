# Touch and Button target ownership

Touch and Button now own their JieLi wrappers, task declarations/policies,
firmwares and packages under their respective example target directories.
The old Display labels are compatibility aliases. Their shared LVGL/Runtime
library component is owned by `projects/example/native_component_src/jieli/wl82/devkit_app:ui_libs`.
Neither new firmware graph depends on the Display target, as verified by a Bazel
dependency query filtered to example target labels.

Both wrappers are unchanged byte-for-byte. SHA-256:

- Touch: `1a99e737a61c991ca4b2b316ba3fb3070e28adc55f57508e05db4185d1539de5`
- Button: `82e756822aec1af658ad62da9c9486ee5aaee89082639f62e71d491501d32e6e`

Task policies, image identities, App roles, and the shared board layout remain
unchanged. This is ownership/build validation, not new touch/button hardware
acceptance or completion of the PAL resource-lifetime audit.

Both packages built successfully through their old Display aliases using the
native AC791N Linux toolchain (63.166 seconds), including both firmware builds
and tar.zlib packaging. Local log: `tmp/jieli/input-owner-build.log`.
