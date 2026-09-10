# Loader 更新

Loader 更新使用 Partition 2 临时运行候选 Loader，再把候选镜像完整回写到固定的 Partition 1。流程不保存 trial/canonical phase，也不依赖 Stage 判断运行在 Partition 2 的 Loader 是否需要回写。

该流程要求 Partition 2 能运行 Loader image。BK7258 的 App window 按自身地址链接，不能运行 Loader，因此不支持本流程：Loader package 在写 Flash 前返回 unsupported，Loader 通过系统烧录路径更新，见 [BK7258 H2Loader](../boards/bk7258_v3_202405/h2loader)。

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

1. 先确保异常重启仍选择 Partition 2。
2. 提交 `partition_1.valid=false`。
3. 从当前 Partition 2 完整读取自身 image，写入 Partition 1。
4. 写入成功后用当前运行 Loader 的权威 identity 填充 Partition 1 metadata：静态字段来自构建，checksum/size 来自当前 Partition 2 原始镜像。
5. 最后提交 `partition_1.valid=true`。
6. 选择 Partition 1 并重启。

写入失败时不选择 Partition 1，Partition 1 保持 invalid；下次从 Partition 2 启动后重新完整回写。

## Partition 1 收尾

回到 Partition 1 后，Loader 发现 Partition 1/2 checksum 相同，不再进入 Partition 2。如果 Stage checksum 也相同，则补齐 Partition 1 的 package 来源 metadata，清理 Stage，并留在 Partition 1 命令模式。

Partition 1/2 checksum 相同是 Loader 内部判断回写已结束、可以清理 Stage 的依据，不是外部验收条件。Partition 2 只是回写前临时运行候选 Loader 的中转区，更新后其中是什么不影响结果，下次安装 App 时会被覆盖。

完成条件是：设备从 Partition 1 启动新 Loader，即运行在 Partition 1、active role 为 Loader、active version 与 image 为新包，Partition 1 metadata valid 并记录新包的 image 与 package，Stage invalid。完成条件不检查 Partition 2。不存在 `loader_upgrade`、phase 或 recovery step 字段。
