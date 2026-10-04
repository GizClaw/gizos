# Storage device ledger verification

宿主持有设备的显式 port/UID 分配，并保存安装前 status/coredump、实际 package/image checksum、五次独立正常 boot 的命令 receipt，以及安装后的 status/coredump。验证器不扫描、不重启也不写设备；单独的 case ledger 不能证明 UID、镜像、Loader 或 coredump 身份。

当前契约 2 的采集顺序为 phase 1（31 个 seed case）、phase 2（3 个持久化读取及 cleanup case）、phase 3（2 个 cleanup persistence case）、phase 4、phase 4。最后两次只重新确认 empty=1/rc=0，不能产生新的 case 或重复增加通过数。每次 boot 需要实际的 `H2_STORAGE_BOOT contract=2 version=... phase=... nonce=...`，不能用 `replay=1` 代替；若宿主 monitor 到期，保留初始 BOOT 日志并拼接同一次 boot 的后续 replay，直到拿到完整 terminal phase。各 boot 使用同一版本及由版本生成的 nonce；UART 的不可变重放可以重复，但内容不能变化。

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
