# GizClaw E2E

流式数据在 `h2_gizclaw_req_do(request, user, input_read, output_write)` 绑定。data-up task 按需调用 `input_read`，`WOULD_BLOCK` 留到下一轮，`OK + 0` 表示输入结束；data-down 数据只在调用方执行 `service_poll` 时交给 `output_write`，部分写入和 `WOULD_BLOCK` 都保留剩余字节。协议 RESPONSE/EOS/COMPLETE 不暴露为用户事件。每个方向只允许一个 active request，同方向冲突立即返回 BUSY，上下行互不阻塞。真实 BJ 验收尚待补充，不能将本地线程交接测试当成网络覆盖。

后续本地修复已统一业务 404 和流式 PAL 错误，并将 Telemetry 改为单次提交、WOULD_BLOCK 不重试；下面的网络数据来自这些修复之前，未重新验收。既有清理义务仍保持未确认。

2026-09-03 统一录音接口版本的最新真实结果：macOS / H2Peer / BJ / default，完整六项 E2E 三通过、三失败。ASR/Extract、PTT、AudioPlay 子项通过；Realtime 两轮后的结束阶段超时，另有 RPC/Firmware 失败及五项未确认清理义务。单向测速十二轮通过（上行 8.439–12.788 Mbps，下行 44.151–90.200 Mbps，每轮 1 MiB）；不能据此宣称全量通过。日志在忽略目录 `build/validation/unified-audio-bj-all/`，当前不重复创建远端测试资源。

`gizclaw` 是 `projects/e2e` 持有的 target-independent、headless、阻塞式测试 App。它直接验证 `libs/gizclaw` 的连接、RPC、Firmware 下载、语音流和有界并发能力，不包含 H106 Main、MFG、UI、产品 RuntimeProfile 断言或目标平台的网络连接策略。

Launcher 提供已经初始化的 `h2_runtime_t`、真实 RegistrationToken、服务 endpoint、suite mask 和可选的 16 kHz mono S16LE 测试音频。App 不读取环境变量或文件，不选择 AP/BJ，不创建 Wi-Fi task，也不拥有 Pion/H2Peer provider。RegistrationToken 原样用于注册；注册响应返回的 RuntimeProfile identity 只作为结果 evidence 记录，不与产品常量比较。

一次 `h2_gizclaw_e2e_run()` 调用按固定顺序运行所有选中的独立 case。普通 case 失败不会阻止后续独立 case；取消会把尚未运行的 case 标为 `CANCELLED`。如果 Service 无法完成 stop/deinit，则保留 heap fixture，不再启动后续 case，返回 harness error 并保持全局 run guard。App-owned runner task 执行 case，调用方通过 Runtime Log 和同步 progress observer 接收进度。结果包含 terminal counts、首个失败、cleanup result、retained resource count 和注册返回的 RuntimeProfile。未清理的远端资源只按资源类型写入 redacted recovery ledger，不记录资源名称。每个选中 case 生成唯一 terminal record，runner 确认退出后才 join；已退出 runner 的 PAL handle 释放按 cleanup deadline 有界重试。返回 retained resources 非零时，调用方必须保留 Runtime、provider 和 config/user 的生命周期直至进程退出，不能在同一进程重跑。

Fixture 的每个 actor 直接持有一个 `h2_gizclaw_service_t`，通过新 RPC 注册和清理；不再提供 direct client → Service 的转交入口。配置由 actor 持有直到 Service deinit，删除 Peer 必须收到有效确认，不能把连接关闭当作成功。清理阶段忽略用户停止信号，但仍受独立 cleanup deadline 限制。

Fixture 仅在注册 profile 非空、完整终止且与已有 actor 一致后发布 `registered`，此时记录同一 Service 上 init/start 的可用性断言。stop 后有界排空通知，拒绝超出 poll 批次的返回数量；排空成功才记录 stop 断言，deinit 成功才清除本地 handle 并记录 deinit 断言。任何阶段失败都保留必要的借用数据，不以重试成功覆盖先前失败的 case 终态。`service_coverage_test` 额外运行 11 种 Fixture 生命周期边界场景，正常场景只识别这四个入口、整体仍为 `valid=false`，失败场景不计入覆盖；不替代 Track、内存泄漏分析或真实网络验收。

