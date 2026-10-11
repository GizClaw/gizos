# AMOLED GizClaw Session E2E

`gizclaw-e2e/amoled` 运行与 Desktop 相同的 portable GizClaw E2E App。普通 Voice 用例从创建连接开始使用 `libs/gizclaw` Session 注册、加载完整 `assistants` catalog、选择服务器提供的 Workflow、准备临时 Workspace 并管理 Conversation 输入及终态。测试只保存选择和清理账本，不维护另一份可用于业务决策的就绪状态。RPC 和 Group 等底层接口测试使用独立 fixture，不混用同一活动 Session 的状态修改接口。

## 构建

Portable E2E 的并发 run guard 由每对象普通 static backing 提供，launcher 无需额外的模块级 init/shutdown；retained session 仍保持 guard 占用，阻止同一 image 再运行一轮。

```sh
bazel build --config=esp32s3 \
  --define=H2_GIZCLAW_E2E_VOICE_ONLY=1 \
  --//tools/bazel:firmware_version=0.1.0-session-e2e \
  //projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/amoled:package
```

未指定 `VOICE_ONLY` 时仍运行完整 `all` suite，其中 Voice 使用同一 Session 路径。Launcher 通过 `gizclaw_e2e_fixture` macro 显式注入E2E endpoint/RegistrationToken、期望RuntimeProfile、AppConfig key/value、NTP与Device API/tone输入URL；没有源码凭据或key默认值，缺输入在Runtime网络初始化和业务变更前失败，不确认该image。设备只借用已保存的Wi-Fi；不把 Wi-Fi 凭据编入固件。Voice 使用确定性的 16 kHz mono PCM，经真实网络上传，并在库的 PCM Track 上核验下行非静音音频；启用physical模式时Testing Audio wrapper同时drain真实麦克风，但上传的仍是fixture PCM；该用例不验收麦克风音质或扬声器听感。

## Lua App 大包与 golden

`--define=H2_GIZCLAW_E2E_LUA_ONLY=1` 选择独立 `lua-apps` suite。调用方通过 `app_config_fixture` 的 string flags 提供 endpoint、RegistrationToken、期望 Profile、time_server、HTTPS device_api_url，以及 `lua_app_url`（仅在控制端本地解码的 Base64 archive fixture）、`lua_app_http_url`、`lua_app_https_url`、`lua_app_id`、`lua_archive_sha256`、`lua_entry_sha256`、`lua_frame_sha256`。两个 HTTP(S) 下载 URL 与本地 archive fixture 必须表示同一个 caller-owned 包，压缩大小必须大于 65535 字节且不超过 Binary 的 512 KiB 限制。私有游戏和本机地址只作为外部 build inputs，不复制进公共源码。可将这些输入放进工作树外的 Bazel rc 文件：

```sh
bazel --bazelrc=/absolute/path/lua-fixture.bazelrc build --noannounce_rc --config=esp32s3 \
  --define=H2_GIZCLAW_E2E_LUA_ONLY=1 \
  --//tools/bazel:firmware_version=0.1.0-lua-e2e \
  //projects/e2e/targets/h2loader_tar_zlib/gizclaw-e2e/amoled:package
```

自签 HTTPS fixture 通过 `app_config_fixture_root_ca_hex` 注入 hex PEM；portable fixture 创建自己的 coreHTTP provider，保留 required TLS verification，退出时在所有请求和 Service join 后销毁它。Public control API 使用 Edge 入口，Server 私有 HTTP 入口不承载这些 API。Directed 本地测试的 Server/Edge ICE listen 应绑定实际 LAN 地址，避免把主机的其他接口 candidate 集合引入设备验收。

先用 `//projects/e2e/targets/cc_test/gizclaw:gizclaw_h2peer_lua_apps_live_test` 和同一包在 368×448 headless display 上记录参考帧。只有显式 `H2_GIZCLAW_E2E_LUA_RECORD_GOLDEN=1` 的 host run 可以 record；随后把摘要固定到 fixture，再以默认严格模式运行。AMOLED 始终比较独立预期值。 Host 同时用设备的 ledger contract 校验每条 Lua 证据记录，统一使用 `H2_GIZCLAW_E2E stage=lua-golden` 前缀和单行格式，避免 host PASS 却无法冻结设备账本。首帧摘要覆盖 renderer 的完整 RGB565 framebuffer，成功-present callback 发生在真实 Display PAL 完成后；这不替代面板的光学观察。

