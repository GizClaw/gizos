# Portable H2Loader Host Core

`libs/h2loader_host/` 是 Batch Loader internal Web SDK 与 native CLI 共用的 target-independent library。它位于 PAL 之上，不依赖 Swift、AppKit、CoreBluetooth、IOKit、BlueZ、termios、LVGL、CLI 或 Desktop/Web project 类型。

## Ownership

Host Core 拥有：

- 串口与 BLE 候选的独立扫描结果；
- reliable serial 与 BLE-iKCP H2Loader connection；
- authoritative status 解析和 firmware compatibility；
- transport-neutral typed command、authoritative per-command gate、bounded output 与 terminal result；
- Release catalog、资源长度与 SHA-256 校验；
- managed stage、activate、reconnect 与 final-state verification；
- destructive recovery authorization、factory bundle parser 和 raw driver contract。
- frozen factory batch、bounded claim、per-slot result/retry/cancel，以及 JSON/CSV export。

Linux Host Serial 归 `libs/pal/providers/linux/serial_host`，Darwin Host Serial 与 CoreBluetooth 归 `libs/pal/providers/darwin/pal_core`，共同的 termios/session lifecycle 只在 private `libs/pal/providers/posix/serial_host` 中共享。工厂 Batch Loader 通过 Web PAL/Web Serial 消费 Host Core；native CLI 通过 project-owned macOS/Linux target 消费相同 contract。Batch Loader、CLI 和后续 Web SDK 不能依赖彼此的 App、adapter 或 entry source。

## Discovery 与 identity

一次普通 scan 同时启动 BLE 扫描并枚举串口。两个 provider 分别返回 result；一边 permission、timeout 或 unavailable 不会丢弃另一边的候选。不同 transport、相同名称或广播 board 不会自动合并。调用方也可提供当前 BLE backend 生命周期内的 exact `ble_endpoint`：匹配候选完成格式化后立即请求停止扫描；未匹配时以最长 250 ms 的 slice 轮询直到完整 `ble_timeout_ms`，不提供 endpoint 时仍等待一次完整扫描窗口。

Candidate 的 endpoint、USB VID/PID/serial、display name、主机侧可见的 BLE address 和 advertisement 都不是 authoritative physical identity；PAL 的 address type 只描述 controller/backend selector，CoreBluetooth 截断 UUID 明确标记为 `PLATFORM_ID`，不能冒充 BLE identity address。Lifecycle 首次连接从严格 `H2_LOADER_STATUS` 锁定 12 位小写十六进制 `device_uid`；它由设备固件读取 BLE public/identity MAC 后报告。重启后可用 endpoint 重新发现候选，但新连接必须先匹配 UID，再验证 board/target/role/partition。Legacy BLE advertisement 保留 H2Loader service UUID，并把 compact Loader identity 放在 manufacturer data；payload 可以位于 primary advertisement 或独立 scan response。Host 为兼容既有固件也接受相同格式的 Service Data。macOS CoreBluetooth 没有合并 scan response 时，只要 primary packet 是 connectable、完整并携带 private H2Loader service UUID，Host 仍把它保留为 protocol candidate，连接后再从严格 status 取得 authoritative UID、board 与 capability。该 status 保持固定字段顺序与宽度，`capabilities` 只表示 UART/Wi-Fi/BLE 硬件，`command_availability` 表示逐命令 gate；lifecycle 的唯一来源是 `device_uid`、active identity、running/next partition、`boot_intent`、Stage、Partition 1、Partition 2 和 `last_result`。BLE 广播的明文 board 只做连接后交叉检查。

Serial candidate 只尝试 reliable iostreamikcp command transport。Host Core scan 只返回 frozen discovery metadata；native CLI 在 scan snapshot 完成后按顺序使用 candidate 的 exact opaque `port_id` 执行一次 connect/status/disconnect，并把 live identity 或 exact per-candidate PAL failure 投影到 JSON。Timeout 不会触发 retry、legacy raw status probe、transport fallback 或 BLE/serial pairing，也不会产生第二种可管理设备协议；用户可见的在线设备、Firmware action 与 Console session 只来自 reliable serial 或 BLE-iKCP live status 成功。