Workspace 清理分别记录主/隔离 actor 的有效删除确认。case 删除后若列表验证失败，Fixture 保留确认并在重试时 get 检查目标缺失；确认存在且 get 返回 NOT_FOUND 才退还义务。未确认删除的 NOT_FOUND、错误对象或查询失败不能算完成；仍查到目标时允许下一次重试删除。清理预算耗尽后不再发送 Peer delete，保留身份用于重试。两个 actor 的 36 组边界场景覆盖此状态交接；不能据此声称超时创建永远不会迟到。

当前公开 API 共 190 个：包括独立的 Workspace reload、统一的 Service audio_start/audio_end 和库内 PCM Track；已删除 req_finish_input 及 Conversation 的旧 begin/end。Firmware case 已接入新的 req/resp/rpc，完整网络验收仍需逐用例记录，不以编译通过代替。Connectivity 在同一注册连接上用 req/resp 和同步 RPC 各做三轮上传、下载，每轮 1 MiB；输出传输耗时和请求总耗时，建连不计入传输。上传计时截止服务器 EOS 确认，不以本地发送完成代替。独立本地测试使用模拟传输，不能作为真实 Mbps 或业务 E2E 结果。

Connectivity 使用两个隔离 Peer，以便分别验证 req/resp 和同步 RPC 的 peer_delete；注册复验、ping 与全部测速始终使用同一个主 Service，不在测量间重连。全部测速成功后才删除两个 Peer，有效删除响应清除对应义务，任何失败仍交给 Fixture 收尾。`gizclaw_e2e_connectivity_test` 的本地场景覆盖调用阶段失败、错误响应、取消失败、预算耗尽、时钟读取失败、上传读取、下载写入、块顺序/内容和 poll 失败；`connectivity_coverage_test` 检查 12 个业务函数、12 条测量记录及六条数据搬运记录，缺少实际 dispatch 记录不能认证 req/resp 下载测速。正常场景仍为 `valid=false`，不是实际网络速度或远端清理验收。

独立测速可使用 `//projects/e2e/targets/cc_test/gizclaw:gizclaw_h2peer_connectivity_live_test`，同样要求 token 环境变量和显式 endpoint 参数。该目标链接同一个 runner、Fixture 与真实 Connectivity case，但只提供 Connectivity catalog；选择其他 suite 会失败。完整目标仍保留六个 case，不能用专用目标取代全量验收。

2026-09-03 历史代码复测（本轮统一录音控制前）（包含长回复 ID 修复，保留 SCTP RTT / HTNA 及 H2Peer 仅空闲时等待）：真实 macOS / BJ / default，默认 fastbuild（运行 `cff05e12-49b8-4bf6-bd92-c19da25d1226`）上传 7.294–12.336 Mbps、下载 31.775–88.301 Mbps；`-c opt`（`2065a8b4-b73e-458f-b184-0ab068528b29`）上传 6.732–12.633 Mbps、下载 59.494–135.300 Mbps。两次均十二轮全部成功，Connectivity PASS、cleanup_rc=0、retained_resources=0；下载校验循环模式，上传仅确认长度。这不是受控 A/B，每轮仅 1 MiB，不证明完整语音/ASR/全部 RPC 或稳定吞吐。八帧批量发送实验未观察到明确收益，已撤回；剩余发送背压、快速恢复与 T3 仍待排查。历史与逐轮数据保留在本地设计文档和忽略的原始日志中。

Friend 创建响应丢失时，清理按对端公钥分页恢复目标，最多 32 页，每页受清理截止时间限制。只有完整查询确认唯一目标才保存删除 ID；删除响应必须匹配该 ID 和公钥。查询失败或未找到时保留义务与相关 Peer，不以空列表证明此前超时的创建不会迟到。该迟到创建的最终恢复仍待验收。

