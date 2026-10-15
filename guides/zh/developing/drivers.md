# Drivers

`libs/drivers` 保存与具体 board 和 SDK 无关的芯片、传感器及外设 driver。Driver 负责设备协议和状态机，实际总线、GPIO、UART、sleep 和同步能力由调用方注入。

## API Reference

[API Reference](/references/drivers)

`libs/drivers` 各 driver 的 `include/` 目录中实际参与项目构建的头文件是 Drivers 的生产 Public API contract。

## Driver Families

### Quectel Modem

`modem/quectel` 实现 Quectel modem 的 AT command、URC、call、GNSS、cell locate、PPP 和状态处理，并输出 `h2_pal_modem_api_t`。Config 注入 transport callback、PAL sync、mem 和 system event API。

Quectel 和 SIMCom 共用 `providers/modem/common:urc`，通过 `h2_tasks` 声明 `$modem/urc`，每个异步 Modem 实例启动独立的 PAL task。接入方在 provider config 同时注入 `urc_task_api`、`urc_queue_api` 和 `sync_api`，并在 firmware target 配置该任务策略；`h2_tasks` 本身不启动任务。

串口接收回调调用 `h2_quectel_post_urc_line` / `h2_simcom_post_urc_line`，复制完整通知后立即返回。公共 worker 按 FIFO 顺序调用厂商解析器，获取 Modem 锁、更新状态和发布 PAL 事件。队列固定 16 项，每项最多 191 字节正文；满队列或过长输入返回 FULL/TRUNCATED，由 transport 处理交付失败，不覆盖旧消息，也不回退到接收线程同步执行。没有配置 worker 的 `post` 返回 INVALID_STATE。同步 AT 解析仍走原有内部路径；transport 负责命令响应与异步通知的分流，不应把同一行重复投递到两个路径。

worker 从 provider init 存活到 deinit。销毁前必须停止并等待外部 API/RX 调用退出；deinit 在 Modem 锁外关闭队列并 join worker，停止时尚未处理的通知可以丢弃。deinit 返回错误时必须保留整个实例并重试，不能释放仍被 worker 引用的资源。worker 回调及 AT command 不能相互等待，也不能在 worker 中调用 deinit。同步轮询 transport 可不配置 worker，但调用方仍须满足原有同步入口的串行化要求。


#### Model Families

UART 接线 profile 和 AT 协议 family 是两个独立维度。`h2_quectel_model.c` 从实际 `AT+CGMM` 应答识别 EC25／EC25- 与 EC800M／EC800M- 家族，并在模块重启或 close 后重新识别。`EC800MX`、`EC25X` 和其他型号不借用这两类的紧急号码／QuecLocator 后端。`H2_QUECTEL_MODEM_PROFILE_EC800M_UART` 可显式要求 EC800M；原 `EC25_UART` 保留现有 EC800M board 的接线兼容性，但具体协议始终由实际型号决定。

型号差异放在 `h2_quectel_ec25.c`、`h2_quectel_ec800m.c`，共用的 AT transport、operation/state lock、URC、通话、PPP 和生命周期保持一份实现。当前适配范围如下：

| 能力 | EC25 | EC800M |
| --- | --- | --- |
| 紧急号码 | `QECCNUM` 的两类模组配置表 | 先探测 `CPBS` 的 EN 存储区，再通过 `CPBR` 读取 |
| LTE RSRP | `QCSQ` | `QENG="servingcell"` 的当前 LTE 小区字段；SEARCH、邻区、缺失／异常值不当作测量 |
| GNSS | `QGPS`／`QGPSEND`／`QGPSLOC=0` | 应用指导定义相同的命令和字段顺序，复用现有路径 |
| UART sleep／SIM hot-plug | `QSCLK`、DTR／RI、`QSIMDET`／`QSIMSTAT` | 共用命令；仍需 board 明确注入接线和 transport 能力 |
| 呼叫音量 | 探测 `CLVL` 等级并映射百分比 | 同样探测等级；厂商音频手册说明设置自动保存，PAL 不承诺易失性 |
| token 型 QuecLocator | 通过配置 token 启用 `QLBSCFG`／`QLBS` | 提供的文档未建立这一后端的支持；不发布该 capability，也不发送 token 命令 |
| 模组固件 URL OTA | `QFOTADL`，EC25 DFOTA 参数与进度 | `QFOTADL`，按固件 flash 标识／包名区分 DFOTA 和 MiniFOTA |

