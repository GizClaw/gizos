# H2Loader Batch Loader Web target

Build and verify the serve-ready archive from the repository root:

```sh
make test-web
bazel build //projects/h2loader/targets/pkg_tar/batch-loader:batch-loader
```

The artifact is
`bazel-bin/projects/h2loader/targets/pkg_tar/batch-loader/batch-loader.web.tar`.
Extract the archive at the HTTPS origin root. Keep `_headers` when the hosting
provider supports that convention, or configure the same headers explicitly.
Web Serial requires a secure context; `http://localhost` is suitable only for
local development.

Build, safely extract, and serve the archive for local inspection:

```sh
bazel run //projects/h2loader/targets/pkg_tar/batch-loader:serve -- \
  --host 127.0.0.1 --port 8000
```

The server applies the archive's `_headers` policy and serves `.wasm` as
`application/wasm`. Pass `--port 0` to choose an ephemeral port. Stop it with
Ctrl-C; the extracted temporary directory is removed automatically.

The archive contains the React/shadcn DOM App plus `sdk/h2loader.js`, its
matching generated Emscripten runtime, and `h2loader.wasm`. The SDK is an
internal Bazel target rather than a separately published package. Protocol,
package inspection, managed install, reconnect, and final verification remain
owned by `libs/h2loader_host`; the frontend owns presentation and batch state.