Group 管理的 12 项业务分别走 req/resp 与同步 RPC，共 36 个函数。两套调用各自创建临时群、修改元信息、创建并读回邀请、加入成员、修改角色、删除成员和群组；写入后读回、删除后验证缺失。每次调用先检查预算；分页最多 32 页、每页 32 项、游标 255 字节。失败保留既有清理目标，成功后保留一个新建的 owner 群供消息及同名隔离使用。431 组边界场景和 `group_coverage_test` 验证错误响应、未落地操作、分页、预算及真实调用日志；正常局部识别 36 项，但完整验收仍为 valid=false。

Group 失败清理使用 fixture.c 内部的成员查找/删除 helper：最多 32 页、每页 64 项，校验目标群组和跨页唯一的成员公钥，使用响应的 membership ID 删除。删除确认必须同时匹配 ID、群名和公钥，失败保留成员义务及父资源。30 组本地场景覆盖分页、异常响应、预算和重试；仍不证明超时 join 不会迟到。

Group 消息用例先向本轮新建群的 system Workspace 上传 PCM（PTT BOS/EOS），保存发送前历史后查找新增且带音频的 GEAR 条目；Chatroom 不要求助手回复，也不强求已开启转写。取消订阅并安全解绑 Track 后才把历史 ID 交给 list/get/download，失败不跳过后续步骤并假装成功。群 Workspace 单独选择，不修改普通 Voice 的 Workspace 或清理目标。当前只有本地替身测试与模块构建通过，真实 BJ 运行仍待完成。

`group_message_case` 对同一新消息分别走 list/get 的 req/resp 与同步 RPC，核对群名、历史 ID、owner 公钥、GEAR 类型和音频标记。列表最多 32 页、每页 32 条、游标 255 字节；响应须来自有效 arena，目标不能缺失或重复。允许无关记录的未知类型及可选文本。113 组本地场景和十四个输入边界覆盖失败、分页上限、异常字段、取消及释放；`group_message_coverage_test` 正常识别六个函数，但整体仍 valid=false，不代表完整 Group 45 函数的真实服务验收。

群消息音频下载对同一条目分别走 req/resp 与同步 RPC，核对非零 sink 字节数、长度和精确身份。计数器由 Fixture 持有，有限 wait 失败后的迟到 sink 不再借用局部栈；两次下载使用独立计数器，禁止再次启动同一 Fixture 的下载 helper。36 组本地场景及输入边界验证两套接口，日志对接只识别三个函数，完整验收仍为 valid=false。此项不验证音频格式/播放，Group 的真实消息生成和完整 45 函数覆盖尚未完成。

Desktop live test 位于 `projects/e2e/targets/cc_test/gizclaw/`。H2Peer 与 Pion 分别由独立的 `manual` `cc_test` 装配 Desktop Runtime/PAL，从 `H2_GIZCLAW_E2E_REGISTRATION_TOKEN` 读取真实 token，加载确定性 PCM，然后调用同一个 portable entry。Token、private key、authorization metadata、Firmware URL、原始音频和响应正文不得进入日志或 artifact。

Desktop 必须显式传入 `--endpoint=<hostname-or-ipv4>:<port>`（端口 1–65535）；不读取默认 endpoint、不映射 `bj` / `ap` 别名。缺失、重复、非法 endpoint 或未知参数均在网络 provider 初始化之前失败，只打印固定错误原因。当前格式不接受 URL、凭证、路径或 IPv6 literal。BJ 验收使用：

```sh
gizclaw_e2e_exit=0
bazel test --config=macos_arm64 \
  //projects/e2e/targets/cc_test/gizclaw:gizclaw_h2peer_live_test \
  --test_arg=--endpoint=edge-bj-01.e2e.gizclaw.com:9821 \
  --test_output=streamed --nocache_test_results --runs_per_test=1 \
  || gizclaw_e2e_exit=$?
```

上述命令要求环境中已配置真实 RegistrationToken；`H2_GIZCLAW_E2E_SUITE=connectivity` 可选择独立测速，不得把它当成 `all` 验收。Bazel 默认 args 提供 PCM fixture 与 suite；endpoint 没有默认值。