## Typed command

Console 和 Firmware 生命周期动作只能使用
`h2_h2loader_host_command_t` 是闭集。Request 必须携带从同一 authoritative connection 获得的 live status 和当前 operation fence；Host Core 只检查该命令的 `command_availability` bit，再叠加 Host 自己的 managed-operation serialization。Host 不从 role 或 hardware capability 重建设备 availability。Help/status/stats、条件 memory、Stage abort/URL、Wi-Fi、coredump 与 `reboot app|loader|upgrade` 都使用 closed typed request 和 bounded parameter field；任意文本 command 不属于这个 API。

Firmware 先以 command registration 声明 implemented mask，再由产品 owner 通过 `h2_loader_set_command_availability(loader, flags, available)` 原子 set/clear 运行时 gate。它们只能额外限制 Loader 自身的 MFG、artifact 与 lifecycle 校验。connected status 发布 effective mask，执行路径在所需 operation lock 内重新计算；BLE advertisement 不携带该动态值，serial 与 BLE-iKCP 都以 connected status 为准。

Browser SDK 0.2.0 投影 `deviceUid`、`commandAvailability`、`capabilities`、`active` identity、`runningPartition`、`nextPartition`、`bootIntent`、`stage`、`partition1`、`partition2`、`lastResult` 和 MFG 信息。`H2LoaderCapabilities`、`H2LoaderCommands` 与 `commandAvailable()` 是公共解码入口；生命周期没有旧 packed states、installed/staged scalar 或 APP 专用 status fallback。SDK 的 breaking lifecycle API 只有 `stage`、`stageUrl`、`abortStage`、`rebootApp`、`rebootLoader` 和 `rebootUpgrade`。

Reliable serial 与 BLE-iKCP adapter 都消费相同的 request，按 callback 投影 bounded output，并返回 transport result、terminal kind、output byte count、truncated 与 lifecycle-transition 标记。Cancellation 在写入前、读取后的 bounded boundary 和 Launcher shutdown 上检查；断线不换 transport、不 replay。BLE-iKCP 的无响应写允许在 `WOULD_BLOCK` 后最多重试 40 次、每次间隔 2 ms；Host 与 Loader/App 两端使用相同 bounded backpressure，超过预算仍返回原始 transport error。

Native CLI 在 command parser 之后只创建一个 transport-neutral session。`iostreamikcp` 与 `bleikcp` adapter 分别拥有连接资源，但 `status`、typed command、payload stage 和 disconnect 调用点相同；`send`、`send-url`、Wi-Fi 与 lifecycle command 不注册 transport-specific handler。Serial disconnect 先发送带当前 session ID 的 `SESSION_CLOSE` control frame；Loader/App 收到后只停用该 session，不把它当作 command payload，下一次 open 必须重新握手。BLE endpoint 只负责发现；CLI 从首次 status 锁定 `device_uid`，跨重启的新 connection 必须匹配 UID，缺失或不同都 fail closed。`send` 和 `send-url` 优先在同一 BLE connection 内完成 Stage terminal 与 exact package bytes/SHA-256 status 验证；如果 package 已被完整确认，但同连接的 durable metadata status 暂时不可读，Host 只有在旧 transport disconnect 成功后才能做 bounded reconnect，并按 UID 拒绝替代设备后重新验证。Teardown 失败直接返回其错误，不能同时打开替代 session，也不能按 display name 或 board 选择另一个设备。

三个 reboot 的 `result=accepted` 只表示设备端接受请求。同步返回时还必须得到 `H2_LOADER_REBOOT_FINAL result=OK`；设备真正重启时，Host 在 accepted 后处理 transport close，并通过 bounded reconnect 读取 live status。只有 board/target、预期 role、running partition、boot intent 和三份 metadata 满足请求后的状态，才能投影 lifecycle success；timeout、同名替代 endpoint 或单独的 accepted 都不是成功。

## Catalog 与 operation

