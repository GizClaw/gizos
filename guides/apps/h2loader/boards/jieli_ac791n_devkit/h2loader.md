# JieLi AC791N DevKit H2Loader <Badge type="warning" text="WIP" />

本板不在固件发布集合中，canonical `:package` 不标记 `firmware-release`，Release 工作流不提供 AC791N slice。普通 Bazel 构建与 JieLi CI 继续保留。

## 构建

Linux x86_64（或 macOS 上的 Linux x86_64 dev container）：

```sh
bazel build --config=ac791n \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit:package \
  //projects/example/targets/h2loader_tar_zlib/display/jieli_ac791n_devkit:package \
  //projects/example/targets/h2loader_tar_zlib/crash-before-confirm/jieli_ac791n_devkit:package
```

Managed package 内的 `app/jieli/update.ufw` 是 native updater 消费的 image。空片首刷使用杰理 USB UBOOT 工具或烧写器，之后只通过 H2Loader `send` 加 `reboot upgrade` 更新。

### App watchdog 诊断

`//projects/h2loader/targets/h2loader_tar_zlib/loader/jieli_ac791n_devkit:app_watchdog_package` 是手动诊断 App，不属于任何发布镜像：`:app_watchdog_trial` 带 `manual`，package 带 `no-release` 与 `manual`，`scripts/bazel/bazel-release.py` 只选择带 `firmware-release` 的 canonical Loader `:package`，并拒绝 alternate package；`no-release` 仅作诊断标记。

该镜像从共享 color-bar launcher 启动；launcher 在确认 trial 前调用强符号 `h2_jieli_target_application_run()` hook，输出一行 `H2_WDT_TRIAL role=app core=<id> control=0x<wdt_con> action=hang`，随后关闭当前核中断并永久执行 `idle`，使该核停止喂狗。它不修改 watchdog 超时或复位模式，也不调用 `wdt_close()`，用于验证双核喂狗策略在任一核停喂时会复位整板。该镜像始终不确认 trial，因此复位回到 P1 后，`jieli_trial_attempt` 仍匹配 P2 App 的 image checksum，PAL 将该 App 判为不可启动，公共 Loader 留在命令模式。

同一 BUILD 文件中的 `:loader_watchdog_package` 在 Loader stage `105` 执行相同的停喂检查。编译通过不代表复位行为已验证；实机记录保留在 PR #178。

### Audio stop/restart 诊断

`//projects/example/targets/h2loader_tar_zlib/audio-system/jieli_ac791n_devkit:audio_stop_restart_package` 是手动诊断 App（`manual`、`no-release`，不进入发布目录），用于在实机上证明板级 audio provider 能承受受控的停止→重启循环。它复用 audio-system 场景（Opus 音乐播放加麦克风回环）：入口初始化 Runtime、打印 `H2_JIELI_AUDIO_CYCLE baseline heap_free=<n> tasks=<n> cycles=<N>`，启动 image 自有的 `audio-cycle` worker，并在第一轮 `h2_smoke_audio_system_run()` 返回后以 `H2_JIELI_AUDIO_CYCLE_READY cycles=<N> result=<rc>` 返回该结果，使 launcher 在 120 s trial 窗口内确认；随后 worker 持有 Runtime 完成其余循环。循环次数 N 是 Bazel string build setting，默认 10，可用 `--//projects/example/targets/h2loader_tar_zlib/audio-system/jieli_ac791n_devkit:stop_restart_cycles=50` 覆盖；只接受 [1, 1000] 内不带符号、不带前导零的十进制值，`0`、`010`、`ten` 或 `1001` 等覆盖在 analysis 阶段即失败并指出 flag 与取值，不会进入编译或上板。