Make 包装器通过 `H2_GIZCLAW_E2E_ENDPOINT` 显式生成同一个 `--endpoint` 参数；变量缺失或为空时，不启动 Bazel。示例：

```sh
H2_GIZCLAW_E2E_ENDPOINT=edge-bj-01.e2e.gizclaw.com:9821 \
H2_GIZCLAW_E2E_SUITE=connectivity \
make bazel-test-gizclaw_h2peer_live_test BAZEL_CONFIG=macos_arm64
```

手动 Live E2E workflow 同样要求显式 `endpoint` 输入，不再接收 `entry` 区域别名。H2Peer 支持全部 suite（含 `service`）；Pion 及 `both` 仅接受 `rpc`、`firmware`、`voice`、`firmware-voice`。`both` 会运行两个后端，但任一失败都会使整个步骤失败。`//tools/bazel:gizclaw_live_command_test` 用假 Bazel 检查真实 workflow shell → Make → 包装器的参数和退出状态，不连接任何服务，不能用它证明 live E2E 已通过。

## 逐函数覆盖验收

测速上传与下载分别由 `$gizclaw/data-up`、`$gizclaw/data-down` 任务搬运，不使用测速专用 executor。每个方向只有一个固定 active slot；同方向已有请求时立即返回 BUSY，不排队、不覆盖旧请求，反方向可同时运行。Service 网络任务仍独占 SDK 协议操作，通过 4 KiB 槽和有界 ring 交接；SDK 借用数据先复制，ring 满时阻塞网络生产者，直到调用方 poll 消费、取消或停止唤醒，不把可靠数据静默丢弃。`--stream-data-only` 是本地线程交接/回收测试选择器，不是 BJ 运行参数；本地通过不产生新的 Mbps 结果。真实 BJ 验收及 SDK 网络请求就绪/终态通知迁移尚未完成。

ASR/Extract 同样使用共用流式传输，删除 Speech 专用 executor 和额外的 16 KiB PCM 中转 ring。do 仅登记路由，audio_start 后由 uplink 每 20 ms 交接一帧；发送忙时保留原帧，不覆盖或继续消费。audio_end 冻结尾部，尾部交接完毕后由协议层结束输入；Track 不接收 EOS。`--pcm-stream-only` 选择本地录音边界、背压、提前拒绝及生命周期回归，不是远端 E2E 参数。

AudioPlay 使用固定 audio-down task；Pixa 与 Group Audio 使用 data-down task，并通过 `output_write` 在调用方 poll 上下文逐块交付。AudioPlay 在 do 时登记音频下行 slot 并立即拒绝冲突；收到完整成功响应、验证长度与 EOS 后，只通知一次播放路径，后者投递完 PCM 或出错时发布一次完成结果。接收完成不等于 PCM 已交给 Track，更不等于扬声器已播放完。当前仍先保存完整压缩体再解码。`--download-stream-only` 是本地下载、一次性通知和在途取消/停止回归选择器，不连接 BJ。

Workspace case 分别通过 req/resp 和同步 RPC 覆盖八个业务方法，共 24 个函数。两套接口使用不同临时工作区名称，创建后 get/list 读回，set_input 后确认工作区可用，activate 只提交 SET 并核对选中身份，再显式 reload 核对激活身份与 RUNNING 状态，删除后遍历列表确认缺失。只有确认完成才清除对应义务；失败保留 Fixture 中的精确名称。最后创建原名工作区供后续重连使用，Voice 准备只走正常创建/配置路径。

Workspace 响应校验 arena、数组边界/对齐、字符串与 profile/revision；列表和历史最多 32 页、每页 32 项，游标最多 255 字节。列表检查目标跨页唯一，历史只检查页内 ID 重复，不宣称跨页快照一致性。保留未知历史类型、可选文本和可选 activation workflow 字段。响应没有 input mode，不能把 set_input 的可用性断言当作 PTT/Realtime 行为证明，仍需 Voice E2E。309 组本地边界场景与 `workspace_coverage_test` 验证错误响应、预算、未生效操作和清理标记；这些不是实际服务验收。

