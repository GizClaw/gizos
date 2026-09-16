# Loader 更新

Loader 更新使用 Partition 2 临时运行候选 Loader，再把候选镜像完整回写到固定的 Partition 1。公共流程不保存 trial/canonical phase，也不依赖 Stage 判断运行在 Partition 2 的 Loader 是否需要回写。平台可以保存启动安全所需的候选记录；它不是公共升级 phase，不能替代公共镜像身份、确认、回写和收尾。

该流程要求 Partition 2 能运行 Loader image。BK7258 的 App window 按自身地址链接，Loader image 按 Loader window 链接；Loader 更新时借用原生 B 槽重映射，把 Loader window 的地址映射到 App window 上运行候选 Loader，流程与本节一致，见 [BK7258 H2Loader](../boards/bk7258_v3_202405/h2loader)。

## Partition 1 AUTO 流程

1. Host 发布 `role=loader` 的 Stage 并执行 `h2loader reboot upgrade`。
2. Partition 1 Loader 重新验证 Stage package、manifest 与全部 identity。
3. 在写入前提交 `partition_2.valid=false`。
4. 完整写入并校验 Partition 2。
5. 从 Stage 提交 Partition 2 metadata，最后设置 `partition_2.valid=true`。
6. Partition 1/2 image checksum 不同时选择 Partition 2 并重启。

如果 Partition 1/2 已经是同一有效 image，Loader 不再切换；若 Stage 也与该 image 一致，则补齐来源 package metadata 并清理 Stage。

## Partition 2 Loader 回写

Loader 只要确认自身 role 为 Loader 且运行在 Partition 2，就执行回写：

1. 公共启动流程先挂载存储、读取状态并登记当前运行镜像的权威 metadata，再调用平台 `confirm_active_image`（如有）。确认失败进入恢复命令模式，不擦除 Partition 1。
2. 确认成功后，通过 PAL `set_next_boot_partition(2)` 确保异常重启仍选择 Partition 2。该调用失败也不得擦除 Partition 1。
3. 提交 `partition_1.valid=false`。
4. 从当前 Partition 2 完整读取自身 image，写入 Partition 1。
5. 写入成功后用当前运行 Loader 的权威 identity 填充 Partition 1 metadata：静态字段来自构建，checksum/size 来自当前 Partition 2 原始镜像。
6. 最后提交 `partition_1.valid=true`。
7. 选择 Partition 1 并重启。

在已提交 `partition_1.valid=false` 后写入失败，不选择 Partition 1，Partition 1 保持 invalid；下次从 Partition 2 启动后重新完整回写。确认或选择恢复分区失败发生在此失效提交之前，应保留原 Partition 1。

候选 Loader 在确认前复位或崩溃、平台回滚到 Partition 1 并撤销 Partition 2 的 `BOOTABLE` 时，Partition 1 Loader 不再自动启动这个候选：Stage 保留，留在命令模式，与失败 App 的处理相同。只有发布不同的新 Stage 才会再次尝试。

## Partition 1 收尾

回到 Partition 1 后，Loader 发现 Partition 1/2 checksum 相同，不再进入 Partition 2。如果 Stage checksum 也相同，则补齐 Partition 1 的 package 来源 metadata，清理 Stage，并留在 Partition 1 命令模式。

Partition 1/2 checksum 相同是 Loader 内部判断回写已结束、可以清理 Stage 的依据，不是外部验收条件。Partition 2 只是回写前临时运行候选 Loader 的中转区，更新后其中是什么不影响结果，下次安装 App 时会被覆盖。

完成条件是：设备从 Partition 1 启动新 Loader，即运行在 Partition 1、active role 为 Loader、active version 与 image 为新包，Partition 1 metadata valid 并记录新包的 image 与 package，Stage invalid。完成条件不检查 Partition 2。公共状态不存在 `loader_upgrade`、phase 或 recovery step 字段。

## AC791N 固定 WL82 layout 的启动适配

AC791N 遵循上述公共确认、P2→P1 完整复制、失败恢复和 P1 收尾合同；以下是其板级启动适配，不是要求其它平台采用的公共存储格式。实现和地址约束见 [AC791N H2Loader](../boards/jieli_ac791n_devkit/h2loader.md)。

- 写入候选 Loader 时，NOR adapter 暂存 P2 的 32-byte native BootInfo，不提前改变 ROM 的启动选择；候选 SHA、代码长度、CRC 和启动头组成固定 112-byte pending-boot 记录，保存在 Preference。对于尚无有效 native P2 启动头的热启动候选，记录缺失、损坏或与候选不匹配时拒绝发布及回写，保留 P1 恢复路径。
- 候选先通过单次 RAM handoff 从 P1 早期启动进入 P2。RAM 请求仅跨软件复位，不是断电后的持久启动依据；持久 trial attempt 用于识别未确认候选。确认前复位仍回到完整旧 P1，保留 Stage/候选及故障证据，进入命令恢复。
- P2 的公共确认回调核对运行身份及 pending-boot 记录。随后公共回写的 `set_next_boot_partition(2)` 才发布已确认 P2 BootInfo，并要求回读验证成功后才允许失效、擦除 P1。启动头发布成功后清除 attempt；确认成功本身不等于已完成升级。已通过有效 native P2 BootInfo 启动的恢复/旧版本升级路径可直接确认并沿用该启动头，不要求重新创建热启动记录；它仍须经过公共回写和 P1 收尾。
- 原始 image 的 SD shadow 保存在 `/dl/.h2loader-image-1`、`/dl/.h2loader-image-2`，为 PAL 镜像读取和 P2→P1 copy-back 提供字节；不能放在安装 App 会清空的 `/data`。确认后复制中断依靠有效 P2 native BootInfo 重新启动并完整回写，而不是依靠已消费的 RAM handoff 或 Stage 驱动另一套复制状态机。
- P1 重新运行后的公共 metadata/Stage 收敛及外部完成条件与上文相同。pending-boot 是板级安全记录，不能作为额外的公共完成条件。

### 验收证据与边界

本次拆分不携带历史实机记录。正常更新、确认前故障、P1 擦除后断电及原生头部分写入需要分别验收；诊断包只提供定点注入，不能代表任意故障覆盖。

主机测试应覆盖公共确认失败不得复制、P2 回写各失败边界及重新进入，板级测试覆盖 pending-boot 解码、发布 gate 和读回失败。实机最终验收必须停止监控后独立查询 UART status，核对运行 P1、新 Loader image/package metadata、Stage invalid 和 `last_result=0`。定点测试结果不推广为任意 NOR/Preference 写入损坏或所有硬件异常保证；SDK pin、布局或启动映射变化后需重新验收。