每轮循环启动场景、流式运行 3 s、经正常 `h2_smoke_audio_system_stop()` 路径停止（失败时按 audio target 的 100 次、10 ms 间隔有界重试），然后用 `h2_jieli_ac791n_devkit_audio_idle_probe()` 检查 provider 已无打开 track、无保留操作、无 PCM ring 字节、无 SDK server 句柄、麦克风关闭且扬声器停止，并最多等待 2 s 让 `os_tasks_num_query()` 回到参考值。每轮打印一行 `H2_JIELI_AUDIO_CYCLE cycle=<i>/<N> run=<rc> mic_frames=<n> music_frames=<n> consumed=<bytes> stop=<rc> stop_ms=<ms> idle=<0|1> heap_free=<bytes> tasks=<n> result=<ok|fail>`：`run=0`、`mic_frames>0`、`music_frames>0`、`consumed>0`（SDK decoder 确实从 track ring 取走 PCM）、`stop=0`、`idle=1` 且 `tasks` 等于参考值才算 `ok`。参考值取自第 1 轮结束后的堆余量和任务数，因此 SDK 首次打开后按设计常驻的分配不计为泄漏，但 baseline 仍打印以便看到首轮差值。结束时打印 `H2_JIELI_AUDIO_CYCLE_SUMMARY cycles=<N> ok=<n> failed=<n> heap_baseline=<b> heap_ref=<r> heap_last=<l> heap_min=<m> tasks_baseline=<b> tasks_ref=<r> tasks_max=<x> result=<ok|fail>`，`result=ok` 要求全部循环 `ok` 且 `heap_last >= heap_ref`。任一轮 `run()` 或停止失败即终止循环，但汇总行始终打印。

Runtime 归属遵循 audio target 的规则：最后一次停止成功后 worker 调用 `h2_runtime_deinit()`；有界清理耗尽时打印 `cleanup did not complete; Runtime intentionally retained` 并保留 Runtime，不释放 worker 仍可能借用的内存。该镜像在循环结束后保持已确认状态留在 P2，主机用 `reboot loader` 返回 P1。

## 分区与启动

`[0, 0x700000)` 由 SDK double-bank packer 管理。当前 pinned WL82 layout 的两个 SFC 映射基址分别为 `0x4020`、`0x37c020`；它们不是可跨 SDK/layout 复用的公共 PAL 常量。稳定 Loader 在 P1；P2 保存 App，或在 Loader 自更新期间暂存候选 Loader。

App 的安装和再次启动不再依赖擦除 BootInfo：

1. Host 仍发送公共 `tar.zlib` package；Stage、镜像身份和 boot intent 由公共 H2Loader Preference 管理。
2. 安装 App 时 PAL 使用 SDK updater 写入并校验 P2，但不发布 P2 native BootInfo。ROM 仍选择 P1 Loader。
3. PAL 在复位前提交以 P2 image checksum 为值的 trial attempt，并写入 board-layout 专用的单次 RAM handoff。P1 在 SDK `main()` 最早期、CPU1 和应用任务启动前消费 handoff，切换 SFC 映射并进入 P2 `_start`。
4. App 返回 Loader 只需复位；不擦除 App BootInfo 或 App 数据。再次 `reboot app` 不需要新 Stage。App 自重启使用同一 handoff，避免意外执行已经暂存的其它升级包。

热启动组件归属 `boards/jieli_ac791n_devkit/ac791n/layouts/h2loader`，由 Loader target 引入；App 只使用共享 handoff ABI。交接结构的 `sfc.app_addr` 是 CPU 地址，不是 updater `BootInfo.codeLength`。修改 SDK pin、映射、保留 RAM 或链接布局必须重新验证，不能仅凭编译通过沿用上述地址。

持久状态与 RAM handoff 的职责不同：boot intent 和 trial attempt 在 Preference 中持久化；位于 `0x01c7fc00` 的 24-byte handoff 只跨软件复位使用，不承诺跨断电保存。handoff 校验 magic、基址白名单和校验值，并在跳转前清除 magic，拒绝重复消费或不完整请求；无有效请求时继续执行 ROM 选中的 Loader。运行分区身份同时检查实际 SFC 映射与 native BootInfo，候选热启动仅允许有效 handoff 作为补充证据。这里的校验不是固件签名或安全启动机制。

重启调度只在定时回调真正执行复位前发布 handoff，请求的目标随回调参数固定。定时器分配失败返回错误，不提前留下可能被后续无关复位消费的请求。`test_jieli_loader_reboot.py` 执行生产函数的主机桩测试，覆盖分配失败、重复调度、目标固定和普通 Loader 复位；它不代替硬件重启验证。

Loader 在 SD 卡 `/dl` 下保存 image 的原始字节影子（`/dl/.h2loader-image-1`、`/dl/.h2loader-image-2`），用于 Partition 2 校验和 Loader self-update 回写。影子不能放在 `/data`：安装 App 时 image writer 完成后会清空 App 的 data root。

Trial 证据保存在 H2Loader Preference：Loader 在请求 App 启动前写入 `jieli_trial_attempt`（P2 image checksum），App 确认时与 `jieli_trial_checksum`、`jieli_trial_reset_reason` 一起删除。回到 P1 时，若 attempt 仍匹配 P2 App，则 PAL 将该 App 判为不可启动，公共 Loader 留在命令模式；此判断也覆盖没有 Stage 的已安装 App 试运行。不能仅因 Stage 等于 P2 就认定发生回滚。