测速日志的 `integrity` 区分校验范围：下载成功为 `pattern-verified`（逐字节核对固定上游 v0.13.2 的 0..255 循环模式）；上传成功仅为 `length-ack-only`（服务端 EOS 确认消费及长度，未校验上传内容）；失败为 `not-verified`。模式校验不是密码学摘要，不能据此声称完成上传端到端内容校验。

`api_coverage.py` 的矩阵独立列出约定的 190 个函数，并与 `libs/gizclaw/tests/public_api.inc` 核对。每行指定用例、按序成功调用和显式业务断言；req_create / resp_parse 必须有直接 create → do → wait → parse 的记录，同步 RPC 的内部调用不算另一套 API 的覆盖。Profile / Workflow / Contact 已输出对应业务断言，Point 已输出账户/交易字段、存储归属及有界分页检查；不据 Point 查询宣称账务计算或跨页去重正确。其他尚未补齐的断言仍保留为要求，不降级成“调用返回成功”。Telemetry 是单向包，其 `telemetry_send-assert` 仅按公开 API 契约确认传输层接受，不表示服务端确认或落库；两套 API 使用不同 sequence、各自读取当前时间，测试值明确标为 `e2e-fixture`。

Runner 在 actor 初始化前输出 `coverage-begin`，在清理后输出 `coverage-end`；RPC domain 使用 `rpc/<domain>` 嵌套范围。校验器拒绝缺失、重复、乱序、失败或未关闭的范围，父用例清理失败会使子范围失效。最终只接受指定平台、backend、endpoint 和 profile 的一次 `all` 完整运行，以及全部六个顶层用例和十个 RPC domain。测试进程真实退出码和日志内 summary 都必须成功；不能把 summary 的 exit_code 当成真实进程退出码。

Service case 的四个通用 req 函数和 `service_poll` 已接入调用/断言记录：验证不依赖 poll 的重复 wait、释放用户引用，以及空闲 poll 的分发数量。取消检查使用尚未 do 的请求，验证幂等、CLOSED 终态、错误输出清零及禁止再次启动；不代表网络中途取消已经验收。`service_coverage_test` 分别检查请求、Fixture 生命周期和 Voice Track 三类本地记录，不能拼接这些日志当成完整运行或真实服务器的覆盖证明。

Track 的 set/unset 覆盖归属于 `voice`：同一 Track 必须实际完成 PTT 上行及非静音回复，才证明绑定可用；完成对话后解绑旧 Track、绑定第二个接收 Track，在同一 Service 重播本次生成的历史音频。新 Track 必须收到非静音 PCM，旧 Track 的读写调用次数必须不变，且第二个 Track 也要成功解绑。观察期间保留两个 Track 的状态；解绑失败时仍由 Fixture 持有供清理重试。这一探针验证替换期间的数据路由，不替代在途回调阻塞测试或无限时间的无迟到访问证明。

Voice 同时记录五个 Conversation 入口与 AudioPlay 的业务证据：实际 PCM 上传、完整文本/音频回复、唯一终态、重复 end、取消后读写调用停止、释放及本地清理、历史音频的非静音自动播放。release 断言以前述终态和成功清理为前提，不宣称无限时间内绝无迟到回调。51 种边界场景（另含逐次分配失败）检查错误 generation、重复/错误终态、取消失效、Track 路由和异常响应等；日志对接仅确认八个函数可被识别，整体仍未达到真实全量 E2E 验收要求。

Voice 先校验所读响应字段属于调用方 arena，检查数组边界/对齐、字符串长度/终止符、历史可用性与重复 ID，再使用数据。未知历史类型可存在于列表中，不作为可播放 Agent 回复。隔离 Voice fixture 的历史快照最多保留 64 项；若快照页还有下一页则明确返回 NO_SPACE，不接受部分基线并误认旧音频。这不是完整历史遍历能力的测试。

