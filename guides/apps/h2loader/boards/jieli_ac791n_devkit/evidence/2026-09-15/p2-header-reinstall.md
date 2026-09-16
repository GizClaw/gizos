# P2 残缺启动头：当前 SDK 完整重装恢复 — 2026-09-15

## 选择的机制与责任

选择 **从完整 P1 经 UART Loader 完整重装 P2**，保留现有生产实现；不是重复 publish，也不新增按 CRC 猜测并原地擦除的修复。机制由 AC791N board layout 的 NOR adapter 与其固定 SDK updater 拥有，公共 Loader 不增加芯片专用恢复分支。

`header_arm()` 单独面对非空 P2 头仍失败。完整安装则先由 SDK 在 payload 阶段擦除含头的 P2 扇区，再校验完整 payload，最后才 arm/capture。已有 O11 verified-erase 路径检查 ioctl、按 256-byte 块读回整个擦除范围；错误、短读、非 FF 任一种均锁存 `erase_failed` / `GATE_FAILED`，后续写入、arm、publish 和伪成功 SDK completion 都失败，不能继续提交。该锁存直到复位才清除。擦除范围属于 P2，P1 保持可启动。

相同物理头的 publish 幂等成功；非空且不同的头一律拒绝，**包括 CRC 有效的不同 bank**，不得将其当作 torn header 自动擦除。替换完整旧 bank 只允许显式完整安装。P2 上 arm 拒绝；publish 只写已擦除头或接受相同头，绝不从 P2 增加修复擦除。

## 固定 SDK 的调用顺序

源码基线 `0404a537`，测试/诊断修改 `cd6e5616`。SDK revision 为 `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`，`cpu/wl82/liba/update.a` SHA-256 为 `30d61af6c11905a6a8101027f731538313be1d6810931712672be1eb3d7eb05e`。

