# 更新、启动与恢复

H2Loader 使用同一套 Stage 和双分区流程管理 APP 与 Loader 更新。流程不保存“安装到了哪一步”的状态机；每次启动都依据实际 package、镜像 metadata、checksum、当前运行分区和 `boot_intent` 作出决定。

## 更新入口

APP 和 Loader 都通过同一设备命令接收 package：

- `h2loader stage <bytes> <sha256>`：从当前 UART、BLE 或 Web Serial transport 接收 package。
- `h2loader stage url <url> <bytes> <sha256>`：通过设备 Wi-Fi 下载 package。
- `h2loader stage abort`：删除 Stage 文件并清空 Stage metadata。

Host CLI 对应 `send`、`send-url` 和 `stage abort`。传输成功只表示 Stage 已完整发布，不会自动安装。

## Package 与 identity

Managed package 支持两种格式，按实际字节识别，不依赖文件后缀：

- Format 1：整个 USTAR 由一条 zlib 流压缩，外部文件后缀为 `.update.tar.zlib`。解压后的顺序是 `manifest`、`checksum`、`data/` 文件、`app/` 镜像；由保留的 `h2loader_tar_zlib` target 生成，继续支持旧设备与新旧格式对照。
- Format 2：外层是未压缩 USTAR，外部文件后缀为 `.update.tar`，顺序严格为 `manifest`、`data.tar.zlib`、`app.bin.zlib`。两个 payload 各自使用独立 zlib 流；data 内层是排序后的 `data/` 文件 USTAR，app 内层直接是 raw image。

两种 manifest 都包含 role、board、target、version、raw image size 和 raw image SHA-256。Format 2 另包含 `data_sha256`、`data_tar_size`、`data_bytes`、`pixa_bytes`、`app_zlib_size`、`app_zlib_sha256`、`data_zlib_size` 和 `data_zlib_sha256`。`data_bytes` 与 `pixa_bytes` 分别统计普通文件与 `.pixa` 文件在内层 tar 中的 payload bytes，提供不依赖解压的安装进度总量，安装时核对实际计数。Data checksum 继续按排序后的每个文件 `path + NUL + bytes + NUL` 计算；与 format 1 的 `checksum`、安装后的 `/data/.checksum` 使用同一 identity。完整 package SHA-256、compressed member SHA-256 与 raw image SHA-256 不能互换。

Format-2 inspection 校验外层 tar、manifest、每段 compressed size/SHA-256、终止和 padding，不解压 payload。安装时分别比较目标 App 分区的实际 SHA-256 和 `/data/.checksum`，只对变化的段执行解压及写入；跳过段直接按 offset 定位，无需解码前面的压缩流。变化 App 校验解压长度、raw SHA-256 及目标分区回读；变化 data 校验解压长度、tar 路径和 canonical SHA-256，成功后才写入 `.checksum`。Data 替换开始前移除旧 `.checksum`，因此失败留下的 partial tree 不会被当作旧 tree 跳过。两部分都未变时不调用安装解压器。Format 1 仍保持原有验证和流式处理方式。

Bazel 双轨并行：保留 `h2loader_tar_zlib` target 生成 format 1，新增 `h2loader_zlib_tar` target 生成 format 2，两者复用原始固件和 data。Native C Writer/CLI 默认生成 format 2，不提供 CLI 格式选择参数。旧 Loader 不接受 format 2；仍可管理的旧设备先使用 format-1 双格式 reader Loader package 更新，再接收新 tar。过渡包保留构建 revision、package/image checksum 和 metadata。Factory/recovery 继续遵守各 board 的既有授权与进入条件。普通与 MFG image 共用公共安装实现；内部 Stage 文件仍为 `/dl/update.tar.zlib`，Preference metadata 和传输命令不变。

