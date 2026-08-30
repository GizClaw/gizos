# APP 更新

APP 和 Loader 使用同一个 Stage 实现。当前运行 APP 时，Host 可以直接执行 `send` 或 `send-url`，无需先切回 Loader。

## 安装流程

1. Host 发布 `role=app` 的 Stage。
2. Host 执行 `h2loader reboot upgrade`。
3. Partition 1 Loader 在 `boot_intent=AUTO` 下重新验证 DL 文件、package SHA-256、manifest 与 Stage metadata。
4. 在写 Partition 2 之前提交 `partition_2.valid=false`。
5. 完整写入并校验 APP image。
6. 将 Stage identity 复制到 Partition 2 metadata，最后提交 `partition_2.valid=true`。
7. 选择 Partition 2 并重启。

写入失败时 Partition 2 保持 invalid，Loader 留在 Partition 1，并记录 `last_result`；下次 AUTO 从头重写，不做字节级续写。

## APP 启动后的 Stage 收尾

APP 启动并完成平台 OTA image 确认后，必须核对：

- 当前运行在 Partition 2。
- 固件内嵌 identity 与 `partition_2` metadata 一致。
- Stage 与 Partition 2 的 image checksum 一致。

全部一致后，APP 删除 `/dl/update.tar.zlib` 并清空 Stage metadata。流程不保存 `app_confirmed` 或 install state。

## 重启语义

`reboot app` 只选择 Partition 2 并重启，不安装或清理 Stage。`reboot loader` 只返回 Partition 1 且设置 `boot_intent=LOADER`，同样不消费 Stage。只有 `reboot upgrade` 进入 AUTO 安装检查。

命令返回 accepted 只证明请求已提交；Host 必须在重连后验证 active identity、运行分区、Partition metadata 和 Stage 终态，才能报告安装成功。
