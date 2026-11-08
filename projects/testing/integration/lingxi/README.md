# Lingxi device E2E App

The portable App runs the public Lingxi library through injected HTTP, JSON, Memory and Time PAL. It owns ordered cases, individual results, finite per-case budgets, dependency-failure handling, progress callbacks and temporary device-credential cleanup. It does not read environment variables, files, concrete operating systems or boards. Host and device launchers share the [App contract](app/include/h2_lingxi_e2e.h) and [case registry](app/include/h2_lingxi_cases.inc); launchers own concrete providers and network lifetime.

The App obtains credentials for a registered device, then sends one caller-supplied heartbeat. A failed query marks the heartbeat BLOCKED and proceeds to cleanup. Any FAIL, BLOCKED, timeout or launcher-teardown failure prevents success. Results contain only cases, PAL return values, HTTP/business status and duration, excluding product Keys, device Secrets and raw responses. The fixed ledger validates one device HTTP integration; it does not establish model integration, OneNET validation or physical-device enrollment.

## Host entry

`projects/testing/targets/cc_binary/lingxi` composes real CoreHTTP, yyjson and Darwin/Linux Net PAL. The entry parses process-environment configuration and passes it to the App; no positional arguments or mandatory parameter file are needed. Read required and optional variables from usage:

```sh
bazel run --config=macos_arm64 //projects/testing/targets/cc_binary/lingxi:lingxi -- --help
```

Product, device number and encoding use platform-registered test identities. Heartbeat JSON is the caller's protocol-compliant params object. Host simulation uses an explicit test-version marker, host observations and permitted defaults; production acceptance uses actual device data. Credentials remain in controlled local configuration or the process environment, outside Git, BUILD, CMake and compiler arguments.

After setting the environment required by usage, invoke the live test manually:

```sh
bazel test --config=macos_arm64 \
  --test_env=LINGXI_BASE_URL \
  --test_env=LINGXI_PRODUCT_ID \
  --test_env=LINGXI_PRODUCT_KEY \
  --test_env=LINGXI_DEVICE_NO_TYPE \
  --test_env=LINGXI_DEVICE_NO \
  --test_env=LINGXI_SOURCE_IP \
  --test_env=LINGXI_HEARTBEAT_JSON \
  //projects/testing/targets/cc_binary/lingxi:live_test
```

Optional configuration is inherited through matching `--test_env` arguments. `live_test` is `manual`/`external`, excluded from default automatic tests and prior live-result reuse, while build caching remains enabled. The live entry always verifies TLS and rejects plaintext and redirects. The ordinary host executable offers an explicit plaintext switch only for isolated loopback fixtures.

## Automatic validation and boundaries

```sh
bazel test --config=macos_arm64 //projects/testing/integration/lingxi/app:all
bazel test --config=macos_arm64 //projects/testing/targets/cc_binary/lingxi:wire_test
```

App-local tests check dependency failure, clock errors, deadlines, ledger and resource cleanup. The wire test uses real CoreHTTP and host Net to check environment injection, query-before-heartbeat ordering, business rejection, identity mismatch, input/response budgets, redirect rejection and the live entry's TLS gate. It connects only to an isolated local fixture and reads no external credentials. The macOS/Linux host and [DevKit carrier entry](../../targets/h2_loader_tar_zlib/integration/carrier/espressif/esp32s3_devkitc_1/README.md) share the App. The device entry builds its heartbeat from Runtime and consumes one-use runtime configuration. Results bind each platform's actual image and run; they cannot substitute for one another.