按[SDK IR 提取方法](../2026-09-14/pal-review-followup-sdk.md#reproduction)重新检查固定 archive 的 `dual_bank_passive_update.c.o` 与 `flash_fs_api.c.o`：

1. `update_target_update_info_get` 取得 inactive bank，init 将头地址加 32，P1 安装目标代码地址为 `0x37c020`。
2. `flash_update_allow_check_handle` 等待 worker；worker 的 type-1 分支先设置 `curr_erase_addr = target_update_addr - 32 = 0x37c000`，再通知 allow-check 完成。它不依赖旧 P2 头 CRC。
3. worker 在每次 native payload 写入**之前**将擦除游标推进到覆盖 write-end。第一份 4096-byte native payload 从 `0x37c020` 开始，调用 `flash_erase_by_blcok_n_sector`，通过 `dev_upgrade_erase(2, 0x37c000)` 和 `dev_upgrade_erase(2, 0x37d000)` 擦除两个 4096-byte 扇区，然后 `dev_upgrade_write(..., 0x37c020, 4096)`。Loader 初始输入片段可能仍在 SDK cbuf 等待足够 UFW 数据，不应将该输入片段误认为已经写入 Flash。
4. 地址边界检查限定 inactive bank；没有把 `0x4000` 的 P1 头扇区纳入上述擦除。其余 payload 随写入按需擦除目标区域。SDK helper 忽略 erase 返回值，所以必须保留 adapter 的 O11 锁存。
5. Loader 候选路径在完整镜像校验后才 `header_arm()` / `dual_bank_update_burn_boot_info()`。SDK 构造新 BootInfo 时检查运行 P1 头 CRC，随后尝试在 `target_update_addr - 32` 写 32 字节；adapter 暂存它，等待 P2 确认后发布。此时旧 torn 头早已擦除，arm 不再失败。App 路径跳过 arm/burn，保留物理头未发布，通过 RAM handoff 进入 P2，再确认 Preference。

可复核的 emitted IR 位置：`dual_bank_passive_update.c.o.ll` init 1853–1860、allow worker 2490–2504、erase/write 2100–2196、BootInfo 2861–2960；`flash_fs_api.c.o.ll` 的 erase helper 从 216 开始。IR 是 archive 的静态检查，未冒充在 host 执行 SDK。

## Host 回归与构建

`test_jieli_upgrade_io.py` 在真实 adapter 与真实 `update_burn_complete` 上重放上述 SDK 回调边界。16/32-byte torn prefix 各测成功、ioctl 失败、脏读回、短读共 **8 个组合**；先验证 bare arm 拒绝且不擦除，再验证完整重装后 arm/capture 成功、相同 publish 幂等、不同有效头拒绝、P2 arm 拒绝。所有物理写/擦地址均断言处于 P2；错误组合验证 SDK 报成功也不能清除失败。保留原有 adapter 命令和 31 个部分发布前缀回归。

这个恢复测试在未修改的生产 adapter 上已通过，符合“现有 SDK 完整重装就是机制”的选择，不宣称修复了不存在的生产擦除缺口。诊断确有一处需要改动：旧实现安全守卫失败会直接返回 publish observer，可能继续正常发布/回写；新的守卫失败复位，不继续发布。针对 `0404a537` 的真实诊断源码，新增 host 断言在 P2 读失败分支以 `assert(0)` / SIGABRT 失败；修改后 16/32 两种编译配置均通过。正常诊断确认 P1 CRC、预先确认 torn 头 CRC 无效、只写 P2 并完整读回，然后软件复位。32-byte 配置写全部 32 字节并翻转末字节，表示完整长度但编程内容错误；不是声称有效头的前 32 字节仍无效。

严格编译：Apple Clang 21.0.0、OrbStack GCC 13.3.0，均 `-std=c11 -Wall -Wextra -Werror`。macOS Bazel 的 `jieli_partial_header_test`、`jieli_upgrade_io_test`、`jieli_evidence_test` 通过；新增 diagnostic py_test 使用 host-only `target_compatible_with`，`bazel build --config=ios_sim_arm64 --nobuild --keep_going //tools/bazel:all` 通过。未改变线程算法，本次没有新的 TSan 场景。

OrbStack 使用 `--config=ac791n --symlink_prefix=bazel-amd64-` 构建 Loader、button、`loader_partial_header_package` 与 `loader_partial_header32_package`，最终四包构建成功（81.379 秒）。诊断包是 `no-release`，不作为生产 Loader 安装到 P1。

## 当前硬件验收

设备 UID `d879349abc9f`；先重新枚举 `/dev/cu.*`，全程只用 `/dev/cu.usbserial-20131240` @ 460800 UART Loader 安装 P2。未使用 USB DL、format 或其它端口。每个复位后的判断使用新 CLI 进程的独立 `status`，没有超过 90 秒无响应的情况。

保留原 P1 Loader 镜像 `5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada`，包 SHA `9f3eea5602a5919f65dd17d3f293bf5b08e1b1a428c0298da3ac11e59fc5ecae`。[原 P1 构建记录](pal-final-acceptance.md)为源码 `3d7db276`；本次重新检查 `git diff 3d7db276 0404a537 --` 对 `jieli_upgrade_io.c` 与 `jieli_loader_platform.c` 均为空，SDK revision/hash 相同。因此实际负责重装的 P1 含当前 deferred-header / O11 实现，但不声称整份 P1 已换成当前主分支固件。两个诊断和 button 均从本次源码重建。

| 当前重建包 | 包 SHA-256 | 镜像 SHA-256 | 包/镜像字节数 |
| --- | --- | --- | --- |
| loader-partial-header | `4c19ee232486b631b90babcb33734ee32bf80e2ecf3544097e8d751ba38c9823` | `1362ed1c630b5ffb4d1df1919ab4612a55349896003bf612c6d1c8b93d54d6e0` | 927750 / 938697 |
| loader-partial-header32 | `0f18d3df4cfd854a2d982be71bc74aae8c8418f8e012686684ad072148849575` | `8e496b352c53c8d0f67c41fd7fb4f332f5c009c7576a64f373daf11436a0ce5f` | 927780 / 938665 |
| button | `a8cd41c7147212ffeedd21b2c95d29f502516db85e9164536e64fe2713957561` | `046ac9dfba56af0e74e13931a83dc56dbfdccd62d8953f4f2dea081fd2f85208` | 1277681 / 1292049 |

两次诊断 READY 均要求：运行 P2、P1 头解码 CRC 有效、将要写入的 P2 头解码 CRC 无效、写入长度正确、32 字节原始读回逐字节一致。随后调用软件复位；**残缺字节已持久化在 NOR，故此复位与在该已写入点断电具有相同启动头现场**，不是编程指令执行中途真实切断电源的测试。

本轮截取的实际日志（诊断 marker 从紧接着复位的 warm-log replay 取回）：

```text
H2_JIELI_UPDATE_WRITE_DONE expected=938697 native=938697 result=0
H2_JIELI_WARM_LOG [00:00:00.652]H2_JIELI_PARTIAL_HEADER_READY bank=2 prefix=16 p1_crc=valid reset=software
H2_JIELI_UPDATE_WRITE_DONE expected=938665 native=938665 result=0
H2_JIELI_WARM_LOG [00:00:00.653]H2_JIELI_PARTIAL_HEADER_READY bank=2 prefix=32 p1_crc=valid reset=software
H2_JIELI_UPDATE_WRITE_DONE expected=1292049 native=1292049 result=0
H2_JIELI_BUTTON_SMOKE_READY buttons=8 display=480x320 result=0
```

button 的 WRITE_DONE 两轮均捕获；READY 捕获于 32-byte 恢复轮。两轮均有 `task_create: button-smoke/...` 与 `[I][button-smoke]` 事件。没有捕获 `JIELI_APP_CONFIRM result=OK` 文本（两轮各增加一次已确认 App 的重启采集仍未取得该文本）；**试运行确认以独立协议状态的正确 App SHA、P2、Stage 清空及 result=0 为证据**，不把缺失的文本写成已观察到。16-byte 首轮采集脚本因要求该文本而报断言失败，随后已独立核对状态并返回 P1；这是日志采集断言失败，不是把安装返回码当成确认。

| 注入 | 独立 status：诊断复位后 | 独立 status：button 试运行后 | 独立 status：返回 Loader 后 |
| --- | --- | --- | --- |
| 16-byte | `running_partition=1 next_partition=1 boot_intent=auto stage_valid=1 last_result=0`，active SHA 为原 P1 | `active_role=app running_partition=2 next_partition=2 boot_intent=auto stage_valid=0 last_result=0`，active SHA 为上表 button | `active_role=loader running_partition=1 next_partition=1 boot_intent=loader stage_valid=0 last_result=0`，active SHA 为原 P1 |
| 32-byte | `running_partition=1 next_partition=1 boot_intent=auto stage_valid=1 last_result=0`，active SHA 为原 P1 | `active_role=app running_partition=2 next_partition=2 boot_intent=auto stage_valid=0 last_result=0`，active SHA 为上表 button | `active_role=loader running_partition=1 next_partition=1 boot_intent=loader stage_valid=0 last_result=0`，active SHA 为原 P1 |

所有六个检查点的 `partition_1_valid=1`、P1 镜像 SHA 均保持不变。诊断后 `partition_2_valid=1` 是 Preference 内的逻辑候选元数据，**不代表 ROM 已接受物理残缺头**；`running_partition=1` 与诊断 CRC/读回守卫才限定本次观察。32-byte 开始前一次 status 输出夹杂 SDK 文本，使采集脚本解析失败；尚未发送包，重新独立查询取得完整 P1 状态后才继续。

复现命令：`CLI` 指本机已构建的 `bazel-bin/projects/h2loader/targets/cc_binary/cli/h2loader`。每条命令使用 `--no-ble --port /dev/cu.usbserial-20131240 --baud 460800`。先 `status`，`send --file <diagnostic.update.tar.zlib>`，`status` 核对 Stage SHA，再 `reboot upgrade --monitor`；诊断自动复位后停止 monitor，另起 `status` 核对原 P1。接着 `send --file <button.update.tar.zlib>`、`status`、`reboot upgrade --monitor`，停止 monitor 后独立 `status` 核对 App SHA/确认状态，最后 `reboot loader` 和独立 `status`。对两种诊断包各执行一轮。监控进程在约定采集窗口后 SIGINT 退出不作为成功判据。

结论：**16-byte 与 32-byte 两个具体持久化损坏现场的完整重装/试运行/返回原 P1 均通过（2/2）**。不外推为所有 NOR 位损坏、所有写入时序、物理擦除故障或 P1 损坏场景。当前证据取代旧 2026-09-13 记录作为此 SDK/deferred-header/O11 组合的恢复验收依据；旧真实断电记录仍保留其历史范围。

本轮只提交本页内联事实，采集原件和 Issue 草稿留在用户指定的外部 `codex-review8` scratchpad，不提交 raw log/status。`bazel build --config=macos_arm64 //guides:build`、evidence guard 和 `git diff --check` 作为文档验收。
