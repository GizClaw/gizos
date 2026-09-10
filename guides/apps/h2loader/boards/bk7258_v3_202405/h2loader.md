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

内部 8 MiB Flash 使用 position-independent A/B：

- A / canonical：`primary_cp_app=1360 KiB` 加 `primary_ap_app=2380 KiB`，合计 3740 KiB。
- B / trial：`s_app=3740 KiB`。
- 必要固定区保留 `ota_fina_executive=4 KiB`、`usr_config=128 KiB`、`flashdb=128 KiB`、`coredump=360 KiB`，以及 SDK 使用的尾部分区。
- `/dl` 和 `/data` 位于 SD 卡 FATFS 的 `h2loader/dl` 和 `h2loader/data`，不占用内部 Flash。

分区总长度正好为 8 MiB。A/B 边界变化时，旧布局设备必须按 board recovery 流程重新烧录 combined image，不能直接把新 package 当作普通 self-update。

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

未确认 APP 的回滚使用 SDK 执行标记 `final=A, temporary=B, confirm=A`：
复位后启动 A，同时保留 B 曾被尝试的证据。实际运行 A 时，Power PAL 对这一组合
撤销 P2 的 `BOOTABLE`，使共享 Loader 保留失败 Stage、P2 metadata、boot intent
和 last result，避免再次自动安装同一个失败候选。APP 确认成功写入 B/B/confirm-B；
显式选择 Loader 写入 A/A/confirm-A，不与失败回滚混淆。
开始替换上传会清空旧 Stage，因此没有 Stage 时也必须尊重 P2 的不可启动标记；
只有发布了不同的新 Stage，才允许安装新的候选，不能因上传中断再次启动失败 APP。

BK 条件变量等待使用栈上的静态信号量，但 SDK 仍为每个信号量分配动态自旋锁。
等待节点从链表移除后必须销毁信号量，再返回并释放栈空间；否则 BLE 高频等待会
耗尽 `CONFIG_SPINLOCK_DYNAMIC_CNT`，触发 `spinlock_mem_dynamic_alloc` 断言。
扩大锁池不能替代配对释放。`sync_cond_test` 同时验证唤醒、超时及创建/销毁计数。

## 预期表现

运行 `bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- scan` 后，只选择结构化 identity 为 `board=bk7258_v3_202405`、`target=bk7258`、`active_role=loader`、`transport=iostreamikcp` 的设备。APP 或 Loader 状态都可以通过 `send --file <build-dir>/update.tar.zlib` 直接发布 Stage；安装使用 `reboot upgrade`。

验收必须看到 Partition 2 候选 Loader 启动、自动回写、最终运行 Partition 1，且 Partition 1/2 metadata valid、image checksum 相同、Stage 已清理，并在 power-cycle 后复查。reboot accepted 本身不是完成。已经安装 H2Loader 的正常路径不调用 `bk_loader`；只有 scan、status、`reboot loader` 都无法通信或 Loader 无法自我恢复时，才按 `bk_loader.json` 烧录 combined image，并把它记录为恢复路径。不支持 reliable command contract 的旧 image 只能进入该 recovery 流程，不能使用 legacy raw H2Loader command 迁移。

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