`firmware-index.json` 及其全部资源由 CI 聚合，随同一个 Release 原样嵌入 Desktop。Catalog parser 在暴露 entry 前校验 schema、枚举值、safe relative path、唯一性、bytes 和 SHA-256。Managed package 使用现有 `.update.tar.zlib`，recovery 使用 `.recovery.h2fb`，diagnostic asset 不可安装。

浏览器从本地选择 standalone format-1 `.update.tar.zlib` 时没有 Release catalog。Host Core 的 package inspector 通过 caller 提供的 offset reader 按 bounded chunk 读取，计算 archive SHA-256，流式解压 zlib，并复用 Bundle USTAR path contract 校验 manifest、checksum、data 与唯一 App image。它输出 `identity_source=PACKAGE_MANIFEST` 的 immutable managed asset；format 1 不携带 App image name，因此 `image` 为空。只有这个显式 identity source 可以省略 name，既有 `RELEASE_CATALOG=0` caller 仍必须严格匹配 catalog image name，不能从文件名或 chooser label 推断 identity。

Standalone package 的最终校验要求 board、target、role、version、image size/checksum 与对应 Partition metadata 完全匹配，并且 Stage 已按角色流程收尾。APP 必须运行在 Partition 2；Loader 自升级必须完成 P2-to-P1 回写、运行在 Partition 1，且 Partition 1/2 identity 一致。预操作 already-target 与重连后的成功判定调用同一个 verification contract，只有版本相同不能跳过。

Managed operation 对串口和 BLE 使用同一个状态机：

1. Connect 并读取 live status。
2. 校验 board/target 与 asset。
3. Stage 完整 package；中断不自动换 transport 或重放。
4. 执行 `reboot upgrade` 进入 AUTO 流程。
5. Disconnect、重新发现并重连。
6. APP 要求运行在 Partition 2、active/Partition 2 identity 匹配 package 且 `stage.valid=false`；Loader 要求运行在 Partition 1、Partition 1/2 identity 与 package 匹配且 `stage.valid=false`。

传输完成、命令 accepted 或进程退出都不是成功。

## Recovery

Raw recovery 只能使用 serial。成功的任何 reliable serial 或 BLE-iKCP H2Loader probe 都会禁止 raw erase；Launcher 只有在同一 serial candidate 的 reliable probe timeout，并且第二轮完整 reliable probe 仍得到相同结果后，才把时间、结果和 endpoint snapshot 形成短期 authorization。Busy、permission、generic I/O、allocation 或参数错误都不能证明 Loader 不存在。UI 还必须要求操作员手工选择 board/target，并分别确认 physical identity 和 destructive erase。BootROM recovery driver 与 legacy raw H2Loader command protocol 无关。

`.h2fb` 是固定 header 加连续 member payload 的二进制容器。Parser 在 erase 前校验外层 catalog、内嵌 board/target/driver、每个文件名、flash range、payload range、成员 SHA-256、无 flash overlap 且无尾随数据。ESP driver 使用官方 `esp-serial-flasher` C library，并只通过 Host Serial PAL 访问端口。Driver 的 erase/write/readback verify/reset 成功仍只是中间状态；Desktop 必须重新发现并验证 live Loader。

BK7258 driver 在 Host Core 内用 C 实现 Beken HCI ROM download protocol，只消费 Host Serial、Time 与 Memory PAL。操作员或 fixture 必须先让设备进入 download mode；driver 不改变 DTR/RTS。当前维护中的两块 BK7258 board 都使用 8 MiB 内部 Flash，driver 先用 64 KiB block erase 覆盖整颗 Flash，再以 4 KiB sector 写入 exact reviewed `.h2fb`，最后通过 ROM readback 重新计算每个 member 的 SHA-256。任一响应、地址、status 或 checksum 不匹配都禁止 reboot-to-success。产品不能退回 BK7231 工具、SDK subprocess 或内置 Python runtime。

## Threading 与 shutdown

