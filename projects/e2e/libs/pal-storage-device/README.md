# Storage device ledger verification

宿主持有设备的显式 port/UID 分配，并保存安装前 status/coredump、实际 package/image checksum、五次独立正常 boot 的命令 receipt，以及安装后的 status/coredump。验证器不扫描、不重启也不写设备；单独的 case ledger 不能证明 UID、镜像、Loader 或 coredump 身份。

当前契约 2 的采集顺序为 phase 1（31 个 seed case）、phase 2（3 个持久化读取及 cleanup case）、phase 3（2 个 cleanup persistence case）、phase 4、phase 4。最后两次只重新确认 empty=1/rc=0，不能产生新的 case 或重复增加通过数。每次 boot 需要实际的 `H2_STORAGE_BOOT contract=2 version=... phase=... nonce=...`，不能用 `replay=1` 代替；若宿主 monitor 到期或初始输出缺行，保留初始 BOOT 日志并拼接同一次 boot 的后续 replay。phase 1/2/3 必须至少包含一个以 replay BOOT 独立分隔的完整块：本块内按 registry 顺序出现全部 case 和 terminal phase，不能把多个残缺块拼成完整 ledger。所有有效观察行，包括不完整初始输出里的 FAIL，都仍参与身份、结果及不可变性检查。各 boot 使用同一版本及由版本生成的 nonce；UART 的不可变重放可以重复，但内容不能变化。

在仓库根目录运行：

```sh
python3 projects/e2e/libs/pal-storage-device/verify_device.py \
  --version pref-strength-20261004-bk --output /tmp/pref-bk-ledger.json \
  /path/phase1.log /path/phase2.log /path/phase3.log \
  /path/complete-boot1.log /path/complete-boot2.log

bazel test //projects/e2e/libs/pal-storage-device:ledger_verifier_test
```

输出 `ledger_complete=true`、36 个唯一 PASS case 及五个 phase。重复 case 只接受同 boot/nonce 下的完全相同数据；选定新 boot 后所有 BOOT 必须是同 contract/version/phase/nonce 的 replay，后续出现其它 boot 或变更身份时，即使还没有新 case 也拒绝。Watchdog、panic、控制阶段提交失败、未确认的 App、错序/不完整的 ledger 或完成状态后的残留数据都会拒绝。独立 boot 仍由宿主的真实 reboot receipts 建立，不能把同一个 phase 4 日志复制两次当成两次执行。

完整资格还必须用同模块的 `final_status(before, after, manifest, package_sha256, uid)` 校验实际 UID 不变、初始 P1 有效且 role=loader、P1 全部 identity 字段不变、最终 P2 有效且 current image 与 package/image checksum 一致、Stage 为空及 `last_result=0`。`coredump_status(before, after, before_sha256, after_sha256)` 校验 dump 参数不变；原本非空时需要实际 dump 文件的前后字节 SHA 相等。`manifest` 使用实际 firmware JSON 的 `package_manifest`，不能只传版本字符串或把新构建 artifact 重绑到旧 boot。

## BK native console startup

The shared fixture emits one non-replay `H2_STORAGE_BOOT` during the actual
`h2_storage_device_run`, after loading its persistent phase/version/nonce.
`h2_storage_device_replay` remains explicitly `replay=1`; it never substitutes
for the fresh marker. Console flushing stays in the native BK launcher; the
portable archive does not access the SDK's standard-stream state.
The 36-case contract, `1/2/3/4/4` lifecycle and strict c93 oracle are unchanged.

BK alone gives the already-started rollback/control service a bounded 60-second
startup settle (host uses a 20-second handshake and 15-second command window),
then stops/joins its UART control owner. The real suite and its first complete
immutable replay use the native console during that quiet interval. After a
bounded 200 ms queue settling interval it restores management on both suite
success and failure, confirms only a successful suite, and emits READY. It
repeats READY with each later replay, including both phase-4 completion checks.
A failed stop/restart or confirm cannot be admitted as a successful capture.
The 3600-second stress watchdog remains specific to the BK test launcher.

The CLI installs `on_log` before initial serial connect; the real
`serial_session_control` demux forwards non-frame bytes to that sink before a
SESSION_OPEN ACK. macOS serial configuration uses TCSANOW without input flush.
Thus a native fresh line can be captured during handshake rather than waiting
for management restoration after the long Pref run. Repeated failed connects
still leave close/reopen gaps; the bounded settle improves the normal startup
window and is not a promise to recover arbitrary UART loss. Hardware qualification
continues to require an actually captured fresh marker and a full independent
ordered immutable replay. R3's 259 replay markers, 31 PASS phase result and
confirmed App demonstrate that phase's business result, not a five-boot receipt.

`bk_console_lifecycle_test` compiles the actual launcher run function against
scripted SDK/time boundaries to check ownership order and stop/suite/restart/
confirm outcomes. It also compiles the real host session control and frame
filter to check fragmented/delayed/missing ACK capture. These host regressions
are not board receipts. Run it alongside `ledger_verifier_test`.
