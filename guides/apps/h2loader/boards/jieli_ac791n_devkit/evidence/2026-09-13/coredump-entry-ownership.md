# Coredump diagnostic entry ownership

The two diagnostic images now belong to `projects/e2e/targets/h2loader_tar_zlib/coredump/jieli_ac791n_devkit`. They share one diagnostic launcher and a target-owned Bazel task policy, and still use the board's shared H2Loader layout. Both packages remain `no-release`, retain their image names and App role, and the old Display labels remain aliases for existing scripts.

The injected fault remains at SDK boot stage 105, with caller marker `0x48324352`. The shared boot patch invokes this hook after initcall and before module_initcall and app_main. The diagnostic now owns app_main too: if reached unexpectedly, it prints a failed-injection message and returns. It no longer pulls in Display's direct LCD/Touch/ADC loop or Loader's USB log smoke launcher merely to satisfy that symbol.

Validation:

- Bazel dependency query for the new smoke package found no dependencies under `projects/example/targets` or `projects/h2loader/targets`.
- Native AC791N builds through both old package aliases succeeded.
- No diagnostic package was installed in this verification pass.

After separating the diagnostics, the obsolete `direct_firmware`, its task policy, and its 591-line direct peripheral implementation were removed. Repository text search found no external consumer of these labels; the package-scoped reverse-dependency query returned only `direct_firmware` itself. A repository-wide query was unavailable because the local Android SDK repository lacked `core-for-system-modules-jar`, so it is not claimed as successful evidence. The maintained PAL Color Bar package remains.

This completes coredump entry separation and retirement of the redundant direct Color Bar path, not the broader example/diagnostic ownership audit.

## Speaker diagnostic

Speaker smoke now lives under the E2E `speaker-smoke` target with its own launcher and task policy. Its previous Display labels remain aliases. Playback and heartbeat run in the explicitly budgeted `speaker_smoke` task; app_main returns after task creation so SDK app_core dispatch is not blocked by the diagnostic's permanent loop. This remains a manual diagnostic, not an automatically asserted audio or AEC acceptance test.

The new firmware dependency graph has no Display/Loader target dependency. Native compilation validates the new entry, but no speaker firmware was installed during this ownership pass, so no fresh audio result is claimed.
