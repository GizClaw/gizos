# Shared example launcher ownership

The ColorBar, Audio System, Touch and Button targets share a startup component through `h2_jieli_target_application_run()`. That component now lives under `projects/example/native_component_src/jieli/wl82/devkit_app`, rather than being owned by the Display target. Existing Display launcher/task labels remain aliases. Application task policies and firmware/layout selection are unchanged.

The moved C source has identical SHA-256 before and after the move: `3b5656a410bbe985357d4a1768bdf17619f83bd9ec17213f2430ced0622f571c`. The logical task remains `h2color/runtime`; this change does not rename tasks or change stack sizes. The two compiled-fixture test modules for App power and UART console behavior execute seven passing cases after updating their source paths.

All four firmware targets (`firmware`, `audio_system_firmware`, `touch_firmware`, `button_firmware`) built successfully using the AC791N Linux toolchain in OrbStack. The local build log is `tmp/jieli/shared-launcher-owner-build.log`.

This extraction does not complete the audit of SDK-private diagnostics in the launcher, nor the migration of Audio/Touch/Button firmware graphs to their own application targets. It is not hardware acceptance. The board still runs the older P2 candidate Loader, with P1 marked invalid, awaiting USB DL recovery.
