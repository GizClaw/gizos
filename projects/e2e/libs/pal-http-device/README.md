# PAL HTTP device runner

The runner connects using the Board Runtime's existing saved STA configuration without printing or replacing credentials. It sets the real wall clock to the explicitly supplied fixture epoch so certificate validity is checked, creates CoreHTTP over the board's real Net provider with the isolated test CA, and runs the same portable registry. The regular Runtime HTTP provider is left owned by the board. Serial command service remains available; launchers only confirm an App after all cases pass and replay an immutable result without rerunning requests.

Start the fixture explicitly on an operator-selected LAN address, keep it running through image build/install/qualification, and inject its generated public settings into the build:

```sh
bazel run //projects/e2e/libs/pal-http-fixture:serve -- --bind 0.0.0.0 --advertised <host-lan-ip> --bazelrc /tmp/pal-http-fixture.bazelrc
bazel --bazelrc=/tmp/pal-http-fixture.bazelrc build --config=esp32s3 //projects/e2e/targets/h2loader_tar_zlib/pal-http/devkit:package
bazel --bazelrc=/tmp/pal-http-fixture.bazelrc build --config=bk7258 //projects/e2e/targets/h2loader_tar_zlib/pal-http/bk7258_v3_202405:package
```

The generated Bazel settings contain only fixture URLs, public CA PEM encoded as hex, and fixture time. Private keys remain in the fixture's temporary directory. Never commit generated settings or keys. Empty default settings permit CI compilation but fail setup at runtime. Build and install remain separate operations; this runner does not open serial ports, reset a board, flash a Loader, format storage or publish artifacts. Follow the board's H2Loader-first installation workflow and record actual UID/version/image hash, exact case registry, final confirmation and empty Stage before marking that platform passed.
