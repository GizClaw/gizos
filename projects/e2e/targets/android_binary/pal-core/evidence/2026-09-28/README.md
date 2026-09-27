# PAL Core android — 2026-09-28

`android_pal_core_simulator_test` 在模拟器实际执行，41/41 PASS，FAIL/BLOCKED/NOT_RUN 均为 0。 `cleanup=0`、`teardown=0`，测试前后 PAL 资源计数一致。

- `qualified.json`：完整 case 清单和资源基线。
- `environment.json`：运行环境以及 App/实际使用的 SDK 包 SHA-256。
- `sdk-metadata.json`：对应包的架构、版本和校验值。
- `pal-core.stdout`：实际 case 执行日志和 pthread 栈大小观察。
- `bazel-test.txt`：Bazel 测试完成记录。

复跑方式见 `projects/e2e/libs/pal-core-mobile/README.md`。未宣称物理手机已通过。
