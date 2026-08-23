# Lua Runtime E2E on AMOLED

该 H2Loader App 使用 PSRAM Memory 与现有 Runtime Task/Queue/Sync/Timer provider，
运行 [E2E 测试 App](/apps/e2e) 的九个固定 case。构建：

```sh
bazel build --config=esp32s3 \
  //projects/e2e/targets/h2loader_tar_zlib/lua-runtime/amoled:package
```

安装和恢复必须遵守 [AMOLED H2Loader](./h2loader)。串口日志必须保留九条
`H2_LUA_E2E_CASE ... result=PASS`，以及：

```text
H2_LUA_E2E result=PASS scheduler=multi-worker passed=9 total=9
```

只有完整 marker 后才允许确认 image。构建成功不能替代真实设备上的 marker、
H2Loader command responsiveness 与返回原 App 的恢复验证。