用例创建一个预装 Lua fixture，验证 listing 与 params/KV；随后经过真实 Server/Edge 用多帧 Binary、HTTP、HTTPS 安装大包，验证错误压缩摘要被拒绝且旧 entry 保留、混合 listing、同 ID 去重、已写 entry 的完整 SHA-256，再经远程 run 启动实际 Lua Host，要求至少三个成功呈现的帧及首帧 golden 一致。软件单测另覆盖错误摘要、长度、TAR/zlib 结束、路径、取消、配额与原子发布失败。该 lane 只验收应用安装与启动，不宣称完成游戏全过程或旧八项语音业务资格。

设备保留 `/data/gizclaw-e2e-lua` 下的安装目录和独立 KV，便于后续独立启动验证持久化；仅识别该 fixture 自己写入的 `.owner` 标记，不接管未知目录。结束时停止/join Lua 与安装 worker、销毁目录 handle、撤销临时 API key 并确认 Peer 删除，要求 `suite=lua-apps selected=1 terminal=1 pass=1 cleanup_rc=0 retained_resources=0 complete=true exit_code=0`。这里的 retained resources 指仍存活的句柄/任务与远端清理义务，预期保留的已安装文件不是泄漏。

持久化验收在后续独立 boot 的构建中同时指定 `--define=H2_GIZCLAW_E2E_LUA_EXPECT_PERSISTED=1`。用例在任何 Binary/URL 安装之前要求旧目录含目标 app、完整 entry SHA 匹配，并通过远程 run 直接运行该 entry、核对 golden；旧文件或整个 index 丢失均失败，不能按冷启动重新安装后补判 PASS。默认冷启动模式只允许首次不存在目标 app，已经存在的 entry 也会先校验并运行。诊断镜像保持未确认，正常 App reboot 可返回 Loader；独立设备 boot 可以由 H2Loader 安装后续诊断镜像进入，保留原始执行 nonce 和 package/image identity。

Host Lua live test 在同一私有 Filesystem 上运行两次，第二次重建目录与 Service 并启用相同的持久化要求。显式 `H2_GIZCLAW_E2E_LUA_PERSISTENCE_FAULT=missing` 或 `digest` 在两次运行之间删除 entry 或写入等长损坏内容；harness 只有看到持久化检查的预期失败、且清理成功时才将这个负例记为通过。该模式只修改当前 host session 自己的临时文件。

所有子集保持诊断身份，只有既有完整 `all` suite 可以调用 H2Loader confirm；Lua 通过不能确认镜像。保留本轮版本、包/镜像 SHA、真实 execution nonce、原始串口记录和 coredump readback，再通过 H2Loader 返回 Loader。禁止用较早启动的 replay 补齐失败轮次。

## Session 验收

确认未注册时阻塞，注册后自动得到一致的 Profile/catalog，刷新及 catalog 副本一致，Workspace 经服务端确认后 `can_start=true`。对话创建后禁止再次启动准备；输入开始和结束分别验证 `conversation_input_open`，PTT 完成及 Realtime 挂断验证对应终态。释放 route 后重新允许准备；不同输入模式先释放空闲 route，再通过 Session 重新准备参数和绑定对话。重连销毁原 Session，并为新 Service 创建和注册新 Session。

保留既有 Voice 的完整文本/非静音回复、一次 PTT、一次取消、两轮 Realtime、历史播放、Track 替换及重连历史读取验证。Case 完成后释放回调和 Track，关闭并销毁 Session，再将 Service 交给原有精确资源清理账本；未确认删除不能计为回收。Session 的准备取消仍由本地并发测试验证，当前硬件 Voice 流程不声称覆盖在途准备取消。

## 设备运行

只对已明确移交的独占端口使用H2Loader `status`，核验 `board=amoled`、`target=esp32s3`、UID、P1/Stage与实际coredump baseline；并行agent运行期间不使用scan。未知有效Stage不能覆盖；同任务已记录的失败pending App可以由下一版managed package替换，保留P1/Settings与失败记录。使用 `send --file <package>` 和 `reboot upgrade --monitor` 安装；CLI确认前短重连窗口可能结束，随后普通monitor接同一运行的周期重播，不提前确认。不要擦除Wi-Fi或coredump。