### Loader 自更新及剩余安全边界

Loader self-update 现在复用 SDK updater，但通过本 layout 的 NOR adapter 暂存 P2 的 32-byte BootInfo，不提前改变 ROM 的启动选择。暂存启动头连同候选 SHA、原生代码长度和 CRC 写入 Preference；候选通过热启动进入 P2，公共 Loader 的确认回调验证候选记录，随后在公共回写流程擦除 P1 之前发布 P2 BootInfo。确认前故障可用 `loader_trial_crash_package` 在 stage 105 注入；应检查旧 P1 恢复、候选保留及 coredump，不能以一次复位代替验收。2026-09-13 已实测不同 Loader 镜像之间的正常路径，以及独立的[确认前故障注入恢复](./evidence/2026-09-13/loader-preconfirm-recovery.md)：候选在 stage 105 保存断言记录并复位，自动返回旧 P1，UART 可查询状态并导出匹配的 coredump。这个测试不覆盖任意硬件异常，也不代表断电验收。

此前通过 GNU `--wrap` 截取 SDK BootInfo 写入的实验已撤销：该工具链的内部 LTO 调用没有经过 wrapper。当前 adapter 完整提供 pinned `update.a` 中 NOR I/O member 的八个导出函数，使 archive 不再抽取原 member，固件反汇编已确认 SDK 调用绑定本实现；实际 NOR 读写、擦除和保护操作仍调用官方驱动。该替换限定于本 NOR layout，不支持 `CONFIG_SDFILE_EXT_ENABLE`。不能重新启用只在主机 mock 中有效、实际固件未拦截的实现。

正常自更新实测的候选包 SHA 为 `3af1c30a724182da5b49ae3e77075651cc438b40be2f5fb816a2a1530d243da2`：依次观察到 `LOADER_COMMIT mode=warm boot_info=unpublished`、P2 的 native BootInfo 不存在但逻辑分区为 2、`LOADER_TRIAL confirmed=1`、`LOADER_HEADER published=1`、回写事件 2、原生 P1 启动及完成事件 4。最终状态为 P1/next P1、Stage 空、`last_result=0`，运行镜像 SHA 为 `3a3ae59391c4f4e17b7edde708be1725cc82dca4afc0411e5fcd8051ca22b0f8`。这些证据仅证明正常路径，不证明提交中断恢复。

v2 已完成一次[空闲状态实际断电后的 App 恢复](./evidence/2026-09-13/installed-app-powercycle.md)。最终 v5 也完成了[独立断电复测](./evidence/2026-09-13/installed-app-v5-powercycle.md)：用户断电上电，UART 断开后重新连接，捕获 App 确认与心跳；独立状态查询确认原 App SHA、v5 Loader SHA、Stage 空和 last_result=0。v5 串口重新枚举期间未捕获最早的 Loader 日志，不能宣称记录了全部早期启动过程。提交写入过程中断电仍未验收，本页保持 WIP。

候选启动记录采用固定 112-byte little-endian 编码，不直接持久化 C 结构体填充。字段偏移为 magic 0、代码长度 4、代码 CRC 8、保留字段 10、65-byte SHA 字符串 12、32-byte 启动头 77，尾部 109–111 必须为零。解码同时要求精确长度，后续仍验证候选身份和启动头 CRC。v5 已完成[不同镜像 self-update](./evidence/2026-09-13/loader-v5-self-update.md)、[确认前故障恢复](./evidence/2026-09-13/loader-v5-preconfirm-recovery.md)及下表 UART/BLE 完整回归；不能把这些结果扩展为提交中断或全部 PAL 的验收。

2026-09-13 另做了[无 Stage 的已安装 App 往返验证](./evidence/2026-09-13/installed-app-relaunch.md)：确认 App 后只执行 `reboot loader` 和 `reboot app`，三个状态快照均为 Stage 空，P2 SHA 不变；P1 请求 P2 时没有活动更新或已提交候选，App 重新确认并持续心跳。这项证据独立于安装流程和 E2E 总数，不表示真实断电已经通过。

## BLE

