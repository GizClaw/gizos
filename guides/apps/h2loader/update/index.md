# 更新、启动与恢复

H2Loader 使用同一套 Stage 和双分区流程管理 APP 与 Loader 更新。流程不保存“安装到了哪一步”的状态机；每次启动都依据实际 package、镜像 metadata、checksum、当前运行分区和 `boot_intent` 作出决定。

## 更新入口

APP 和 Loader 都通过同一设备命令接收 package：

- `h2loader stage <bytes> <sha256>`：从当前 UART、BLE 或 Web Serial transport 接收 package。
- `h2loader stage url <url> <bytes> <sha256>`：通过设备 Wi-Fi 下载 package。
- `h2loader stage abort`：删除 Stage 文件并清空 Stage metadata。

Host CLI 对应 `send`、`send-url` 和 `stage abort`。传输成功只表示 Stage 已完整发布，不会自动安装。

## Package 与 identity

`update.tar.zlib` 的 manifest 包含 role、board、target、version、raw image size 和 raw image SHA-256。完整压缩 package SHA-256 与 raw image SHA-256 是两种不同 identity，不能互换。

运行时 identity 不读取 Stage 或 Preference：role、version、board 和 target 由构建事实随固件链接；image size 和 SHA-256 由平台直接读取当前运行分区并计算。把 whole-image SHA-256 原样写回同一镜像会形成自引用，因此 checksum/size 采用运行分区的权威计算值，并必须与 package manifest 精确一致；无法读取或计算时启动失败关闭。

持久化状态只描述三个槽位：

- `stage`：已发布 package 及其 image identity。
- `partition_1`：正式 Loader 分区中的 image identity。
- `partition_2`：APP 或临时候选 Loader 的 image identity。

三份 metadata 都包含 `valid`、image checksum/size、role、version、board 和 target；Stage 以及有来源 package 的 Partition metadata 还包含 package checksum/size。`last_result` 只用于诊断，不参与升级判断。

## Stage 发布合同

发布顺序固定为：

1. 先提交 `stage.valid=false`。
2. 接收或下载到临时文件。
3. 校验 package 长度和 SHA-256。
4. 读取 manifest，并校验 board、target、role 和 raw image identity。
5. 发布为 `/dl/update.tar.zlib`。
6. 保存完整 Stage metadata。
7. 最后单独提交 `stage.valid=true`。

任何中断都会留下 invalid Stage；AUTO 流程不会使用它。下一次 Stage 会完整覆盖，`stage abort` 可以主动清理。

## 安装入口

- `h2loader reboot app`：设置 `boot_intent=AUTO`，选择 Partition 2 并重启；不消费 Stage。
- `h2loader reboot loader`：设置 `boot_intent=LOADER`，选择 Partition 1 并重启；Loader 停留在命令模式。
- `h2loader reboot upgrade`：设置 `boot_intent=AUTO`，选择 Partition 1 并重启；Partition 1 Loader 执行 checksum 驱动的完整 AUTO 流程。

旧的 `restart`、`rollback`、无参数 `reboot`、`reboot ota`、`reboot-loader`、独立 `upgrade` 和 `hold on/off` 不属于设备协议。

详细流程见 [APP 更新](./app)和 [Loader 更新](./loader)。