`h2loader stats` 在原有 status 行后增加 `H2_LOADER_DATA_CHECKSUM checksum=<64位小写SHA|none|unavailable>`，只读同一配置的 installed checksum 文件；缺失文件为 `none`，不支持或损坏为 `unavailable`。`status` 原有 wire 行保持兼容，不增加持久化状态或设备 model field。Loader E2E 在每次安装前后读取此事实，结合权威 App identity 和 Stage 清理验证四种 checksum 组合；format 2 再用无效未变 zlib 段证明没有解压。

运行时 identity 不读取 Stage 或 Preference：role、version、board 和 target 由构建事实随固件链接；image size 和 SHA-256 由平台直接读取当前运行分区并计算。把 whole-image SHA-256 原样写回同一镜像会形成自引用，因此 checksum/size 采用运行分区的权威计算值，并必须与 package manifest 精确一致；无法读取或计算时启动失败关闭。

持久化状态只描述三个槽位：

- `stage`：已发布 package 及其 image identity。
- `partition_1`：正式 Loader 分区中的 image identity。
- `partition_2`：APP 或临时候选 Loader 的 image identity。

三份 metadata 都包含 `valid`、image checksum/size、role、version、board 和 target；Stage 以及有来源 package 的 Partition metadata 还包含 package checksum/size。`last_result` 只用于诊断，不参与升级判断。

## MFG 进度记录

MFG 进度与升级槽位分开保存在同一 Preference namespace `h2loader`：

| Key | 类型 | 合法值 | 写入时机 |
| --- | --- | --- | --- |
| `mfg` | blob | v4：u32 LE `format=4`、u8 `total`（1..`H2_LOADER_MFG_STEP_MAX`=32）、`total` 个 u8 step status；长度必须恰好为 `5 + total` | `h2_loader_mfg_write`/`h2_loader_mfg_reset`，以及读取 legacy 记录后的一次性迁移 |
| `mfg_acceptance_revision` | u32 | 非 0 产品验收 revision | `h2_loader_mfg_ensure_acceptance_revision` 在重置 `mfg` 之后写入 |

Step status 为 `0` 未测、`1` 通过、`2` 跳过、`3` 失败。步数由产品决定：`h2_loader_config_t.mfg_required_total` 为 0 表示不启用 MFG gate，否则取 1..32，并且只有记录的 `total` 与之相等且每一步都通过时才放行 APP 相关命令。

- 旧 v1（16 字节计数）、v2（24 字节计数加 passed/skipped mask）和 v3（`format=3` 加 22 个 status byte）一律解码为 22 步，读取后原内容改写为 v4。
- 未知 format、长度或 `total` 不合法、status 超出 0..3 的记录视为损坏，读取时重置为 22 步全未测的 v4 记录；key 不存在时 MFG 视为未启用（`total=0`）。
- `h2_loader_mfg_ensure_acceptance_revision(pref, total, revision)` 在以下任一条件成立时重置为 `total` 步全未测并写入 revision：已存 revision 缺失或不同；`mfg` 缺失或损坏；已存 `total` 与请求不同。因此产品从 22 步改为 24 步后首次启动会从头开始产测，而 revision 与步数都未变的 legacy 22 步记录保持原进度。

`H2_LOADER_STATUS` 的 `mfg_mode` 为 `1` 未启用、`2` 启用；`mfg_steps` 按顺序输出恰好 `total` 个十进制 status 数字。未启用时保持旧格式，输出 22 个 `0`。Host Core 接受 1..32 位 `mfg_steps` 并保存解析出的步数，Browser SDK 的 `mfg.steps` 数组长度与之相同。

## Stage 发布合同

发布顺序固定为：

1. 先提交 `stage.valid=false`。
2. 接收或下载到临时文件。
3. 校验 package 长度和 SHA-256。
4. 读取 manifest，并校验 board、target、role 和 image identity。Format 1 验证解压内容，format 2 验证外层结构及各 compressed member 的长度/SHA-256；format 2 的 raw 内容在安装变化段时验证。
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