Friend case 已接入七组业务的 21 个函数：req/resp 和同步 RPC 各自验证邀请 token 的 create/get/clear、好友 add/info/list/delete，以及删除后的列表。info 与对端 Profile 比较；分页最多 32 页，游标最多 255 字节。响应错误或读回失败会保留清理义务，不覆盖既有目标。`friend_coverage_test` 使用本地边界替身验证记录，整体仍不满足真实 E2E 验收。

Pet case 已接入 adopt/get/list/drive/action_get/pixa_download/delete 的 21 个函数，两套 API 使用同一 Service 和已知名称：领养后 get 读回，重复领养核对身份，列表最多遍历 32 页，动作映射支持服务器自定义 ID。Drive 当前验证无行为的状态结算请求及对象读回，不验收具体喂食/洗澡的数值变化。Pixa sink 计数由 Fixture 持有，失败等待返回后不借用局部栈状态；核对实际字节数与下载元信息，不等于验证文件格式或内容哈希。两次删除各核对目标并等待 get 返回 NOT_FOUND，确认缺失后才切换到下一名称，最后保留新建的 owner Pet 供同名隔离测试。

Pet 删除确认代表服务端已排队。case 和 Fixture 最多查询 32 次、间隔 100 ms，每次检查剩余预算；超时仍保留名称和已验证确认，清理重试只查询，不重复删除。无有效确认的 NOT_FOUND 不能当作已清理。251 组业务边界场景、两个 actor 的 38 组清理场景和 `pet_coverage_test` 只证明本地行为及记录接通；完整真实 BJ 验收和超时创建迟到后的最终恢复仍未完成。

在上述测试后、同一个 shell 中执行：

```sh
gizclaw_e2e_log="$(bazel info bazel-testlogs)/projects/e2e/targets/cc_test/gizclaw/gizclaw_h2peer_live_test/test.log"
bazel run --config=macos_arm64 //projects/e2e/apps/gizclaw:api_coverage -- \
  --log="$gizclaw_e2e_log" --process-exit-code="$gizclaw_e2e_exit" \
  --endpoint=edge-bj-01.e2e.gizclaw.com:9821 \
  --backend=h2peer --profile=default --platform=macos
```

输出 JSON 包含全部 190 行、调用/断言的日志行号、日志 SHA-256 和未覆盖项；退出码 0 表示日志满足覆盖要求，1 表示验收未通过，2 表示输入无效。`covered` 是日志中的诊断计数，只有 `valid=true` 且真实 E2E 测试通过才能验收。仍须记录实际构建版本并保留本次原始日志，校验器不能鉴别伪造日志或替身服务器。不得合并多轮日志凑覆盖；多轮运行应逐份核验各自 `run_N_of_M/test.log` 和真实运行结果。

只有完整 live 日志满足全部函数断言才能通过，历史 Connectivity 子集不能通过此审计。`api_coverage_test` 用合成日志验证校验器，逐一删除每个函数的调用/断言并要求失败，也验证进程崩溃、错误 endpoint/profile、跳过用例及清理失败；它不是 BJ E2E 结果。

Desktop 将 Runtime、provider、配置、endpoint、token 和 PCM 放在同一 heap session 中。runner 报告 retained resources 时，session 保留至进程退出，返回 harness error，并禁止该进程再次运行；不在仍有借用者时销毁 provider 或清空 PCM。正常回收后可以再次运行，并恢复原来的 SIGINT/SIGTERM handler。`gizclaw_e2e_desktop_options_test` 直接检查生产参数解析；`gizclaw_e2e_desktop_test` / `gizclaw_e2e_desktop_pion_test` 使用真实 Desktop provider 初始化和 runner 边界替身检查生命周期，不创建 Peer、不连接服务器，不属于 live E2E。

未来 firmware launcher 负责 Wi-Fi credential、重连 task、Runtime event main loop、image/package 和结果传输。`H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_GOT_IP` 第一次出现后，launcher 在独立 runner task 中启动 App；`LOST_IP` 或 `DISCONNECTED` 只更新网络状态，同一次 boot 不启动第二个 runner。