EC800M 核对依据为厂商提供的 LTE Standard(A) AT 命令手册 V1.4（2026-06-30）§8.3–8.4、§11.3，QuecCell 应用指导 V1.6 §2.3，音频应用指导 V1.3 §2.1，GNSS 应用指导 V1.1 §2.3.2–2.3.4，以及低功耗模式应用指导 V1.2 的 UART 接入说明。AT 手册明确包含 EC800M-CN，并注明部分软件版本不支持电话本命令；识别到 family 不代表所有固件都具有所有可选能力。实际拒绝命令仍返回错误，主机 mock 测试不代替型号／固件／接线的设备验收。

#### Module OTA

`H2_PAL_MODEM_CAPABILITY_OTA`（`1u << 7`）表示 provider 提供模组自身固件的 URL OTA 后端。PAL 的 `ota_start`／`ota_get_status` 及对应 wrapper 由 Quectel 接通；SIMCom、Desktop 和无 Modem 后端未接通，调用返回 UNSUPPORTED。Quectel 首次使用确认实际 family，仅 EC25／EC800M 使用该路径；开始前还会以 `AT+QFOTADL=?` 检查固件支持，明确不支持时清除 capability，其他错误照实返回。

现有 `h2_pal_modem_get_identity()` 每次通过 `AT+CGMR` 读取 `identity.revision`。这是厂商的版本标识，按完整字符串匹配；不能按字典序或通用 semver 规则判断新旧。读取失败或回复不完整时 revision 为空，不能据此选差分包。应用根据型号、当前版本及升级包元数据决定是否升级，并选取匹配源版本／目标版本的原始厂商差分包。OTA request 提供一个 URL、必填的 `target_revision`、可选的 `expected_revision` 和每条 AT 命令的启动超时；字符串只借用至 `ota_start` 返回，provider 保存版本标识，不保存 URL。如果当前已经是目标版本，直接报告 SUCCEEDED，不下载；否则源版本不符返回 INVALID_STATE，不发送升级指令。

```c
/* package_url / target_revision come from application package metadata. */
h2_pal_modem_identity_t identity = {0};
h2_pal_result_t rc = h2_pal_modem_get_identity(modem, &identity);
if (rc == H2_PAL_OK && identity.revision[0] != '\0') {
    h2_pal_modem_ota_request_t request = {
        .url = package_url,
        .expected_revision = identity.revision,
        .target_revision = target_revision,
        .timeout_ms = 5000u,
    };
    rc = h2_pal_modem_ota_start(modem, &request);
    /* OK accepts the request; observe ota_get_status in the modem task. */
}
```

EC25 发送 `AT+QFOTADL="<url>"`，HTTP(S)／FTP URL 最长 255 字节。EC800M 的固件标识 `M02`／`M04`／`M08` 表示 MiniFOTA，URL 必须指向 `.mini_1`、最长 128 字节；同名 `.mini_2` 由模组自行下载，服务器须同时提供两个包，PAL 不拼接或下发第二个 URL。`M16` 走 DFOTA，最长 255 字节，并发送 `,0,100`，以 100 条下载报告让计数与百分比一致。未知 flash 标识保守限制为 128 字节，并结合 `.mini_1` 包名选择 MiniFOTA 参数。2 MB 不支持 HTTPS，2／4 MB 不支持 FTP；错误后缀、引号、反斜杠、空白／控制字符、URL fragment 和超长输入在下发前拒绝。HTTP(S)／FTP 的访问及网络配置由模组和接入方负责；普通 EC800M DFOTA 使用模组内部 PDP，接入方须按 DFOTA 指导准备相应上下文，PAL 不通过 host PPP 下载，也不改写接入方 APN／PDP 配置。

