# H2Loader Desktop

H2Loader Desktop is the generic Tauri client for the GizOS Host Core. Its React interface, Rust process/file ownership and native serial adapter live in this project. The CLI and desktop adapter are built from the same checkout; there is no separately pinned SDK checkout or copied platform implementation.

The implementation was migrated from `GizClaw/h6infra`. H6Infra remains a website and browser-SDK consumer and links to published GizOS installers. The application name and identifier are declared in `src-tauri/tauri.conf.json`; the migrated application has independent settings and can coexist with the former H6 application.

## Build and test

Install Node.js, Rust, Bazel and the platform's Tauri prerequisites, then run from the GizOS repository root:

```sh
npm ci --prefix projects/h2loader/apps/desktop/frontend
node projects/h2loader/apps/desktop/scripts/prepare-native.mjs
npm test --prefix projects/h2loader/apps/desktop/frontend
npm run test:e2e --prefix projects/h2loader/apps/desktop/frontend
cargo test --locked --manifest-path projects/h2loader/apps/desktop/src-tauri/Cargo.toml
npm run desktop:build --prefix projects/h2loader/apps/desktop/frontend
```

The frontend imports the repository's command definitions. Native preparation builds the public CLI and the desktop session target through Bazel, records their SHA-256 and source revision, and copies only those verified executables and the repository license into ignored bundle inputs.

## Operations

Scanning enumerates serial endpoints without opening them. Explicit connections remain open for status, firmware operations and logs. Device identity comes from connected H2Loader status; reconnect requires the same device UID. Cancellation and shutdown release the owned processes and sessions. Serial connections preserve DTR/RTS; this tool does not perform raw flash recovery.

Local files, HTTPS downloads and Forge selections create private immutable snapshots. Rust enforces the compressed input limit and a bounded native inspection deadline. The native adapter delegates both update formats, version syntax and payload checksums to the Host Core package inspector. Format-2 raw data validation remains the device installer's responsibility, matching the CLI contract. Selecting or inspecting a file performs no device write.

The Forge picker starts at the generic releases directory. Its existing registered H6Infra callback URI is retained as an intercepted redirect identifier; the desktop intercepts it before navigation and does not load the website callback page. Only bounded anonymous HTTPS downloads from the release distribution origin are accepted for picker results. The browser picker is a separate consumer.

## Publication

`.github/workflows/h2loader-desktop.yml` builds and tests the supported installers as a required CI execution class. `.github/workflows/h2loader-desktop-release.yml` reuses that build from the same source revision. Manual publication from `main` creates a versioned desktop Release only after every installer producer passes. `release-contract.json` declares the supported output inventory; the release assembler verifies every installer against its producer digest, native source revision and adapter source digest. Published files are downloaded and compared before making the draft Release public.

Desktop releases and npm SDK releases have independent version declarations. They do not modify device Loader firmware or activate a consumer website. Installer signatures and supported platforms are recorded in the release index.
