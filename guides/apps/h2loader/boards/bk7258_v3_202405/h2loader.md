# BK7258 V3 202405 H2Loader

## 构建

```sh
bazel build --config=bk7258 \
  --//tools/bazel:firmware_version=<version> \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/bk7258_v3_202405:package \
  //projects/example/targets/h2loader_tar_zlib/ble-broadcaster/bk7258_v3_202405:package
```

该命令先通过内部 `:firmware` 构建 AP、CP、combined image 和 `app_ab_crc.rbl`，再通过 `:package` 生成 `update.tar.zlib`。`firmware_version` 同时注入当前固件 metadata 和 package manifest，二者不得使用不同版本。

## Partition layout

内部 8 MiB Flash 的原生启动入口固定为 Loader：

| 区域 | 物理地址 | 物理大小 |
|---|---|---|
| 系统 Bootloader | `0x000000` | 68 KiB |
| Loader CP | `0x011000` | 1156 KiB |
| Loader AP | `0x132000` | 1224 KiB |
| App CP | `0x264000` | 1156 KiB |
| App AP | `0x385000` | 3944 KiB |
| 原生启动控制 | `0x75f000` | 4 KiB |
| 用户配置 | `0x760000` | 124 KiB |
| App 启动记录 | `0x77f000` | 4 KiB |

Loader 合计 **2380 KiB**，App 合计 **5100 KiB**。可执行分区按 68 KiB 对齐；扣除每 32 字节附加 2 字节 CRC，App 的 CPU 可见空间为 **4800 KiB**，还需容纳镜像尾部元数据。尾部 FlashDB、coredump、EasyFlash 与 RF/网络配置保持原地址。`/dl`、`/data` 仍在 SD/FATFS，不占内部 Flash。

Loader 与 App 使用各自的分区表，分别链接到最终地址。App CP 的 XIP 向量位于 `0x02240000`，App AP 的向量位于 `0x02350000`。不通过 BK 原生 B 槽 remap 来执行 App；H2Loader 协议里的 P1/P2 是 Loader/App 逻辑角色，不能直接等同于 SDK 的执行标记。

从旧等大布局迁移必须使用系统烧录路径，安装新 Loader 及其原生 Bootloader 分区表，并初始化启动控制记录。不能把新 Loader 包当作旧布局的普通 self-update。App 的 `all-app.bin` 包含 SDK 打包器生成的引导内容，**不能作为整机恢复镜像烧录**；App 使用 managed `update.tar.zlib` 安装到独立 App 区域。Loader 自升级不再借用 App 分区，当前固定地址后端在写 Flash 前拒绝这种包；Loader 更新使用系统烧录路径。

## 平台配置

Loader 和 H2Loader APP layout 使用 AP 直驱的 UART1 承载 managed UART 与 AP 日志，固定为 460800 8N1。板级 AP defaults 选择 UART1，GPIO 表同时声明 P0=`UART1_TXD`、P1=`UART1_RXD`；SDK 会拒绝映射未出现在该表中的引脚，因此只修改串口号不足以启用 UART1。USB 转串口的 RXD 接 P0/TX，TXD 接 P1/RX，并与开发板共地。UART0 保留 ROM 下载与 CP 日志，UART0 转串口的 RTS 可以接 CEN 控制复位；两路串口可同时连接，均不启用硬件流控。

AP 的 UART PAL 持有 IO Stream iKCP session 和 Loader/App command owner。Loader 只有在 firmware identity 与共享 Loader state 初始化成功后才确认 UART session；随后在 storage mount、publish recovery 和 startup retry 之前启动 UART command task。startup 与 UART/BLE lifecycle/package operation 继续由共享 mutex 串行化，因此 mount 或启动恢复失败时仍保留串口诊断与管理入口。

Host 打开 managed serial 后、借出 stream 前明确 deassert DTR/RTS；不支持控制线的 endpoint 只有返回 canonical `UNSUPPORTED` 才可继续，其它错误立即终止连接。APP 与 Loader status 都从设备端同一个 BLE public/identity MAC 返回 12 位小写十六进制 `device_uid`，BLE 重启后以该 UID 验证设备，而不是信任 endpoint/address。

APP 的 Loader client 必须显式传入 `app_entry_path=app/bk/app_ab_crc.rbl`，
与 Loader 使用同一包入口。共享 client 的默认值仍为 ESP 包入口，遗漏此配置会在
完整接收后以 `H2_LOADER_STAGE_ERROR step=validate code=-12` 拒绝 BK 包。
启动身份校验使用独立 SHA256 context，避免与已经开放的 UART/BLE 收包共用摘要状态。
APP 确认与 Stage/reboot 命令使用同一 operation mutex；先提交运行镜像 metadata，
再确认平台执行标记，成功后才清理匹配的已安装 Stage。
需要耗时自检的 App 应在开放命令服务前初始化并持有 operation mutex，
在同一启动任务完成确认后释放。`ble-broadcaster` 使用此顺序，防止启动期间
新收到的同版本包被延后的确认误认为已安装 Stage 并清理。

