# OpenAPI C code generator

`openapi_codegen` is a host-side Go tool that converts an OpenAPI 3.0 JSON
contract into C11 request/response types and synchronous client functions. The
generated client calls HTTP, JSON, and Memory PAL directly; there is no
`libs/openapi` runtime.

## Bazel generation

Stable consumers should pin the source document with `http_file` (URL plus
SHA-256) and use `openapi_c_sdk`:

```starlark
load("//tools/openapi_codegen:openapi_c_sdk.bzl", "openapi_c_sdk")

openapi_c_sdk(
    name = "service_api",
    package_name = "service",
    schema = "@service_openapi//file",
    deps = ["//libs/pal"],
)
```

The Bazel action fetches only the digest-pinned JSON input, generates
`h2_<package>_api.h` and `h2_<package>_api.c`, and compiles those outputs into
the consumer library. Firmware links the generated C and PAL only; it never
links the Go tool.

`//libs/haivivi_next_api:haivivi_next_api` is the production instance. Its
source is `https://api.haivivi.cn/openapi-json`, pinned in `MODULE.bazel`.

## Direct usage

```sh
bazel run //tools/openapi_codegen:openapi_codegen -- \
  --schema path/to/openapi.json \
  --output path/to/generated \
  --package pets
```

The dedicated output directory contains the generated header, source, and
`.h2_openapi_codegen.json` ownership manifest. `--check` verifies committed
output without writing and rejects missing, stale, edited, or unexpected files.

## Supported production profile

- OpenAPI 3.0.x JSON and confined local relative JSON references.
- `GET`, `POST`, `PUT`, `PATCH`, and `DELETE`; duplicate `operationId` values
  are deterministically disambiguated with method and path.
- Path, query, and header parameters, including `form`/`explode=true` query
  arrays.
- Per-operation HTTP bearer security.
- JSON request bodies and status-specific JSON or empty responses.
- `multipart/form-data` request objects with binary fields.
- Response header capture through HTTP PAL.
- Objects, metadata-only `allOf` fragments, tagged object `oneOf`, strings,
  booleans, safe integers, float/double, string enums, arrays, nullable values,
  and `readOnly`/`writeOnly` context.
- Open objects with `additionalProperties: true`, preserved as owned or borrowed
  opaque JSON through JSON PAL object iteration.
- `uuid`, `date-time`, and `uri` represented as UTF-8 strings.
- Schema examples are metadata and are not used for wire-shape validation.

Unbounded schema strings are limited by `max_string_bytes`; arrays and dynamic
object member counts are limited by `max_array_items` in the generated client
config. Request, response, and URL storage are independently bounded. Generated
request values are borrowed for a
synchronous call; response strings, arrays, models, dynamic JSON, and captured
headers are uniquely owned and released by the operation-specific
`*_response_deinit` function.

The generator rejects OpenAPI 3.1, remote or escaping references, cyclic model
ownership, callbacks, links, cookies, `anyOf`, `not`, schema-valued or false
`additionalProperties`, unsafe integer ranges, unsupported serialization
styles, and constraints it cannot enforce.
