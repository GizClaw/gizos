# Lua Link E2E on DevKit

DevKit runs the `host` side of [Lua Link E2E](/apps/e2e#lua-link) against an
AMOLED running the `join` side, both over NimBLE Extended Advertising and
Extended Scanning. The H2Loader App command service starts the BLE Host; Wi-Fi
stays off. Build:

```sh
bazel build --config=esp32s3 \
  //projects/e2e/targets/h2loader_tar_zlib/lua-link/devkit:package
```

Install both boards as described in [DevKit H2Loader](./h2loader) and
[AMOLED H2Loader](../amoled/h2loader), boot the AMOLED joiner first, then the
DevKit. The image confirms itself once the command service runs, then runs five
sessions and logs:

```text
H2_LUA_LINK_E2E stage=summary board=devkit role=host passed=5 rounds=5
```

A final `hold` session stays connected until the link drops; resetting either
board makes the other log `LINK stage=hold_end reason=lost:-10 ...`.
