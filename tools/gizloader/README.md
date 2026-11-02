# Gizloader

Gizloader combines the H6 desktop workbench and GizOS native loader commands in one installed application. It supports persistent serial connections, batch firmware operations, shared device logs, local/HTTPS/Forge firmware selection, and the existing native serial/Bluetooth CLI.

## Install and use

```sh
bunx gizos
bunx gizos install gizloader
bunx gizos loader --snapshot
```

The bootstrap installs the GUI application and a `gizloader` command. Running `gizloader` opens the workbench; arguments run the native CLI with inherited standard I/O and its original exit code:

```sh
gizloader --help
gizloader scan
gizloader --no-ble scan
```

macOS supports Apple Silicon (13.0 or newer); Windows supports x64 serial devices. Windows BLE is not implemented by the native provider. The desktop workbench manages serial connections; Bluetooth commands use the CLI. Scanning the GUI port list does not connect or write firmware.

## Develop

Requires Bun, Rust stable, Bazel 9.2.0, and the platform's Tauri 2 prerequisites (Xcode on macOS; MSVC and WebView2 on Windows).

```sh
bun install --frozen-lockfile
bun run prepare:native
bun run desktop:dev
bun run desktop:build
```

Run these commands from `tools/gizloader`. `prepare:native` builds `//tools/gizloader/native:gizloader-cli` and `:gizloader-session` against this checkout, stages target-suffixed sidecars and records their hashes. It does not download or clone a second SDK. Cargo and Bazel use normal per-user caches.

The application is a Tauri/Rust supervisor with a React workbench. `native/` contains the native CLI composition and persistent Host Core session adapter. `src-tauri/src/` owns port preferences, immutable package snapshots, sessions, cancellation, downloads and logs. `src/desktop/loader/` contains the workbench and independent log window. The firmware protocol definitions are imported directly from GizOS.

## macOS permissions and signing

The installed macOS command starts an App-owned worker through LaunchServices. This gives Bluetooth requests the App's privacy identity when invoked from a terminal or agent host. A private request directory carries arguments, SDK environment, working directory, streamed output, cancellation and the exact native exit code; no application window is created for CLI work. Use the packaged `gizloader` command for Bluetooth operations, because a bare development helper can inherit its host application's privacy identity.

`GIZLOADER_BLUETOOTH_DESCRIPTION` configures the Bluetooth usage description at build time. Preparation writes the App's generated plist/config and embeds the same description in the native CLI Mach-O. A signed application's permission text cannot be changed at runtime; rebuild and sign after changing it.

Set `APPLE_SIGNING_IDENTITY` to a Developer ID Application identity for local signed builds. CI imports `APPLE_CERTIFICATE` (base64 PKCS#12) with `APPLE_CERTIFICATE_PASSWORD`, then uses `APPLE_SIGNING_IDENTITY`. CI stores the plaintext notarization key in `APPLE_API_KEY_CONTENT` and writes a private temporary file; the packaging script receives its path as `APPLE_API_KEY`, with `APPLE_API_KEY_ID` and `APPLE_API_ISSUER`, and maps these to Tauri's `APPLE_API_KEY_PATH`/`APPLE_API_KEY` variables. Production publication requires signing, notarization and stapling to pass. Pull request builds use ad-hoc macOS signing and are retained only as build artifacts.

## Releases

The dedicated Gizloader workflow builds macOS `.dmg` and Windows NSIS `setup.exe` on native runners. Stable tags are `gizloader-v<version>`; the rolling prerelease is `gizloader-snapshot`. `gizloader-index.json` records the exact source commit, artifact sizes, hashes, platform, format and signing status; `SHA256SUMS` covers the index and installers. The bootstrap selects Gizloader releases independently of the firmware release's latest tag.

Windows installs per user under `%LOCALAPPDATA%/Programs/Gizloader`. The installer adds a `gizloader.cmd` launcher alongside `gizloader.exe` and registers its directory in the user PATH. The command invokes the bundled console binary to preserve standard I/O. macOS includes its CLI in `Gizloader.app/Contents/MacOS` and installs a user launcher pointing into the App.

## Verify

```sh
bun run build
bun test scripts/release-index.test.mjs
cargo test --manifest-path src-tauri/Cargo.toml
bun scripts/verify-macos-install.ts
bazel test --config=macos_arm64 //tools/gizloader/native:session_test
```

Use the matching native Bazel configuration on other platforms. Hardware transfer/reboot tests are separate from package, process, GUI fixture and installer tests.

## Source attribution

The desktop workbench, Rust supervisor and persistent session adapter were migrated from `GizClaw/h6infra` (`desktop/loader` and the frontend batch loader) at the source revision recorded in `UPSTREAM.json`. The controller retains its original Gizway provenance. GizOS Host Core, firmware protocols, native platform adapters and browser SDK remain shared repository components. Dependency licenses are retained with their distributions; see `NOTICE`.