普通资格先运行真实业务和cleanup，join后启动命令服务并确认；可选observer在384KiB有界账本内保留完整原始227接口记录，冻结后按真实boot crypto execution nonce重播。溢出/format错误禁止admission；host只接受一份完整、byte/record/CRC32一致、当前version、实际confirm=0的admitted账本，另保存SHA-256。独立normal App boot必须拒绝上次nonce，不能用旧重播代替新测试。音频输入必须来自明确的HTTPS对象；本次可使用该受控E2E profile的已部署非敏感media资产，保留对象SHA/大小/版本身份，业务仍使用真实E2E server。

要求安装后 status 的 APP/Partition 2 identity 与本次 package 一致、Stage 清空、`last_result=0`；日志包含 Session 逐操作业务断言、PTT 和 Realtime 结果、唯一 case terminal 及重复 final summary。只有完整 `selected=8 terminal=8 pass=8 cleanup_rc=0 retained_resources=0 complete=true exit_code=0` 才允许确认 App，并仍须保存当前版本227项审计及coredump记录。任何子集（包括 Voice）只提供诊断结果，不能确认镜像或代替全量资格。

独立正常启动可使用 `reboot app`，让资格测试独立运行，完成后再通过 directed `monitor` 读取当前版本的冻结账本。每次必须排除上一轮 execution nonce；持续串口观察期间出现的超时保留为失败记录，不能拿较早的 PASS 代替。该流程减少观察者对运行的扰动，不改变任何业务超时、音频节奏或资源清理断言。

AMOLED 的 H2Loader layout 保留 Wi-Fi/lwIP PSRAM 配置；TCP收发窗口与接收邮箱按Tiga/Zero ESP配置设为11520字节和12项，TCP/IP任务优先级22且固定CPU0。TCP乱序队列上限和selective ACK使用ESP-IDF默认值。ES8311 输出任务现按Tiga/Zero speaker配置设为优先级17、固定CPU1；麦克风配置保持原值。连接丢失后的远端 Peer 清理只在原 cleanup deadline 内重连同一身份一次，仍以服务端删除确认作为回收证据。真实受控 Dev 或 E2E cluster 均可通过相同显式 macro 输入选择；receipt 必须保留实际 cluster、SDK、AppConfig、源码和产物身份。此前 R45 资格对应显式乱序上限4和 selective ACK 开启的原始镜像，不能作为撤回这两项配置后的设备验收。

撤回两项 TCP 覆盖值的实板对照见 [AMOLED TCP defaults comparison](../../../../../projects/e2e/apps/gizclaw/evidence/amoled-tcp-defaults-comparison.json)。同一恢复后的 Dev 0.24.1 环境中，R47 实际生成配置为乱序上限0、selective ACK关闭、speaker优先级6，完整业务结果为6/8，cleanup/retained均为0；Device API在注册阶段返回CLOSED，尚未运行播放器，Firmware下载在300秒deadline内只收到228056/5337909字节。主机同一Firmware用例2.6秒通过。保留原源码和包身份的R45对照（乱序上限4、selective ACK开启、speaker优先级6）实际8/8及227/227通过、confirm=0；最终App分区2/2、Stage为空、原P1和空crash baseline保留。这是一轮每配置的功能对照，未单独隔离两项TCP设置，撤回配置不具备R45的全量资格。

此前小接收配置的实板资格见 [AMOLED small RX qualification](../../../../../projects/e2e/apps/gizclaw/evidence/amoled-small-rx-qualification.json)：R49在相同Dev 0.24.1及SDK0.23.2下，managed和独立normal启动各8/8及227/227通过，cleanup/retained均为0、confirm=0且nonce不同。当时发送缓冲仍为65535、TCP/IP任务使用默认优先级18、speaker优先级6且不固定核。两轮Firmware完整5337909字节、SHA匹配，Device API播放器流程通过；实际App分区2/2、Stage为空、原P1与空crash基线保留。此前相同小接收配置R48首轮为7/8，Resource注册返回CLOSED，未确认镜像，原失败账本保留。这些结果支持当时接收配置的功能就绪，不证明长队列本身制造网络乱序。

