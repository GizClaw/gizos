# Lua Link E2E on AMOLED

AMOLED runs the `join` side of [Lua Link E2E](/apps/e2e#lua-link) against a
DevKit running the `host` side, both over NimBLE Extended Advertising and
Extended Scanning. The H2Loader App command service starts the BLE Host; Wi-Fi
stays off. Build:

```sh
bazel build --config=esp32s3 \
  //projects/e2e/targets/h2loader_tar_zlib/lua-link/amoled:package
```

Install as described in [AMOLED H2Loader](./h2loader) and boot the AMOLED before
the DevKit host; the joiner scans for up to 60 s per session. After five
sessions the console shows:

```text
H2_LUA_LINK_E2E stage=summary board=amoled role=join passed=5 rounds=5
```

The final `hold` session reports how fast a DevKit reset surfaces as `lost`.
