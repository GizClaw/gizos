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
  stage, activate, rediscover, reconnect, and final verification flow.

An `H2_PAL_OK` install result always includes final authoritative verification.
The bounded result ledger retains typed-command transport result, parsed
terminal, output byte count, truncation, and lifecycle-transition metadata; a
launcher never infers command success from console text.