R50进一步将发送缓冲11520和TCP/IP优先级22与Tiga/Zero一致，speaker仍为6且不固定核；全量结果7/8，Device API连续播放失败。播放到20096ms用了25750ms，150秒探针结束时仅到130048ms、未观测到EOS，用户同时报告声音断续和播放停顿；cleanup/retained均为0，未确认镜像。

R51只将speaker改为17/CPU1，保持上述TCP配置；前20秒播放计数在每秒采样间前进960–1024ms，但并发`/device/status`查询返回TIMEOUT、HTTP状态0，全量7/8，cleanup/retained均为0，未确认镜像。R52再将本E2E launcher的`$gizclaw/net`、`$h2peer/net`和`$h2peer/udp`分别设为20、20、21，均固定CPU0，与Tiga/Zero的产品网络任务相同；managed与独立normal启动各8/8及227/227通过、confirm=0、cleanup/retained均为0，实际nonce不同。两轮分别播放到20128ms/20160ms，用时20008ms/20003ms；完整EOS位置均138656ms、耗时141019ms/140928ms。最终App分区2/2、Stage为空、原P1和空crash基线保留。主机同一Device API suite在16秒内1/1通过、5次设备状态查询均200；主机使用software音频delegate，只提供控制链路对照。各轮完整身份与诊断见 [AMOLED playback comparison](../../../../../projects/e2e/apps/gizclaw/evidence/amoled-tiga-tcp-playback-comparison.json)。用户另确认R52这一版听起来连续；功能审计与该听感观察分别记录，不能据这轮同时改变的调度参数认定单项因果。

同步main的ESP证书校验修复后，R53以source503cb56c保留SDK bundle verifier委托与日期检查并重新实板验收；后续6454d1e的5个资格元数据文件与Bazel实际5166个固件输入交集为0。managed/normal分别以独立nonce完成8/8及227/227、cleanup/retained均为0、confirm=0；20秒探针分别为20032ms/19757ms及20128ms/20002ms，完整EOS位置均138656ms、耗时140775ms/138672ms。最终App/P2来源与包/镜像匹配、分区2/2、Stage为空、原P1及空crash基线保留。见[post-main TLS qualification](../../../../../projects/e2e/apps/gizclaw/evidence/amoled-main-tls-qualification.json)。R52的用户听感确认仍归属于R52，调度参数保持相同。

Host verifier将任意`H2_GIZCLAW_BOOT`早期标记视为新启动边界，包括只有`platform=amoled reset_reason=...`、尚未分配execution nonce的标记；边界后清除旧admission结果，并永久拒绝此前冻结执行的重播。新启动在READY前失败、只输出entry失败或只留下不完整新账本时均不能借用旧PASS。R50–R53日期采用Asia/Singapore（UTC+08:00），2026-10-04对应UTC2026-10-03；public receipt逐轮保存UTC和本地的冻结账本观察时间。

## 固件下载诊断

`--define=H2_GIZCLAW_E2E_FIRMWARE_ONLY=1` 单独执行真实固件元数据和完整下载用例，仍受相同长度、SHA、EOS和300秒deadline约束；它不会确认 App。可同时指定 `--define=H2_GIZCLAW_E2E_NET_IO_TRACE=1`，仅对当前测试包编译有界TLS读取统计，每个socket最多每5秒记录调用数、字节、超时和I/O耗时。统计不含TLS内容、URI或凭据，不改变读写结果或证书验证。完整资格包应保留默认八项用例。

## Resource suite

将上述构建参数换为 `--define=H2_GIZCLAW_E2E_RESOURCE_ONLY=1` 可单独验收新增 Resource state。该 suite 不运行音频，测试联系人、Profile 和分组状态；使用新 Peer，并在 Resource 关闭销毁后完成远端资源清理。最终日志必须为 `suite=resource selected=1 terminal=1 pass=1 cleanup_rc=0 retained_resources=0 complete=true exit_code=0`。本地替身测试不能代替这一 live 结果；空联系人或空分组列表也不代表多页加载已验收。

本轮GizClaw业务套件默认使用software音频delegate；显式 `--//projects/e2e/apps/gizclaw:app_config_fixture_physical_audio=true` 才启用本文真实capture/speaker链。两者仍使用同一真实E2E服务及完整SDK业务断言，receipt必须记录选择。software模式不宣称物理Audio资格。
