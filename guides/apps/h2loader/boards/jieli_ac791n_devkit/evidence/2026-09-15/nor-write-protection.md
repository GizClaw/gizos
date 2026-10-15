# NOR 写保护窗口：SDK 合同与当前源码验收 — 2026-09-15

## 固定 SDK 合同

检查 SDK `include_lib/driver/cpu/wl82/asm/sfc_norflash_api.h`、`include_lib/driver/device/ioctl_cmds.h` 和链接的原生函数反汇编。固定 SDK revision 为 `eb04f1966cf2b7cbb72cbb54db906bcb293b5a4a`；`system.a` SHA-256 `924b12f561aa28ee0b653903946c1c420803151d4286d7f825adb22e1024cbba`，`update.a` SHA-256 `30d61af6c11905a6a8101027f731538313be1d6810931712672be1eb3d7eb05e`。SDK 未修改。

用固定 pi32v2 工具链 `bin/objdump -d <firmware.elf>` 检查 `norflash_ioctl`、`flash_write_protect`、`norflash_protect_suspend` 和 `norflash_protect_resume`，结合[既有 SDK IR 提取方法](../2026-09-14/pal-review-followup-sdk.md#reproduction)检查 updater 的调用点：

| API / 字段 | 固定实现的行为 |
| --- | --- |
| `IOCTL_SET_WRITE_PROTECT`（13） | 参数 0 被转换为 SDK command `0x10000`，传给 `flash_write_protect` 以解除保护。非零参数按原值传递。该函数先写 `flash_info + 68` 的配置字，再操作/检查状态寄存器，返回 0 或错误；失败不意味着未改变硬件。 |
| `IOCTL_GET_WRITE_PROTECT_VALUE`（47） | 将 `flash_info + 68` 的**配置字**写入调用者传入的 32-bit 地址；不是重新读取硬件状态寄存器。 |
| `norflash_protect_suspend()` | 比较当前字 `+68` 和唯一备份字 `+72`；不同则把当前字存入备份，再调用 SET(0)。没有 depth/refcount。 |
| `norflash_protect_resume()` | 当前字与备份不同才 SET(备份)，然后清空备份；相同则直接返回。恢复的是保存的配置字，不是“保护所有区域”，也不是硬件状态的独立快照。 |
| 并发与返回值 | ioctl 内部 mutex 串行化单次驱动调用；suspend/resume 在该 mutex 外访问两个字段，不保护调用者整个写窗口。无操作分支没有稳定的零成功值，不能把这对 `int` 函数当作有完整错误合同的可嵌套锁。 |

因此这对函数**不能嵌套，也不保证跨任务窗口安全**：第一次 suspend 保存保护字 A、当前字变为 `0x10000`；第二次 suspend 可把唯一备份覆盖为 `0x10000`，后续 resume 就不会恢复 A。另一种交错可在别的调用者仍写入时恢复保护。原 disk/Pref 的 SET(0) 从不恢复，更会破坏 SDK 对当前配置字的后续保存。

`update.a` 的 init 调用 `dev_upgrade_protect_suspend`，exit/worker 清理调用 `dev_upgrade_protect_resume`；初始化和清理不应假定同一任务。由 board-owned helper 接管这些回调，而不是把 SDK 的单备份函数各自包一层。

## 修复与失败合同

源码基线 `7ed16cda`（已含重复 BLE log initializer 修复），code/test commit **`b063b1a9`**。新增单一 board helper `h2_jieli_ac791n_devkit_flash_window.c/.h`，disk、Pref 和 upgrade adapter 都使用它：

- 首个成功 open 在 mutex 内 GET 当前配置、SET(0)，记录 owner；嵌套/重叠 open 只增计数，最后 close 才恢复首个配置。SDK transaction 使用一个 token，实际 program/erase 各使用自己的 token；SDK 清理重复 close 不减少别人的 owner。`UINT32_MAX` 不回绕。
- mutex 只保护计数和保护状态转换，不跨调用者的 Flash 操作，避免跨 SDK worker 的整段锁死；物理 NOR 操作仍由 SDK driver 自身串行化。mutex 用 WL82 atomic helper 安全发布，创建失败可重试。
- disk 多扇区 erase、write，以及 Pref 的 LittleFS erase/program，成功、擦除错误、写错误、短写都必经 close。GET 失败不操作 Flash；SET(0) 失败即使已改变硬件也立刻尝试恢复原值。PAL 仍返回 `H2_PAL_ERR_IO`，LittleFS 仍返回 `LFS_ERR_IO`。
- **若恢复 ioctl 本身失败，无法保证物理保护已恢复**：返回 I/O 错误并锁死后续 open，直到复位，不能把失败当成功继续写。fixture 同时覆盖“恢复已生效但 ioctl 报错”和“恢复完全未生效”。这是硬件/驱动失败边界，不再是原先遗漏 restore。
- upgrade 的独立 write/verified erase/header publish 同样有内层 token；外层 SDK suspend/resume 回调共享计数。外层失败保留现有 `erase_failed/GATE_FAILED` 锁存。不会因 Pref close 在 updater 尚工作时重新保护 Flash。

helper 保存的是进入第一个窗口时 SDK 的配置；不会猜测应保护哪些 bank，也不能修复其它代码在进入窗口之前已经丢失的保护字。当前生产 disk/Pref/upgrade 调用者统一接入，SDK 启动期配置仍归 SDK。

## Host 回归与构建

`test_jieli_flash_window.py` 编译真实 helper、disk、Pref Flash callbacks 和 upgrade adapter。fake NOR 跟踪配置/保护状态并模拟 SDK 的 32-bit GET 地址 ABI。共 **27 个场景**：两种 write 各 7 种情况、两种 erase 各 6 种情况，以及一个双阶段 pthread 场景。

- 成功、GET 失败、解除保护失败、erase/write 失败、短写、两类 restore 失败；除实际 restore 失败外，结束时必须恢复进入前的保护字。restore 失败返回 I/O 并拒绝后续窗口。
- 两个线程让 disk/Pref 写窗口确定性重叠，第二个 writer 等第一个调用返回后才继续；任何中途 SET(protect) 都断言失败。两者结束后保护必须恢复。随后再在 SDK 外层 transaction 中重复重叠写及 adapter write/erase，只有最后 SDK resume 可恢复保护。
- 对 `7ed16cda` 的实际消费者源码运行，27/27 均断言失败；修改后严格 Apple Clang 21.0.0 / OrbStack GCC 13.3.0 全部通过（`-std=c11 -Wall -Wextra -Werror -pthread`）。不以编译失败冒充回归失败。
- 原 `test_jieli_pref_program_observer.py` 新增 raw mutation 必须在 window 内且退出关闭的断言；`test_jieli_upgrade_io.py` 新增 raw write/erase 的 bracket 断言。它们对基线分别在 `window_active` / `suspended > resumed` 断言失败，Clang/GCC 修改后均通过；原有 observer、读回、擦除故障和残缺头回归保留。
- Apple Clang ThreadSanitizer 跑完真实 fixture，27 个场景通过，无 race 报告。OrbStack GCC TSan 在进入测试之前因 `FATAL: ThreadSanitizer: unexpected memory mapping` 无法启动，未把它记作通过。
- macOS Bazel：`jieli_flash_window_test`、`jieli_pref_program_observer_test`、`jieli_upgrade_io_test`、`jieli_evidence_test` 均通过；新增 host-only `target_compatible_with`，`bazel build --config=ios_sim_arm64 --nobuild --keep_going //tools/bazel:all` 通过。
- OrbStack `--config=ac791n --symlink_prefix=bazel-amd64-` 构建 Loader、button 和 PAL 包，3/3 成功（74.075 秒）。

复现新回归的 failing-before：在当前工作树执行 `JIELI_WINDOW_BASELINE=7ed16cda python3 tools/bazel/tests/test_jieli_flash_window.py`；GCC 使用 `orb -m embed-zig-noble-amd64 env JIELI_WINDOW_BASELINE=7ed16cda python3 tools/bazel/tests/test_jieli_flash_window.py`。取消该环境变量测试修复；`JIELI_TSAN=1` 启用 TSan。两个既有 fixture 也支持同一 baseline 参数。

## UART 硬件验收

UID `d879349abc9f`；重新枚举 `/dev/cu.*` 后只用 `/dev/cu.usbserial-20131240` @460800。所有安装通过 UART Loader Stage 到 P2，再走正常 `reboot upgrade`；Loader self-update 的 P2 确认、发布头与 P2→P1 回写由正常 install-loader 流程完成。未用 USB DL、format 或其它端口。每个复位后都由新的独立 `status` 进程判断健康，没有 >90 秒无响应。

| 包 | package SHA-256 | image SHA-256 | 包 / 镜像字节数 |
| --- | --- | --- | --- |
| loader | `f96cf7e5ec91b66aa7aba0c2ff1c46c710f7784c24419ffca3ebd8fcdf9fea7a` | `eef1e22a75a51174843937dba6ad155be2f1a3bbda16113eb5fbd9243caa4017` | 927639 / 938473 |
| button | `609207a3004473a1fc49381cd4f731e0c949373eac4100fc2f277116a42f3a85` | `b88a25b870149934ddcc8ea96a9eb5fd27206795eefcba7ace8d2609f8ca3044` | 1277841 / 1292305 |
| pal | `b1fde0e35f8f2d74c65b3fba3aa9270b937a2ca14ca938ff488952e5a5421839` | `111475df6c2d2aa7509683f08bf204a7ffd945d1e2d5fa75ea747780c79cf033` | 890582 / 901833 |

开始时 P1 SHA 为 `5698d9ad9935073970857040c0986fec4c678f26f6fffbfff9ebbeb5b1088ada`。本次按要求先安装修复后的 Loader，故后续 button/PAL 的“返回原 P1”指它们启动前的**新 Loader P1** `eef1e22a75a51174843937dba6ad155be2f1a3bbda16113eb5fbd9243caa4017`，不会再装回有缺陷的旧固件。

实际捕获的 self-update 顺序：

```text
H2_JIELI_UPDATE_WRITE_DONE expected=938473 native=938473 result=0
H2_JIELI_LOADER_TRIAL confirmed=1 publish_gate=before-copy-p1
H2_JIELI_LOADER_HEADER published=1 confirmed=1
H2_JIELI_STARTUP_EVENT event=2 code=0
H2_JIELI_UPDATE_WRITE_DONE expected=938473 native=938473 result=0
H2_JIELI_STARTUP_EVENT event=4 code=0
```

| 检查点 | 独立 status 结果 |
| --- | --- |
| Loader self-update 后 | `active_role=loader running_partition=1 next_partition=1 stage_valid=0 last_result=0`，P1/P2 都 valid 且 image SHA 等于上表 Loader |
| button 试运行后 | `active_role=app running_partition=2 next_partition=2 stage_valid=0 last_result=0`，active SHA 等于上表 button；P1 仍是新 Loader |
| button 返回后 | `active_role=loader running_partition=1 next_partition=1 stage_valid=0 last_result=0`，active SHA 是新 Loader |
| PAL 诊断运行后 | `active_role=app running_partition=2 stage_valid=1 last_result=0`，active SHA 等于上表 PAL；该诊断源码明确不确认 trial |
| PAL 返回与清理后 | 独立 status 先确认新 Loader P1、`stage_valid=1`；仅执行 `stage abort` 清除此轮诊断 Stage，再独立确认 `running_partition=1 next_partition=1 stage_valid=0 last_result=0`、新 Loader SHA |

button 捕获到完整 payload `H2_JIELI_UPDATE_WRITE_DONE expected=1292305 native=1292305 result=0`、`H2_JIELI_APP_COMMIT mode=warm boot_info=unpublished` 和 button-smoke 任务/事件日志。没有捕获早期 READY / `JIELI_APP_CONFIRM` 文本；其确认以独立协议状态的正确 SHA、P2、Stage 空为据，不将缺失文本补写为已观察。

PAL 当前诊断捕获以下实际报告并返回 P1：

```text
H2_PAL_E2E suite=64 case=13 result=0
H2_PAL_E2E suite=1 case=1 result=0
H2_PAL_E2E suite=1 case=2 result=0
H2_PAL_E2E suite=1 case=3 result=0
H2_PAL_E2E suite=1 case=4 result=0
H2_PAL_E2E suite=1 case=5 result=0
H2_PAL_E2E suite=1 case=6 result=0
H2_PAL_E2E suite=1 case=10 result=0
H2_PAL_E2E suite=1 case=11 result=0
H2_PAL_E2E suite=32 case=27 result=0
H2_PAL_E2E result=0 passed=10 failed=0
```

Filesystem 是 SD 文件操作，Core 是 time/timer/task/queue/mutex/semaphore/condition/concurrency；不把 10/10 报告当成 PAL disk 原始 NOR 写入或专门 Pref suite 的覆盖。Loader/button 正常确认、自更新与 Stage 清理实际覆盖 Pref 持久化和 upgrade adapter；disk 的成功/失败/并发恢复由真实-source host fixture 验证。PAL 采集复用了普通 App 的 Stage-empty 断言，因这个诊断**故意不确认**而报错；随后按其源码合同验证 10/10 报告及 P2 身份，独立返回 P1 并清 Stage，不伪称 PAL trial 已确认。

物理写保护状态寄存器和真实 restore 故障未在板上另行测量/注入；本轮验证 SDK 合同、所有调用者的成对路径和正常硬件生命周期。原始采集、包副本和 Issue draft 只存用户指定的外部 `codex-review9` scratchpad。本文保留可审查的 SHA、状态与日志行，不提交 raw captures。文档验收运行 VitePress guides build、evidence guard 和 `git diff --check`。