`+QIND: "FOTA",...` 经现有命令感知 RX 分流和 URC worker 更新状态，或由已串行化的 task 调用 `h2_quectel_handle_urc_line()`。只有命令 callback 而未接入异步通知，不能获得完整升级进展。状态保留请求序号、源／目标／观察到的版本、阶段进度及厂商结果码；MiniFOTA 没有下载进度时 `progress_valid = 0`，不得伪造百分比。`OK` 只表示请求接受，下载结束或 UPDATING 100 也不表示成功。收到 FOTA END 和之后的 RDY／APP RDY 后进入版本确认，`ota_get_status` 最多执行一条 CGMR；只有 END 结果为 0 且实际版本匹配目标，才报告 SUCCEEDED。版本查询暂时失败保留 VERIFYING 和错误，后续查询可重试；确认到不同版本报告 FAILED。重复 END 通知保留已观察到的重启。

开始要求已 open 且通话／GNSS／host 数据会话已停止；不会自行关闭它们。升级期间保持唤醒，普通 AT 操作、再次启动、close／deinit 和逻辑 transport teardown 返回 BUSY；模块自身的多次重启保留升级状态。USB 等 transport 自动断开时，接入方重新绑定物理命令端口并保留 provider 实例与通知路由，不能把这次重启当作主动关闭。启动超时、丢失完成通知或 MiniFOTA 下载错误可能留下 UNKNOWN／未确认状态；provider 不自动重复发送升级命令、不重启或断电。MiniFOTA 的非零 END 也可能留下下载系统，此时即使读到版本仍保持会话，交由接入方按厂商流程恢复。状态仅在当前 provider 实例中保存；跨主控重启的升级意图和恢复策略由应用负责。

