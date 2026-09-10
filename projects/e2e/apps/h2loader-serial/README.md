# H2Loader Serial E2E

This portable headless App verifies the Host Serial PAL through H2Loader Host
Core. Target launchers own serial authorization, file or browser resources,
argument parsing, Runtime assembly, and presentation.

The suites are cumulative only when their bits are selected:

- `preflight`: enumerate the provider and optionally require one opaque port ID;
- `status`: open reliable serial, complete the handshake, and read authoritative
  identity;
- `command`: execute one closed, read-only Host Core command;
- `install`: validate a packaged catalog by exact SHA-256 and run the managed
  stage, activate, rediscover, reconnect, and final verification flow;
- `loader-update`: install a Loader package whose image differs from the
  running Loader in Partition 1 and require that the device then boots the new
  Loader from Partition 1: the package version and image are active, Partition
  1 metadata records the package, and no Stage is left. Partition 2 only
  carries the relay and is not checked. The same image is rejected
  before any device mutation because it would finish without a relay.
  `loader_trial_observed` reports whether a reconnect happened to see the
  candidate Loader on Partition 2; it is timing-dependent evidence, not a pass
  condition. The run overwrites the App in Partition 2, so reinstall the App
  afterwards.

An `H2_PAL_OK` install result always includes final authoritative verification.
The bounded result ledger retains typed-command transport result, parsed
terminal, output byte count, truncation, and lifecycle-transition metadata; a
launcher never infers command success from console text.

The desktop launcher runs the same suites against a local catalog:

```sh
bazel run --config=<host> //projects/e2e/targets/cc_binary/h2loader-serial:e2e-h2loader-serial -- \
  --suite loader-update --port-id <port> --expected-board <board> \
  --expected-target <target> --asset-sha256 <update.tar.zlib sha256> \
  --firmware-index <abs firmware-index.json> --resource-root <abs dir>
```

Run it twice with two Loader builds of different versions to cover an update
that starts from App and one that starts from Loader.