原生启动标记始终保持 A，复位总是先运行 Loader CP；Loader CP 在 `driver_init` 之后、无线与 AP 启动之前读取 `0x77f000` 的启动记录，决定是否转入 App CP 的独立 Flash 向量。记录只在发布新请求时擦除一次，其余转换都只清位：

| 记录状态 | 复位后 | 含义 |
|---|---|---|
| 空白 / 其他 | 留在 Loader | 显式选择 Loader，或尚未选择 App |
| 请求（magic 有效、confirmed 为擦除态） | 试运行 App 一次 | Loader 安装完成或 `reboot app` 后发布 |
| 已消耗（magic=0）、未确认 | 留在 Loader，撤销 P2 的 `BOOTABLE` | 试运行在确认前复位或崩溃 |
| 已消耗、已确认（confirmed=0） | 每次直接进入 App | App 启动确认后写入，行为同原生已确认的 B |

Loader CP 在检查 App 向量之前先把 magic 写成 0，所以试运行时向量无效也会记为一次失败尝试，不会反复重启。对已确认记录，这次写入不改变任何位，但必须执行：实板上跳转前没有 Flash 编程操作时，App CP 在启动早期 HardFault（`pc=0`，来自 `bk_pm_module_vote_power_ctrl`）。写入 App 分区前先清除启动记录，CP 不会进入写了一半的镜像。App 每次启动都调用确认，只有记录处于“已消耗、未确认”时才写入。显式选择 Loader 会清除记录，包括失败证据。原生 ROM/系统固件烧录路径保留，CP 不承载 H2Loader UART 转发。

BK 条件变量等待使用栈上的静态信号量，但 SDK 仍为每个信号量分配动态自旋锁。
等待节点从链表移除后必须销毁信号量，再返回并释放栈空间；否则 BLE 高频等待会
耗尽 `CONFIG_SPINLOCK_DYNAMIC_CNT`，触发 `spinlock_mem_dynamic_alloc` 断言。
扩大锁池不能替代配对释放。`sync_cond_test` 同时验证唤醒、超时及创建/销毁计数。

## 预期表现

运行 `bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- scan` 后，只选择结构化 identity 为 `board=bk7258_v3_202405`、`target=bk7258`、`active_role=loader`、`transport=iostreamikcp` 的设备。APP 或 Loader 状态都可以通过 `send --file <build-dir>/update.tar.zlib` 直接发布 Stage；安装使用 `reboot upgrade`。

固定布局验收需要确认：Loader 镜像为 2437120 字节，App 为 5222400 字节；UART1 安装后 App 的实际 PC 落在独立 XIP 地址，SDK native slot 仍为 A；已确认 App 在复位（包括 App 启动过程中连续复位）后由 CP 直接进入；`reboot loader`/`reboot app` 往返；确认前崩溃的 App 回到 Loader 且不再自动启动，重新安装正常 App 后恢复；正常 App 安装不覆盖 Loader。此前等大窗口的自升级验收不能代替此项验证。

2026-09-10 以 Loader、ble-broadcaster 和 crash-before-confirm `0.1.60-fixed-xip` 完成上述矩阵（RTS 接 CEN 复位，未做断电测试）：Loader `pc=0x02143d76`、App `pc=0x0237ad12`，`native_slot=0`；复位后 UART0 只出现 App CP；启动第 0/8/11 秒连续三次复位后 App 正常完成 BLE 自检；crash-before-confirm 在 App 地址 MemFault 后停在 Loader，P2 为该包、Stage 保留；随后重新安装 ble-broadcaster 恢复为已确认 App。

## 旧等大布局的历史验证

以下记录针对迁移前的 A/B 布局，不代表当前固定地址启动和 Loader 更新路径已通过这些矩阵。

2026-09-10 的 UART1 实板验证在 460800 下完整上传并校验 1,691,172 字节的
`0.1.17-bk-e2e` Loader 包，Host 最后一次累计进度报告为 27,804 bytes/s。
自升级完成后确认运行 Partition 1、Partition 1/2 package 与 image checksum 一致、
Stage invalid。该速度包含 Host 上传流程的准备与校验开销，不包含板内解包和写 Flash。
这条结果只覆盖本次 UART 上传和 Loader self-update，不能替代全部 UART/BLE、APP、
coredump 与真实断电重启矩阵；完整报告继续使用同一个 runtime-flag e2e-runner。