- 控制器配置由 board SDK patch `ble_data_length.patch` 打开 DLE（251-byte LL payload）与 2M PHY。
- ATT MTU 512，ATT send cbuf 为 MTU 的 4 倍。SDK 参考外设固定的 512-byte cbuf 放不下 509-byte notification，会阻塞之后的全部 notification。
- PHY 由 central 选择，PAL 不发起 `LL_PHY_REQ`。CoreBluetooth 自行切换到 2M 后，外设再发起的 PHY 请求会收到 `LL_REJECT_IND`，该控制器在 `ll_slave.c:662` 断言复位。
- Loader 连接后立即请求 15 ms interval。
- Board 不启用 `RF_SLEEP_EN`。开启 RF 睡眠时，控制器会间歇性连续数秒收不到 central 的包并以 supervision timeout 断链，与 PHY、DLE 及 connection update 时机无关；期间两个核的调度间隔最大只有 1–2 个 tick。macOS BLE-only lifecycle 中开启 RF 睡眠 147 次连接断开 9 次，关闭后 72 次连接无断开。

曾经看似需要配置规避的现象都来自两个底层缺陷：compiler-rt `__sync_*` 在双核上丢更新，以及上述 PHY 断言。两者修复后，在同一 DevKit 上逐项复验：1M PHY 下 MTU 512 的多分片 ATT 写入正常；连接后立即请求 15 ms 与 central 的 PHY/DLE 建立交错进行正常；2M PHY 不需要 CSA #2。

macOS 上 854,092-byte App package 的 BLE `send` 端到端 32 s（2M）/ 46 s（1M），2M 稳态约 30 KiB/s。

## E2E runner

macOS 上 BLE Host 由 CoreBluetooth 提供，runner 必须从拥有 Bluetooth 权限的 responsible process（例如 Terminal.app）启动。

```sh
bazel run //projects/h2loader/targets/cc_binary/e2e-runner:e2e-runner -- \
  --uart <serial-endpoint> \
  --ble-id <ble-endpoint> \
  --expected-board jieli_ac791n_devkit \
  --expected-target wl82 \
  --app-firmware <jieli_ac791n_devkit-color-bar-wl82.update.tar.zlib> \
  --loader-firmware <jieli_ac791n_devkit-loader-wl82.update.tar.zlib> \
  --crash-firmware <jieli_ac791n_devkit-crash-before-confirm-wl82.update.tar.zlib> \
  --monitor-ms 3000 \
  --report <report.json>
```

Loader 只有 UART 与 BLE capability，不提供 Wi-Fi 与 HTTP；runner 一旦收到 Wi-Fi 参数就会在 Loader 上执行 Wi-Fi case，因此本板不传 URL 与 Wi-Fi 参数。

## 恢复机制与验收边界

