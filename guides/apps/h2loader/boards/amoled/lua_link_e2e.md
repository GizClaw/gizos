# Lua Link E2E on AMOLED

AMOLED runs the `join` side of [Lua Link E2E](/apps/e2e#lua-link) against a
DevKit running the `host` side, both over NimBLE Extended Advertising and
Extended Scanning. The H2Loader App command service starts the BLE Host; Wi-Fi
stays off. Build:

```sh
bazel build --config=esp32s3 \
  //projects/e2e/targets/h2loader_tar_zlib/lua-link/amoled:package
```

The default role is `join`. To use AMOLED as the fixture for a board running `join`, build the same target with `--define=H2_LUA_LINK_E2E_ROLE=host`; only `host` and `join` are accepted. Both roles keep the original five sessions and final hold session. The entry sends the original test records through checked stdout and leaves a five-second serial reconnection window before the first round, without changing the board log level or session deadlines.

Install as described in [AMOLED H2Loader](./h2loader), then boot the joiner before the host; the joiner scans for up to 60 s per session. After five sessions the console shows the following for AMOLED's default `join` role:

```text
H2_LUA_LINK_E2E stage=summary board=amoled role=join passed=5 rounds=5
```

The final `hold` session reports how fast a peer reboot surfaces as `lost`. Reboot the peer through its normal H2Loader command path.