EC25 协议依据为 [EC2x&EG2x-G&EG9x&EM05 DFOTA Application Note V1.1 §3.2.1、§4](https://quectel.com/content/uploads/2024/02/Quectel_EC2xEG2x-GEG9xEM05_Series_DFOTA_Application_Note_V1.1-2.pdf)。EC800M 依据为本次提供的 LTE Standard(A) DFOTA 升级指导 V1.5 §3.3.1、§4–5。上述测试是 host transport／URC 测试；实际升级包、运营商网络、供电及端口重绑定须在对应模组固件上验收。

#### Emergency Numbers

`h2_pal_modem_get_emergency_numbers()` 是只读、阻塞的 Modem PAL 查询，vtable 的 `get_emergency_numbers` 和 `H2_PAL_MODEM_CAPABILITY_EMERGENCY_NUMBERS` 暴露该能力。调用方提供号码数组、容量和 count；每条结果保留号码字符串、无卡／有卡适用范围、查询来源和可选类别。完整列表才返回 OK；容量不足或 transport 截断返回 TRUNCATED，格式错误返回 FORMAT，其他失败保留 provider 错误。有效输出参数在失败时清空，不能把旧表或部分表当成本次查询结果。

EC25 后端每次实际发送 `AT+QECCNUM?`，解析 type 0 的无 (U)SIM 表和 type 1 的有 (U)SIM 表，保留前导零、两类表中的重复号码以及回包顺序；号码不从手册默认值或 Example 中补齐。两类表各最多 20 个号码，响应必须包含两类表且不能重复 type。返回来源为 MODULE、类别为未知，表示查询到的模组配置表；该指令并未标明单独的 SIM 文件／网络来源，不能据此声称枚举了全部有效紧急号码。

EC800M 后端先用 `AT+CPBS=?` 确认支持 EN，保存 `AT+CPBS?` 报告的原选择，再选择 EN、查询 used／total 和 `AT+CPBR=?` 的实际支持索引。单值、稀疏或重叠区间会排序合并，每页最多 8 个索引；读取超过 16 个区间或 4096 个不同槽位返回 TRUNCATED，这是 provider 的事务边界，不是模组容量声明。每页通过 collector 读取完整条目；实际条目数必须与 EN 的 used 相符，不能把空应答、错误或缺页当作完整空表。EN 在手册中表示 SIM 或 ME 紧急号码，未给出两者的独立来源或无卡／有卡适用范围，因此返回 UNKNOWN，不能根据当前 SIM 状态猜测。

选择 EN 和恢复原选择属于同一 operation lock 下的事务，不发送 CPBW 或号码增删命令。原选择已经是 EN 时不重复设置。恢复失败时查询失败、输出清空，并在下一次查询或 close 时重试；close 仍按原有合同确认通话／GNSS／数据停止并 teardown，单独的电话本恢复失败不能阻挡关闭，但会返回其错误。已关闭的会话丢弃恢复状态。SIM/reset 失效时拒绝旧回包，不在失效事务中重放选择；下次操作先重新确认型号。

该查询要求 Modem 已 open；关闭时返回 CLOSED，不自行开机。不以 SIM READY、网络注册或 packet data 作前置门槛，也不拨号或修改号码；固件若因 SIM 状态拒绝电话本访问，则照实报告错误。调用在 Modem task 中执行，复用 operation lock、唤醒／休眠保持、超时恢复及 SIM/reset generation 检查；查询期间换卡或模块重启会拒绝旧回包。Provider 不缓存号码表；应用若缓存，须在模块重启、SIM 或相关网络状态变化时失效。`timeout_ms = 0` 使用已配置的 command timeout，超时参数应用于每次 AT 事务。长 AT 应答用逐行 collector 解析，AT 行上限 768 字节；普通 response 数组和 URC 队列仍使用独立的 192 字节行容量。Command transport 必须尊重传入 response_size、保证 NUL 结尾，并提供完整的本次应答。

Capability 表示 provider 实现了查询入口；具体型号／固件拒绝指令或查询失败时照实返回错误，不能把它解释成空号码表，也不能据此保证网络接通。当前 SIMCom、Desktop 和无 Modem backend 未实现此能力，调用返回 UNSUPPORTED。`libs/app_test` 的 Modem fake 可由 scenario 注入表和失败，默认不启用该 capability。

`QECCNUM` 是移远扩展；3GPP 的 `AT+CEN?` 是可选的网络紧急号码查询，两者并不等价。协议依据见 [EC25&EC21 AT Commands Manual V1.3 §7.19](https://quectel.com/content/uploads/2021/03/Quectel_EC25EC21_AT_Commands_Manual_V1.3.pdf) 和 [TS 27.007 §8.67](https://www.etsi.org/deliver/etsi_ts/127000_127099/127007/17.06.00_60/ts_127007v170600p.pdf)。公开参数、ownership 与错误合同以 PAL header 的 API Reference 为准。

#### Cell Locate

Cell locate 是 QuecLocator 基站定位，与卫星定位是两条独立路径：它不依赖卫星信号，室内和冷启动也能返回粗略位置，代价是每次查询都要经 packet data 访问运营商定位服务。Provider 用 `AT+QLBSCFG="token",<token>` 配置身份、`AT+QLBS` 发起单次查询（成功应答为 `+QLBS: 0,<经度>,<纬度>[,…]`，经度在前），结果通过 `h2_pal_modem_cell_locate()` 返回 `h2_pal_modem_cell_location_t`。`valid = 0` 表示服务未能定位，是正常返回而非错误。何时查询、缓存多久、如何与 GNSS fix 融合都属于产品策略，不在 PAL 或 provider 内决定。

Token 由集成方通过 `h2_quectel_modem_config_t` 的 `cell_locate_token` 注入，字符串是借用的，生命周期必须覆盖 modem instance；`cell_locate_timeout_ms` 控制单次查询超时，0 取默认 60 s。Token 为 NULL 或空串时 `cell_locate` 不装配进 vtable，`get_capabilities` 不置 `H2_PAL_MODEM_CAPABILITY_CELL_LOCATE`，调用返回 `H2_PAL_ERR_UNSUPPORTED`，模组上不会出现任何 QLBS 命令。配置 token 仅表示候选后端；首次使用前通过 CGMM 确认 EC25，EC800M／未知型号清除该 capability 并返回 UNSUPPORTED。含引号、逗号或换行、或超过 127 字节的 token 会让 `h2_quectel_modem_init` 返回 `H2_PAL_ERR_INVALID_ARG`。

前置条件是 packet data 已激活，provider 不会自行拉起 PPP；数据不可用时返回 `H2_PAL_ERR_INVALID_STATE`。Token 在首次 `cell_locate` 时惰性下发一次，`close` 后重置，因此没有用到基站定位的产品完全不会发出 token。

Token 是企业身份凭据：仓库不提供默认值，也不接受把真实 token 写进代码、测试或注释。除了必须携带它的那一条 QLBSCFG 命令外，token 不进入 modem state、response buffer、错误信息和任何日志输出；模组回显 token 时该次调用按 `H2_PAL_ERR_IO` 失败。返回的坐标同样不写入日志。

### Lierda NT26-KCN B

`libs/pal/providers/modem/lierda` 提供显式选型 `NT26KCNB20NNC` 的 data-only Modem PAL。配置复制 APN 和 callback 表，借用 allocator、sync 与 transport context 至成功销毁。GPIO、UART、供电/复位时序、SIM/APN 策略及默认路由归 BSP/产品；driver 不把 CGMM 文本当作产品料号，也不从品牌或前缀推断型号。Identity 使用标准 CGMI/CGMM/CGMR/CGSN/CIMI 的有界信息文本；需要完整身份时，失败或截断清空输出并传播错误，不打印身份或认证数据。

型号与 PPP/UART 能力依据利尔达编写的 [NT26-KCN B 硬件设计手册 Rev2.3](https://atta.szlcsc.com/upload/public/pdf/source/20260202/50AC12B44A6E1249D82013542CE67719.pdf) 选型表、§2.2 和 §4.2.1。所用 CPIN、CEREG、CGATT、COPS、CSQ 与 CGDCONT 属于 [TS 27.007 V17.6.0](https://www.etsi.org/deliver/etsi_ts/127000_127099/127007/17.06.00_60/ts_127007v170600p.pdf) 标准命令；host PPP 拨号与 COMMAND/DATA 转换交给已固定的 [esp_modem 1.4.3 Generic DCE](https://github.com/espressif/esp-protocols/tree/modem-v1.4.3/components/esp_modem)。完整私有 AT/PPP application note 未公开验证，因此不添加供应商私有指令、CALL/GNSS/OTA、CMUX、SIM hotplug 或低功耗 capability。现有其他 NT26 系列与 firmware 功能选配不在此 profile 的支持声明内。

所有控制操作由 mutex 串行化；callback 不得重入 provider。单 UART 的 OPENING/OPEN/CLOSING data 状态下，AT-backed getter 和 `set_apn` 返回 BUSY，PPP 数据不会输入文字 parser。Data open 先确认 SIM READY、HOME/ROAMING 与 packet attach，再设置 IPv4 PDP context，并要求 transport 确认真实 PPP IPv4 地址；未就绪由产品重试，不自动修改 SIM 或网络偏好。`get_data_status` 只查询 host link，不发送 AT。CSQ 仅 `0..31` 有效，其余非负值（包括 99）报告 RSSI invalid；负数、整数溢出、重复信息行、非法 BER 或不完整响应返回 FORMAT/TRUNCATED。此 profile 不查询 RSRP，RAT 仅在明确的标准 COPS AcT 中报告，不能从 RSSI 或 SKU 推断。

Close 通过 transport 关闭整个模块，允许在 graceful data close 失败后恢复；失败保留 instance、mutex、callback 与 dependency 生命周期供重试。销毁前必须停止并 join 外部调用者。具体 ESP 接线与 SDK callback quiescence 见 [ESP-IDF Lierda UART PPP](./components/esp_idf6_x#lierda-data-only-uart-ppp)。Host protocol/state tests 证明有界解析与失败恢复，不能替代真实 SIM、运营商、功耗、UART 波特率和 PPP acceptance。

SIM 查询按 TS27.007 V17.6.0 §8.3、§9.1 和 §9.2.1 区分标准终态。可选 `get_command_error` callback 只提供本实例刚返回的命令 CME 数字元数据，每次命令/拒绝/timeout/close 前清空；没有该 callback 的 transport 保留原 IO 行为。只有 CPIN 本次完成的可信 CME14 才返回 WOULD_BLOCK，输出清空，调用方保持模块供电并按产品定义的间隔/截止策略轮询；一次 getter 不自动 sleep、重置或关电。CME10 返回 ABSENT，明确 PIN/PUK requirement 返回 LOCKED，随后不查询注册/attach、不拨号。CME13/15、未知码、非 CME IO 或不完整回复不当作无卡/忙。PIN2/PUK2 表示当前 credential requirement，不能据此宣称所有 MT 操作都不可用。成功 CPIN 的 READY/标准锁定码也精确匹配，不使用 PIN/PUK 子串推断。Driver 不提供 PIN 猜测、自动输入或模组复位策略。

### QMI8658

`motion/qmi8658` 实现 QMI8658 IMU 初始化、打开和采样。Transport object 提供 register read/write 与 sleep callback。

### SC7A20H

`motion/sc7a20h` 提供 SC7A20H 的同步 I²C 加速度采样，依据士兰微 [SC7A20H 产品页](https://www.silan.com.cn/en/index.php/product/details/3235.html)和 [SC7A20H 说明书 v1.1](https://www.unikeyic.com/media/datasheet/4c/2e/d932/4c2ed93210c2506ae969004cfd38fd37.pdf) §11.1、§13.2–13.14、§13.37 实现。配置支持四档量程和高性能模式的数据率；打开时联合校验 `WHO_AM_I=0x11`、`VERSION=0x28`，开启 BDU、小端 XYZ 和 FIFO bypass。每个 sample 先检查三轴 data-ready，再以 `0xA8` 连续读取六字节。mg 换算使用 §13.13 的数据示例和满量程比例，不能把 ±4g 当作 ±2g 缩放。

调用方注入 allocator、精确 I²C register transport 和可失败的 sleep；7-bit 地址 `0x18/0x19`、bus、GPIO 和同步归 BSP。Driver 不覆盖保留寄存器和工厂校准区，不实现 SPI、FIFO 消费、运动算法或 GPIO IRQ provider。INT1 可选输出 data-ready，仍由调用方接线。公开 header 定义 ownership、串行调用、未就绪返回 WOULD_BLOCK 和失败清零；close/destroy 的 power-down 失败保留 instance 供重试。Host 测试覆盖身份、量程换算、burst、重复 lifecycle、配置/读取/cleanup 失败。

### FM175xx

`nfc/fm175xx` 实现 FM175xx reader、ISO 14443 Type A card activation 和 NTAG 数据读取。Transport object 提供 register I/O 与 sleep callback。

每次 activation 先以 HLTA 结束前次扫描留下的 ACTIVE selection，再用 WUPA 唤醒 IDLE／HALT 中的卡，随后执行 anticollision 和 SELECT，保持 selection 给后续 NTAG READ。HLTA 不返回应答，其 RF timeout 是协议规定的正常结果；其它总线、寄存器和协议错误继续返回失败。调用方串行化 scan 与 read，读内容前重新激活并检查 expected UID，不把持续贴着的卡因重复 REQA 无应答误判为拿开。Host `//libs/drivers/nfc/fm175xx:activation_test` 以寄存器／FIFO transport 模拟 IDLE、ACTIVE、HALT、移卡和总线失败，覆盖重复扫描之后的完整用户区读取。

### FM17660K

`nfc/fm17660k` 实现 reader、card emulation、FIFO 与 RF protocol state。它只依赖 exact `write_reg/write_regs/read_reg/read_regs`、reset、monotonic time 和 fallible sleep。连续读写是否固定在 FIFO data register、UART command byte/read flag/echo validation 都是芯片协议，归 portable driver；I2C/SPI/UART controller、exact-byte readiness、IRQ ring、pin 和 SDK handle 归调用方。Public transport 不暴露 `fifo_read/fifo_write`、partial offset、UART type 或 libco。

## 依赖和边界

Driver 可以知道具体 device protocol 和 register，但不能知道 board pin、I2C controller instance、UART port、interrupt wiring 或 SDK handle。这些 board 差异由 BSP 组装 transport 时提供。

## 构建与测试

每个 driver 目录都是独立 Bazel package，并通过自己的 semantic target 暴露 portable driver；不能再用一个 `//libs/drivers:drivers` 聚合生产 target 隐藏实际依赖。跨 driver public-header contract 位于 `//libs/drivers/tests`，各 driver 的协议、状态机和错误路径测试仍由对应 package 或共享 tests package 执行：

```sh
bazel test //libs/drivers/...
```

## Quectel 低功耗与 SIM 热插拔

Modem 的 ACTIVE/AUTO_SLEEP 是易失策略；实际状态独立报告。当前 Quectel provider 没有可信的休眠状态传感器，`get_power_status` 始终报告 UNKNOWN，也不发送 AT 或为查询唤醒模组。成功设置 AUTO_SLEEP 只表示允许空闲休眠。关闭后策略回到 ACTIVE；意外 RDY 后在下一次功能调用恢复配置。AT 或 sleep gate 失败会禁止自动释放唤醒，调用方可重试设置策略；尚未确认结束的通话、GNSS 和数据会话仍须成功停止。

支持范围使用 EC25／EC800M UART profile，并在准备时通过共用的 CGMM 识别校验型号。显式 EC800M_UART 不接受其他型号；原 EC25_UART 保留 EC800M board 的接线兼容性，协议分派仍跟随实际型号。未声明 profile、缺少独立 command channel、sleep gate 或 PAL recursive mutex 时不发布 LOW_POWER capability，调用返回 UNSUPPORTED。其他 Quectel 系列、USB-only 接线及 SIMCom 等 provider 不因通用 capability 配置而获得低功耗支持。独立 command channel 可以是由 transport 维护的 CMUX AT DLCI；PPP 数据 DLCI 活跃期间保持唤醒，不依赖未经验证的 CMUX/PPP 休眠行为。

[EC25 Hardware Design V2.4](https://quectel.com/content/uploads/2024/02/Quectel_EC25_Series_Hardware_Design_V2.4-4.pdf) §3.4–3.5.1.1 说明普通 sleep 保留网络寻呼及语音来电，UART 场景用 QSCLK 与 DTR 配合，DTR 拉低唤醒，RI 通知主机。Provider 使用 QSCLK 0/1，不使用关闭 RF/SIM 的 CFUN 模式。Board 的 sleep gate 负责 DTR 电平、唤醒后 transport 就绪等待、所有 DLCI 排空、RI 唤醒及无损 URC 接收；存在 USB、WAKEUP_IN 或 AP_READY 时还要满足该板接线条件。官方资料未给出适用于所有固件和接线的固定唤醒延迟，因此 portable provider 不硬编码通用毫秒值。

完整 PAL 操作和 public PPP/prepare 入口共享 recursive mutex。定位的 token 配置与查询不会被关闭或策略更新穿插；GNSS 从启动到停止、通话从拨号/来电到挂断或结束 URC、PPP 从拨号到停止均保持活动。持续活动期间即使策略是 AUTO_SLEEP，也不会允许休眠。Cell locate 仍要求调用方先使 packet data 可用，provider 不建立 PPP；它依赖 QuecLocator 的结果/CME 错误判断数据不可用，不能把本地 PPP 缓存当作模组内部 PDP 激活证明。

[EC25/EC21 AT Commands Manual V1.3](https://quectel.com/content/uploads/2021/03/Quectel_EC25EC21_AT_Commands_Manual_V1.3.pdf) §5.10–5.11、§13.5 定义 QSIMDET、QSIMSTAT 和 QSCLK。启用热插拔需明确 SIM_DET 插入有效电平并提供 host data invalidation callback。准备流程读取 QSIMDET；已匹配时直接启用 QSIMSTAT 通知，无需重启或 sleep_gate。配置不一致时写入期望值；提供可选 restart_module callback 时，provider 使旧会话失效，再由集成方断电/复位并恢复 AT transport。Callback 使用 transport_user，在 task context 中保留 operation lock、释放 state lock；集成方负责有界等待 AT 就绪并排空启动 RX/URC，不得重入命令/生命周期 API 或等待需要 operation lock 的任务，允许 RX/URC 交付。成功后完整重跑 AT、回显/错误、通知配置及型号校验，确认 QSIMDET 读回目标，再下发 QSIMSTAT=1 和可选 QSCLK。每个 instance 最多尝试一次重启；回调失败原样返回错误，读回不符返回 INVALID_STATE，不重复写入或重启。未提供 callback 时维持 INVALID_STATE 锁存语义，集成方须外部重启模组、销毁旧 instance 并重新初始化。Provider 不写产品偏好，配置命令失败则 open 失败。

Prepare 中的 SIM 通知仍更新状态和使旧数据失效，但不会仅因 sim_generation 变化而中断与 SIM 无关的配置命令；reset_generation 变化仍返回 INVALID_STATE。Prepare 完成后的命令继续检查 SIM generation，防止拔卡后接受过期结果。

QSIMSTAT 的 absent、inserted、unknown 结合 CPIN 的 READY、SIM PIN/PUK、NOT READY 复用 MODEM_SIM_CHANGED；插入通知本身不等于 READY。重复状态被合并；拔出后的 NOT READY 不覆盖已知 ABSENT。无卡、锁卡或失效状态会清除 provider 的数据/IP 状态并触发 host invalidation callback，旧 PPP link-up 回调必须被 consumer 丢弃。重新插入只通知状态，不自动拨号，也不改写用户 4G 开关。接收侧应在任务中投递完整 URC；command callback 等待期间不能同步等待另一个调用 provider 的 URC worker，否则会形成锁循环。ISR 只负责缓存/唤醒。

Consumer 集成需要同时完成：

- 转发新增 PAL power 操作和 capability，提供实际 EC25 profile、DTR/RI/transport callback；主控深睡时由私有 board 保留 Modem 电源域和唤醒线路。
- PPP adapter 在 SIM invalidation 时撤销旧 IP/DNS、取消旧 generation 回调并异步清理 netif；所有拨号和停止经过 provider 的 PPP 入口，不能绕过活动保持。
- H106 根据 Wi-Fi 优先和用户期望设置策略，拔卡不复用“用户关闭 4G”的持久化动作；重新插卡按产品期望恢复。
- 实机核验空闲电流、来电 RI/URC、主控深睡唤醒、DTR 时序、CMUX/PPP、GNSS 与反复插拔。Host mock 和并发测试不能替代这些硬件验收。


## ES8311 音量映射

`audio/es8311` 拥有与 SDK 无关的 ES8311 DAC 音量控制点校验和整数插值。
它只认识 codec 寄存器增益范围，不拥有 I2C、静音寄存器写入或系统生命周期。
BSP 选择最大增益和曲线，ESP-IDF audio system 负责硬件应用与错误传递；
固件入口通过 `firmware_lib_component` 链接该 library。
配置、兼容性、静音语义与接入示例见
[ES8311 板级音量映射](./components/esp_idf6_x#es8311-板级音量映射)。

语音呼叫支持可选 DSCI 状态通知，prepare 每轮 best-effort 启用，不支持时保留传统 URC 回退；来电 ID、结束事件去重及迟到通知规则见 [Modem URC 接收与并发合同](./modem_urc.md#语音呼叫状态通知)。
