# ESP32-S3 DevKit PAL Crypto E2E

使用板级 Runtime 的真实 Crypto PAL 和硬件随机源运行同一份 15 操作 / 22 case portable 契约。只通过 public PAL 调用，不替换 entropy，不打印生成的密钥。Crypto 测试不需要存储 fixture 或格式化 SD 卡。

```sh
bazel build --config=esp32s3 --lockfile_mode=off \
  --//tools/bazel:firmware_version=pal-crypto-<unique-version> \
  //projects/e2e/targets/h2loader_tar_zlib/pal-crypto/devkit:package
```

使用仓库固定 SDK 并保留 Bazel/native ccache。安装前用当前 H2Loader 串口查询设备 UID、Loader/P1、Stage 和 coredump 基线；send `update.tar.zlib` 后核对 Stage 包/镜像 SHA 和版本，再 `reboot upgrade --monitor`。安装写入期间不 reset。DevKit 使用 USB Serial/JTAG；BK 使用 AP UART1，UART0 是标准日志口。

接受完整 `H2_CRYPTO_CASE` ledger 和相同镜像版本的 `H2_CRYPTO_REPORT`，要求 22 PASS、0 FAIL/BLOCKED/NOT_RUN、rc=0、complete=1、qualified=1。App 每次 boot 只执行一次，后续慢速 replay 只重放不可变结果；同 ID 的重复结果必须相同。宿主监视超过期限须视为未验证，可重新连接读取同一次 boot 的结果，不能把 replay 当作新的测试。

最终查询状态确认 App/P2 valid、Stage empty、last_result=0、原 Loader 身份保留、coredump 与基线一致。`evidence/` 保存结构化 build/qualification receipt；原始串口日志和 binary dump 不进入 Git。