同日 `0.1.32-bk-e2e` 的 UART 实板生命周期回归达到 24/24 PASS，覆盖 Loader/App
命令、收包与 abort、带日志的重启、跨重启 Stage 保留、Loader 自升级、未确认 APP
自动回滚，以及 coredump 查询、读取、擦除和空白复查。测试前先确认 coredump 为空，
崩溃 fixture 随后生成并读回 32 字节证据；这不是完整 CPU 栈转储。
该轮未配置 Wi-Fi/URL 下载，也不包含真实断电测试，不能代表这些项目或 BLE 已通过。

BLE App 上传在 `0.1.37-bk-e2e` 复现收包完成后 `step=validate code=-6`。
Host 必须把完整的 `H2_LOADER_STAGE_RECEIVE result=fail` 行视为终态：设备不会再
发送 Stage commit 结果，不能继续等待第二个标记或按传输超时重试整包。该退出行为
已经过实板复现和 `h2loader_host_test` 的分段响应、行边界测试。该次校验错误随后定位为下述 PSRAM 分配问题并修复。

BK App 的 command/package allocator 使用 PSRAM：BLE 活跃时，默认 SRAM heap
无法满足 zlib 的连续 32 KiB window 分配，实测 `inflate` 返回 `Z_MEM_ERROR`。
`0.1.40-bk-e2e` 改用 PSRAM 后，App BLE 上传 1,717,524 字节、校验、Stage 提交
及 status checksum 核对均通过；最终双传输回归结果见下文。

`0.1.41-bk-e2e` 的 BLE 离线生命周期报告为 20/21 PASS：上传、App/Loader
更新、Stage 重启保留、崩溃回退、coredump 读取及擦除后为空均通过。唯一失败为
coredump erase 的成功响应丢失（连接关闭，SDK disconnect reason `0x08`）。
BK Loader/App 的 coredump 扇区擦除循环现于每个成功擦除后休眠 10 ms，
为 BLE 通信任务保留运行机会。`0.1.42-bk-e2e` 的 BLE 定向短测已收到
`H2_LOADER_COREDUMP_ERASE result=OK`，随后 status 确认 368,640 字节区域
`stored_bytes=0 blank=1`。该短测起始区域为空；后续完整回归也验证了崩溃 fixture 数据的擦除。

同版 BLE 完整回归曾因崩溃包上传中的 `WOULD_BLOCK` 提前结束（18/21 PASS）。
BLE 输出重试耗尽会关闭流并保留该状态；Host Stage 恢复现将它纳入已有的有限
重连流程，重新核对 Stage 身份，仅在未提交且状态兼容时重传。
`h2loader_host_test` 已验证未提交 Stage 的背压恢复及完整包重传；后续实板完整回归通过。
完整回归通过不代表该轮必然触发了背压，故障注入覆盖来自上述测试。

`0.1.42-bk-e2e` 配合背压恢复修复后的 Host，BLE 离线完整生命周期回归
达到 21/21 PASS（约 22.8 分钟）。包括 Loader/App 上传、取消、安装、两种
Stage 重启保留、Loader 更新、崩溃回退及 32 字节测试 coredump 的读取、
擦除成功响应、擦除后为空检查。该轮不含 Wi-Fi/URL 或真实断电验收；
同版本 UART 结果见下文。

同一组 `0.1.42-bk-e2e` 包与同一 Host 修复版本的 UART 离线完整回归
亦达到 24/24 PASS（约 22.2 分钟），包含串口 monitor、升级/重启 monitor、
App/Loader 生命周期、崩溃回退以及 coredump 读/擦/空验证。两份报告的
App、Loader、crash 包 SHA256 均与归档产物一致。Wi-Fi/URL 和真实断电
不在这两份离线报告范围内。


最终报告归档于 [UART 报告](./evidence/2026-09-10/uart42-lifecycle.json) 和
[BLE 报告](./evidence/2026-09-10/ble42-recovery-lifecycle.json)，对应
[固件包身份](./evidence/2026-09-10/artifacts.json)。

失败 App 的 Stage 清空后又完成一次 RTS 复位补验：`boot_intent=auto`、
`stage_valid=0`、`active_role=loader`、`running_partition=1`、`next_partition=1`，
未重新启动失败 App。见 [复位后状态](./evidence/2026-09-10/empty-stage-after-reset-status.txt)。
此项为 RTS 复位测试。随后用户确认已断电，重新连接同一 UID 后读取
[断电后状态](./evidence/2026-09-10/power-cycle-status.txt)：Loader 0.1.42、
运行和下次启动均为 P1、AUTO 启动、Stage 为空、last_result=0。
这是故障回退与清理后的掉电恢复验证，P2 仍保留 crash fixture 元数据；
不将其作为 Loader 自升级后 P1/P2 相同镜像的掉电验证。