所有 scan、catalog hash、connect、transfer、flash 和 reconnect 在 worker 执行。Worker 只写 caller-owned plain snapshots 与 atomic progress；UI controller 只接收复制后的 structured result。Cancellation 在 bounded I/O 边界协作完成。Shutdown 先拒绝新工作、请求取消、join worker，再关闭 BLE、serial、catalog 和 UI resource。

Scheduler 在 controller thread 上冻结 fixture slot、candidate snapshot 和 asset。`claim()` 最多放出配置的 active 数量；每个 worker 使用独立 transport/operation state，completion 通过 bounded queue 返回 controller 后再写 scheduler。`set_paused()` 只暂停新的 claim，已经运行的 worker 继续到安全终止边界。取消 queued job 不关闭其它 active session；失败或 retry 只改变对应 slot，并继续使用原来冻结的 candidate、asset 和 recovery authorization。

Launcher 的 managed factory batch 最多并发四个隔离 worker。只要 batch 包含 destructive recovery，就必须把整批并发限制为一，因为当前 ESP flasher callback boundary 是 process-global；不能用 UI 上的并发设置绕过。每个 recovery slot 仍需在入队时完成与单设备恢复相同的两次 timeout probe、短期授权和两项人工确认，执行时重新校验授权时效，写入后执行同样的 live Loader final verification。

JSON/CSV writer 是 caller 提供的 byte sink，逐字段转义。当前 export 记录 catalog format、fixture slot、transport、endpoint/candidate/USB identity snapshot、board/target、operation、asset identity 与两种 checksum、时间、retry、final live board/target/role/version/boot intent/checksum、`stage_valid` 以及 exact error。Export 是操作记录，不会把空的 endpoint metadata 升格为 authoritative identity。Launcher 只在整批 terminal 后导出，并先写入、sync 临时文件再替换最终文件；export worker 存续期间 controller 不得修改 scheduler。

## Browser Host Serial

`libs/h2loader_host` 的 serial、catalog、managed operation、ESP recovery 与 BK7258 recovery graph 可以编译为 wasm32，但 Browser 产品能力只由实际验证过的 Web Serial flow 决定。Web launcher 必须从直接用户手势调用 chooser，随后只向 Host Core 注入 opaque port ID；`scan()` 不请求权限。Browser 不伪造 OS path、USB serial、display name 或 board identity，权威 board/target 仍来自可靠连接后的 status。

`projects/e2e/targets/pkg_tar/h2loader-serial/` 只验证 Browser Host Serial contract；它不是产品 UI。[H2Loader Web SDK](/apps/h2loader/apps/batch_loader/) 通过本 Core 与 Web PAL 提供 authoritative status、managed operation、progress 与 final verification，但不暴露 destructive recovery。产品 React UI 位于 `GizClaw/www`，只消费发布的 `@gizclaw/h2loader` Promise API。

Web Serial Promise completion 只记录 generation-tagged result，等待任务由后续 bounded platform pump 唤醒。Host reliable serial connection 在 `open()` 后不读取或设置 DTR/RTS，直接借出 stream；Web 边界也遵守相同规则。重启后的 port 只有在同一授权 registry 中仍能证明为原 `SerialPort` object 时才可重连，不能按 label 或 VID/PID 替换候选。Darwin default-route 查询使用 non-blocking route socket 和一秒 monotonic deadline，保证 Host shutdown/join 不会因为无路由响应永久阻塞。compile/fake/preflight 不能证明真实 status、HELP、install 或 destructive recovery；未执行的 ESP/BK recovery 保持 `SKIP` 并保留风险。

## Validation

```sh
bazel test //libs/h2loader_host:all
bazel test //projects/h2loader/libs/web:all
bazel build //projects/h2loader/targets/npm_package/h2loader:h2loader
```

Fake、PTY 和 cross-compile 只证明 contract 与 host behavior。最终产品验收仍需在准确 reviewed build 上记录 live discovery、authoritative identity、Stage、reboot、partition copy-back 与最终 checksum/metadata。当前 ESP DevKit 已提供 UART/BLE 实板证据；BK 实板因硬件不可用明确 deferred，不能由 build 结果替代。
