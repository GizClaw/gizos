# PAL HTTP device runner

The runner first reuses an existing GOT_IP connection, even when it has no saved profile; otherwise it connects using the Board Runtime's existing saved STA configuration without printing or replacing credentials. Before any case starts, missing/unavailable networking keeps the H2Loader command channel alive and emits a SETUP_WAIT marker. Saving a profile through the normal Wi-Fi command allows that same App to continue automatically; once cases start, failures are retained and never rerun to obtain a pass. It sets the real wall clock to the explicitly supplied fixture epoch so certificate validity is checked, creates CoreHTTP over the board's real Net provider with the isolated test CA, and runs the same portable registry. The regular Runtime HTTP provider is left owned by the board. Serial command service remains available; launchers only confirm an App after all cases pass and replay an immutable result without rerunning requests. Each boot uses a fresh public run nonce to isolate fixture retry counters. The fixture can write per-run server arrival evidence with `--receipt <path>`; timeout/cancel cases must reach the intended server handler, and the total retry deadline must involve multiple real attempts.

Start the fixture explicitly on an operator-selected LAN address, keep it running through image build/install/qualification, and inject its generated public settings into the build:

```sh
bazel run //projects/e2e/libs/pal-http-fixture:serve -- --bind 0.0.0.0 --advertised <host-lan-ip> --bazelrc /tmp/pal-http-fixture.bazelrc
bazel --bazelrc=/tmp/pal-http-fixture.bazelrc build --config=esp32s3 //projects/e2e/targets/h2loader_tar_zlib/pal-http/devkit:package
bazel --bazelrc=/tmp/pal-http-fixture.bazelrc build --config=bk7258 //projects/e2e/targets/h2loader_tar_zlib/pal-http/bk7258_v3_202405:package
```

The generated Bazel settings contain only fixture URLs, public CA PEM encoded as hex, and fixture time. Private keys remain in the fixture's temporary directory. Never commit generated settings or keys. Empty default settings permit CI compilation but fail setup at runtime. Build and install remain separate operations; this runner does not open serial ports, reset a board, flash a Loader, format storage or publish artifacts. Follow the board's H2Loader-first installation workflow and record actual UID/version/image hash, exact case registry, final confirmation and empty Stage before marking that platform passed.

The repository has no default SSID/password. Its documented operation environment checks `.env/devenv`, then `~/.config/h2loader/env`; direct CLI invocation only uses explicit arguments. Configure the device through `h2loader --no-ble --port <port> wifi connect <ssid> <password>`, verify `wifi status` reports a usable IP, and keep credential values out of logs and committed artifacts. A missing saved profile with `ip_valid=0` is an unmet network prerequisite, not an HTTP PASS or a missing Wi-Fi capability.