历史实机证据见下方[验收记录](#验收记录)。编译与主机测试不能替代实机断电、UART/BLE 生命周期验收。

### 共享 NOR 写保护窗口

固定 SDK 的 suspend/resume 只有一个备份配置字，不可嵌套，也不保证跨任务窗口安全。disk、Pref 和 upgrade adapter 统一通过 board-owned counted window：首个 owner 保存配置并解除保护，最后 owner 恢复，操作错误/短写同样关闭；恢复失败返回 I/O 并禁止后续窗口直到复位。此合同属于板级 NOR 适配，不改变公共 Loader 生命周期。[SDK 反汇编、27-case host/TSan 回归与 Loader/button/PAL 验收](./evidence/2026-09-15/nor-write-protection.md)记录具体覆盖及硬件状态测量限制。

### P2 残缺头的完整重装机制

选择从完整 P1 通过 UART Loader **完整重装 P2**，由本 board layout 的 NOR adapter 与 pinned SDK updater 拥有机制，公共 Loader 不增加板级修复分支。SDK 的 payload 阶段先从 `target_update_addr - 32 = 0x37c000` 擦除 P2 头扇区并由 adapter 完整读回验证，完整 payload 校验后才 arm/capture；因此不会在 arm 时仍面对旧残缺头。擦除错误锁存直到复位，SDK 伪成功完成也不能继续写入或发布，P1 保持可启动。

直接 arm/publish 不修复非空冲突头；有效但不同的完整 bank 也不得自动擦除，只允许显式完整安装替换。相同头 publish 幂等，P2 arm 拒绝，不在 P2 增加恢复擦除。[固定 SDK 追踪、host 回归与两轮 UART 验收](./evidence/2026-09-15/p2-header-reinstall.md)记录擦除命令/地址、镜像 SHA、独立状态及未捕获确认文本的限制。

## 验收记录

### 2026-09-17：App Wi-Fi 凭据持久化与重启自动重连

`efbabcc3`（PR #459，Issue #454，含 main `3eeb1b55`）在 UID `d879349abc9f` 上完成台架验收：display App 通过 UART `wifi connect` 用 2 s 保存 `HAIVIVI-MFG`，`wifi status` 报告 `state=5 ip=192.168.4.150 saved=1`；此后 `reboot app`、`reboot loader` 再 `reboot app`、全新安装的 button App 每次启动都从保存的凭据自动重连，无需再次 `wifi connect`。错误密码由认证失败事件结束（host 墙钟 6 s，`code=-4 disconnect_reason=11`），不存在的 SSID 在设备侧 15 s `connect_and_save` 预算到期时返回 `TIMEOUT`（`code=-6`；host 墙钟 17 s，另含串口会话建立与状态读取），保存的凭据均不变，随后重启仍自动重连；显式 `wifi disconnect` 后可再次连接。在最终镜像上连续 10 次连接全部成功，无复位、栈溢出或 netif 断言。途中修复了弱 supplicant stub、非阻塞返回值误判、缺失的 SDK Wi-Fi 驱动任务、冷启动须先安装 STA 默认模式、断开时 `wifi_off` 导致的 netif 重复添加，以及 `RtmpMlmeTask` 栈溢出。Loader 仍无 Wi-Fi capability；台架网络未清除（没有对应命令）；未覆盖断电。[逐步结果、镜像 SHA 与边界](./evidence/2026-09-17/wifi-persistence.md)。

### 2026-09-17：Wi-Fi scan 超时恢复

PR #458（Issue #453，验收 head `c289ae4b`）关闭 2026-09-14 PAL review 的 O3：固定 SDK 只在已关联的 STA 下完成 scan；超时后 provider 发布终态 status，STA disconnect 在 SDK 进入配网模式回调 `WIFI_EVENT_SMP_CFG_START` 时释放被放弃的 scan，AP stop/start 在 `wifi_off` 成功后释放。同一 UID `d879349abc9f` 上经 UART Loader 安装 PAL App 后首轮十条 case 全在且 `result=0 passed=10 failed=0`，强制超时序列按合同返回，A/B 对照中去掉该事件复位的镜像在断开后始终返回 `-18`，最终独立 status 为 P1 main Loader、Stage 空、`last_result=0`。合入前的一次镜像曾在 `wifi connect` 时无响应且未复位，30 次重复序列未再复现，原因仍待查。[SDK 事实、返回码、A/B 与边界](./evidence/2026-09-17/wifi-scan-timeout-recovery.md)。

### 2026-09-17：SD rename/open 的 SDK 审计

PR #456（Issue #451，验收 head `5dbfe8c5`）关闭 2026-09-14 PAL review 的 O9：从 `fs.a` bitcode 与实机探针确认 jlfat 拒绝重命名到已有名称、允许以写模式打开目录、`fdelete` 总是消费句柄、`f_free_cache` 不落盘、超过 130 个 UTF-16 单元的组件会被静默截断；provider 改为“删除再重命名”替换、拒绝目录 open 与超长组件（131 字节组件返回 `H2_PAL_ERR_NO_SPACE`）、对有 PAL 句柄的路径返回 `H2_PAL_ERR_BUSY`。同一 UID `d879349abc9f` 上经 UART Loader 安装该 head 的 PAL App 后所有探针按合同返回，UTF-8 短名/长名读写往返成功，首轮十条 case 全在且 `result=0 passed=10 failed=0`，最终独立 status 为 P1 main Loader、`last_result=0`。[SDK 事实、探针结果与边界](./evidence/2026-09-17/sd-rename-open-audit.md)。

### 2026-09-17：Net provider 的 socket 并发与 DNS 生命周期审计

PR #457（Issue #452，基于 main `4fd6e947`）关闭 2026-09-14 PAL review 的 O7：固定 SDK 的 lwIP 只保证每个 socket 一个 reader、一个 writer、一个 closer，Wi-Fi HSM 退出时 `tcpip_uninit` 只清标志，之后任何 `tcpip_send_msg_wait_sem` 永久阻塞；provider 为每个 descriptor 增加 BUSY 门控与栈 generation，PAL Wi-Fi 未启动或已停止时返回 `H2_PAL_ERR_UNAVAILABLE`，stop 先排空在途操作再关闭 SDK 并结算 pending resolver，同步 DNS 改用 `netconn_gethostbyname_addrtype`。同一 UID `d879349abc9f` 上经 UART Loader 安装 PAL App 后首轮十条 case 全在且 `result=0 passed=10 failed=0`，Wi-Fi 27 经过新的 stop hook，最终独立 status 为 P1 main Loader、`last_result=0`。[SDK 事实、host 与实机结果](./evidence/2026-09-17/net-socket-dns-audit.md)。
### 2026-09-17：audio provider 停止/重启循环

`705bd197`（Issue #450，基于 main `4fd6e947`）新增 `audio-stop-restart` 手动诊断镜像，在同一 UID `d879349abc9f` 上以 N=10（package `be7ecd28…`）和 N=50（package `0b7e502e…`）各跑一轮：两轮 `H2_JIELI_AUDIO_CYCLE_READY result=0`、`JIELI_APP_CONFIRM result=OK`，逐轮均 `run=0 stop=0 idle=1 result=ok`，`heap_free` 从第 1 轮结束起恒为 `7171688`、`tasks` 恒为 `16`（baseline `7315688` / `15` 的一次性差值来自 SDK audio server 首次打开），N=50 的 `stop_ms` 在 80–991 ms 之间，汇总行均 `result=ok`；最终独立 status 为 P1 Loader、Stage 空、`last_result=0`。写入阻塞时停止与过期回调拒绝仅由 host 测试覆盖。[全部镜像 SHA、逐步结果、堆与任务数与边界](./evidence/2026-09-17/audio-stop-restart.md)。

### 2026-09-17：System Event provider 迁移到 SDK sys_event

`99f1f89a`（Issue #448，基于 main `e6c7b6aa`）把 wl82 PAL System Event 改为通过 SDK `sys_event` 入队、在常驻 `h2_sysevt` 任务上异步派发；同一 UID `d879349abc9f` 上完成新 Loader 自更新（`H2_JIELI_LOADER_TRIAL confirmed=1`，双分区收敛到 `ed7d71a6…`）、PAL 首轮十条 case 全在且 `result=0 passed=10 failed=0`、UART 25/25（339.512 s）、BLE 22/22 两轮（373.098 s、382.811 s）、button 与 touch 试运行均捕获 READY、`JIELI_APP_CONFIRM result=OK` 与 trial timer 删除并返回 P1、audio-system READY 后 35 s 内 27 条 mic peak 报告；最终独立 status 为新 P1 Loader、Stage 空、`last_result=0`。溢出、SDK 40 s handler 超时与中断 post 仅由 host 测试覆盖。[全部镜像 SHA、逐项结果与边界](./evidence/2026-09-17/sys-event-provider-acceptance.md)。

### 2026-09-17：button 确认控制台在会话下的丢失修复

2026-09-16 定位的 button 确认文本丢失是设备侧问题：一旦主机在确认之前打开可靠 iKCP 会话，App 之后产生的原始控制台写入就不再上线（原始 `cat` 无会话时可捕获，字节级 tee 证实会话下不上线）。修复在共享 App transport 增加会话准入门：确认控制台产生完毕前 App 不应答 `SESSION_OPEN`，这些行沿无会话原始路径上线，随后 App 打开门；一个有界回退期限保证从不确认的启动也不会把主机永久挡在命令通道之外。实测普通 `reboot app --monitor` 现可捕获 `H2_JIELI_BUTTON_SMOKE_READY`、`JIELI_APP_CONFIRM result=OK` 与 `JIELI_TRIAL_TIMER state=deleted id=11`，独立 status 为 P2 确认、Stage 空、`last_result=0`，字节级 tee 显示确认行在 `H2IKCP` 会话应答帧之前上线，PAL 首轮十条 case 全在且无 12 字节乱码，最终返回 P1 Loader。[全部镜像 SHA、独立状态、逐项结果与边界](./evidence/2026-09-17/button-console-flush.md)。

### 2026-09-16：main 硬件验收，button 启动日志捕获失败

`667cd925` 在 UID `d879349abc9f` 上完成新 Loader 自更新、PAL 10/10、UART 25/25、BLE 22/22 两轮及 audio-system READY 后 30.801 秒流式运行。button image startup log capture 判定失败：两次 CLI 捕获均丢失 READY/确认文本；随后经验证的 460800 原始 cat 捕获包含两行，定位为 host monitor 文本路径问题，尚未修复。原 UART 20/20 partial 的外部宽泛 `pkill` 来源已查明。最终独立 status 为新 P1 Loader、Stage 空、`last_result=0`；Wi-Fi 凭据持久化跳过。[全部 SHA、独立状态、逐项结果及开放问题](./evidence/2026-09-16/main-acceptance.md)。

### 2026-09-15：system-event owner 引用计数

Provider 的每次成功 init 各取得一个 owner，Runtime deinit 只释放自己的 owner，launcher/BLE 的订阅继续工作；最后一个 owner 释放后才关闭，并等在途操作退出后销毁。[退出路径的失败回归、TSan 与 button 实机证据](./evidence/2026-09-15/system-event-owners.md)。

### 2026-09-15：共享 launcher 与 Runtime 事件复用

`5b1d822a` 的 ACTIVE event provider 已支持重复初始化；真实 Runtime/provider 主机回归和 button、touch、audio-system 三个生产包实机验证均成功。三个目标通过 UART Loader 安装到 P2，观察 READY、确认成功及独立状态，最后返回 P1。[源码判定、测试与完整验收边界](./evidence/2026-09-15/runtime-event-reuse.md)。

### 2026-09-14：当前源码 Loader 自更新与 UART 回归

源码 `2a814d32`（含 FAT 属性 stat、SDK 单次路径编码和单层 mkdir 修正）在 UID `d879349abc9f` 上完成不同镜像 Loader 自更新：P2 确认并发布启动头后，读取 SD shadow、回写 P1 并收敛；独立 status 确认 P1/P2 SHA 一致、Stage 空、last_result=0。随后在该 Loader 上执行完整 UART suite，25/25 PASS，560.440 秒。两项重连等待明显偏长且原因未定；本轮未覆盖 BLE 或断电。[完整证据与最终板端状态](./evidence/2026-09-14/loader-uart-lifecycle.md)。

### 2026-09-14：当前源码 Loader BLE 连续两轮回归

同一 UID `d879349abc9f`、当前源码 Loader，经重新发现的 BLE endpoint `5:818f070641f0` 完成两轮完整 BLE-only lifecycle：22/22 PASS（385.850 秒）和 22/22 PASS（357.072 秒）。UART 并行记录 34 / 33 次连接，每条连接的 supervision timeout 与 LL reject 均为 0；仍有 SDK `conn nack` 等非致命诊断。独立 UART status 确认 P1 SHA 不变、last_result=0，P2/Stage 保留预期 crash App。未修改代码，不声称已根治历史间歇性问题或覆盖新断电测试。[两轮逐项结果、连接统计与最终状态](./evidence/2026-09-14/loader-ble-lifecycle.md)。

### 2026-09-13：恢复固件后的独立 UART / BLE 回归

在 `3870bf72` 基础上含本文描述的未提交热启动改动，USB DL 使用本次构建输入执行官方 `-format all` 后，设备 UID 为 `3ce9e275d7aa`。报告中的 package SHA-256 是产物身份；不能把未提交构建归为 `3870bf72` 原始产物。

| 路径 | 结果 | 原始报告 |
| --- | --- | --- |
| UART，460800 | 25/25 PASS，283.8 s | [UART](evidence/2026-09-13/lifecycle-summary.md#retained-acceptance-facts-restored-uart-e2e) |
| 首次 BLE | 19/22 PASS，3 FAIL，319.1 s | [首次失败](evidence/2026-09-13/lifecycle-summary.md#retained-acceptance-facts-restored-ble-e2e) |
| BLE，UART 旁路记录日志 | 22/22 PASS，339.7 s | [旁路观察](evidence/2026-09-13/lifecycle-summary.md#retained-acceptance-facts-ble-full-uart-observed) |
| BLE，无 UART 监控，runner 接入连接失败诊断 | 22/22 PASS，324.4 s | [诊断复测](evidence/2026-09-13/lifecycle-summary.md#retained-acceptance-facts-ble-diagnostic-full) |
| BLE，新版延迟发布启动头 Loader（v2） | 22/22 PASS，333.6 s | [新版 BLE](evidence/2026-09-13/lifecycle-summary.md#retained-acceptance-facts-deferred-v2-ble-e2e) |
| UART，新版候选保护，单一读口进程 | 25/25 PASS，284.7 s | [新版 UART](evidence/2026-09-13/lifecycle-summary.md#retained-acceptance-facts-deferred-clean-uart-e2e) |
| UART，v5（显式候选记录编码、修正 P1 回写误报） | 25/25 PASS，268.6 s | [v5 UART](evidence/2026-09-13/lifecycle-summary.md#retained-acceptance-facts-deferred-v5-uart-e2e) |
| BLE，v5，无 UART 并行监控 | 22/22 PASS，342.1 s | [v5 BLE](evidence/2026-09-13/lifecycle-summary.md#retained-acceptance-facts-deferred-v5-ble-e2e) |

这些完整回归包括正常 App 安装与确认、App 命令与传输、跨重启 Stage 保留、正常 Loader self-update、崩溃 App 回滚及 2096-byte coredump 的查询/导出/擦除。测试未提供 Wi-Fi/URL 参数，不覆盖相应能力，也不覆盖实际断电或候选 Loader 崩溃恢复。

首次 BLE 失败发生在 `install-crash-app`，后续 `coredump-status` / `coredump-dump` 因 runner 未取得预期长度而以 0 比较，产生连带失败。首次失败根因尚未确定；后两轮通过不代表该间歇性问题已经修复。runner 新增接线仅转发已有 Host BLE 连接失败诊断，不放宽身份、回滚或 coredump 验收条件。

### 2026-09-13：P1 擦除后的真实断电恢复

[定点断电验收](./evidence/2026-09-13/loader-copy-powercut.md)已通过：候选 P2 确认并发布启动头后，在 P1 启动头擦除完成处暂停，用户实际断电上电。设备继续从 P2 回写 P1，最终两个镜像校验一致、Stage 清空、last_result=0。诊断包使用正式 layout 和 task policy，但含测试停点，不能作为正式发行固件。

随后补齐了以下具体部分写入边界；不能将定点测试扩大为任意损坏模式的保证：

| 中断现场 | 重启后的结果（复位类型见证据） | 证据 |
| --- | --- | --- |
| P2 原生启动头 16-byte 部分写入或 32-byte 错误编程，CRC 无效；P1 完整 | 2026-09-15 当前诊断软件复位后回原 P1；UART 完整重装 button、P2 试运行确认并返回原 P1，两轮均 Stage 清空 | [当前 SDK 完整重装验收](./evidence/2026-09-15/p2-header-reinstall.md) |
| P1 原生启动头仅写入前 16/32 字节；已确认 P2 完整 | P2 恢复回写 P1，最终 P1 启动，Stage 清空 | [P1 部分头](./evidence/2026-09-13/partial-p1-header-powercut.md) |
| Preference 替换期间 Flash 页仅写入前 128/256 字节 | 旧或新值完整，独立 sentinel 保留，重试写入与完整回读成功，P1 Loader 可通信 | [Preference](./evidence/2026-09-13/preference-powercut-plan.md) |
| `lfs_rename` 内部 Flash 页仅写入前 23/256 字节（变化范围 0–45） | 同上；真实 POWER ON 后验证，不混用升级期间旧固件的恢复日志 | [rename 事务](./evidence/2026-09-13/preference-powercut-plan.md#rename-boundary-physical-recovery-pass-for-the-injected-program) |

[历史 P2 部分头测试](./evidence/2026-09-13/loader-partial-p2-header.md)的同身份重装仅证明当时固件的重写、校验与 Stage 清理，不作为当前 deferred-header/O11 实现的验收替代。2026-09-15 的当前源码复测覆盖持久化 CRC 无效的 16/32-byte 头，软件复位验证相同启动现场，不冒充编程中物理断电。未遍历所有 NOR 位损坏组合。

### 2026-09-12：原生更新流程基线

2026-09-12 在 AC791N DevKit 上以 `1bac88f4` 源码构建的三个包完成 UART + BLE 合并回归，46/46 PASS（约 12.4 分钟），BLE 过程中没有 supervision timeout：

- UART 20 项：命令面、payload Stage 与 abort、monitor、`reboot loader/upgrade/app --monitor`、App 安装与确认、App 命令面、两种跨重启 Stage 保留、Loader self-update（P2 候选回写 P1）。
- BLE 20 项：同一生命周期全部经 BLE 执行，包括 App 安装与 Loader self-update。
- crash-before-confirm 回滚后，UART 与 BLE 各自完成 coredump status/dump/erase 与擦除后空白复查。

852 KB App package 的 `send`：BLE 32.6 s，UART 27.6 s。本轮不含 Wi-Fi/URL、真实断电，也不覆盖"Loader 无新 Stage 时启动已安装 App"（见 [JieLi Components](/zh/developing/components/jieli)）。报告与产物身份：[UART + BLE 报告](evidence/2026-09-12/lifecycle-summary.md#retained-acceptance-facts-uart-ble-lifecycle)、[固件包身份](evidence/2026-09-12/lifecycle-summary.md#retained-acceptance-facts-artifacts)。
