# GizClaw BLE 绑定

`libs/gizclaw` 的 BLE Binding service 在调用方授权的绑定窗口中交付与二维码相同的 HTTPS API-key URL。它消费既有 `h2_gizclaw_api_key_state_t`，不创建第二套 key、身份或信任系统。BLE 发现、连接和凭证可能已交付都不是账号绑定成功；手机取得完整 URL 后继续执行现有 HTTP 设备绑定流程。

公共 header `libs/gizclaw/include/h2_gizclaw_ble_binding.h` 是 UUID、线格式、容量、错误和 API lifecycle 的 source of truth，API 文档由 [GizClaw API](/references/gizclaw) 生成。Library 只依赖 GizClaw 的异步 key snapshot 和 BLE、System Event、Sync、Mem PAL，不引入 SDK、board、页面或手机 UI。

## 归属与凭证

产品决定何时打开绑定窗口，例如在受密码保护的扫码页进入后打开、离页或休眠前关闭。产品提供 HTTPS server origin 和可选 icon/name；RPC endpoint 的 transport port 不能当作 HTTPS origin。二维码与 BLE 应使用同一 key snapshot 与公共 URL formatter：URL 为 `https://<host[:port]>/api-keys/<secret>`，可选 query 按 icon、name 顺序加入，UTF-8 name 使用百分号编码。Origin 接受 DNS/IPv4 host 和可选非零 16-bit port；不接受路径、userinfo、query 或 fragment。产品自己的型号、图标选择和页面布局留在 consumer repository。

广播携带通用 vendor service UUID 和调用方可选的公开 `local_name`，不含 key secret、credential URL 或 key name。`local_name` 是最多 29 bytes 的 printable ASCII 产品／设备显示名称，由 library 在 open 时复制，与 URL 中的可选 `name` metadata 分开；legacy provider 把它放入 scan response。GATT 只在授权窗口中交付凭证，采用明文 Read/Write；打开窗口即允许附近客户端读取与二维码相同的凭证，不额外建立 BLE pairing/bonding 身份。调用方必须在结束展示时停止窗口，不能把 service 永久打开。Library 不记录 secret 或 URL，回调的短期 snapshot 和 URL buffer 在返回前擦除。

只要 snapshot 为 invalid、stale、busy 或 closed，就不交付 credential。每次 REQUEST 和 CREDENTIAL 都重新读取 key state，不从上一次 poll 缓存 secret；刷新后的 revision 不同，旧 REQUEST 被拒绝。一个与刷新并发、已通过 snapshot 校验的 read 仍可能返回先前 revision 的字节，这类披露由 exposure ledger 保守记录，调用方按实际 key name 处理，不能根据当前 snapshot 覆盖这条义务。

## 生命周期与共享 Host

调用方先启动共享 BLE Host，再 `open` 创建 idle instance；`start` 订阅低层事件、注册自己的三个 characteristic 和 primary service，并创建独立的 legacy connectable advertising set。每次 owner loop 可以调用 `poll` 重启因断连或正常停止而结束的本服务广播，并用 `snapshot` 读取 live key readiness。RPC completion 仍由同一 owner 的 `service_poll` 驱动，BLE 回调不提交或等待 RPC，也不创建 worker task。

`stop` 先在 mutex 内封住读写 admission，再在锁外停止/销毁本服务的 advertising set、按 service UUID 解绑 callback，最后 unsubscribe。Exposure records 保留在 stopped instance 中。每一个 PAL 操作都针对本服务的资源；它不停止共享 Host、不操作 default advertising、不全局 unregister、不断开可能同时被 H2Loader 使用的物理连接。Host 所有者仍负责最终物理链接和 controller 生命周期。

Provider 必须支持 handle-scoped advertising set 与 `unregister_gatt_service`。不支持或资源不足时返回原来的 `UNSUPPORTED`/`NO_SPACE`，不能覆盖其他 owner 的广播或用 unregister-all 降级。ESP 需要已启用 NimBLE Extended Advertising、足够的 controller instances 和动态 GATT capacity，即使本服务选择 legacy PDU；BK7258 legacy BLE backend 从 advertising-set create 起明确 unsupported，EtherMind backend 才提供该能力。Portable host 测试不证明 board SDK 配置或手机扫描已经可用。

Stop/destroy/unbind 失败时 instance 仍存在且 admission 已关闭，依赖、GATT declaration、callback context 和 handle 存储持续有效；owner 修复 provider 状态后重试 `stop`/`close`。PAL Host stop 完成也会退休 GATT schema 和 advertising handles，BLE Binding 观察对应事件后可完成剩余 unsubscribe。关闭流程不持有 callback 所需 mutex 等待 PAL，因此可处理 provider 在 stop/unbind 内同步派发事件或等待正在执行的 callback。

`start`/`poll`/`stop`/`next_exposure` 由同一个 owner 串行调用；GATT、System Event callback 与 `snapshot` 可以并发。`close` 需要排除其他公共调用，完成 callback detach 后才销毁 mutex 和 instance。Consumer 必须先完成 BLE Binding teardown，再销毁 key state、Service 或 PAL dependencies。

## 手机交互

手机按 service UUID 发现设备并连接，发现 INFO、REQUEST、CREDENTIAL 三个 characteristic。INFO 保持 14 bytes，flags 只含一个 primary status：CLOSED（0x08）优先于 BUSY（0x02）、READY（0x01）、EXHAUSTED（0x04）、FAILED（0x10），零表示 idle not-ready，其余 bits 为零。CLOSED 是 terminal；BUSY 在重试时盖过上次 generation 的终态错误；READY 只在 key valid、nonstale、idle 时出现。EXHAUSTED 同时要求 scoped create result 与真实 RPC presence/code=8，其它终态 key/format/transport 失败为 FAILED；所有 non-ready status 的 URL length 都为零。INFO 是瞬时可见状态；客户端不得把 ready、连接建立或广告可见作为设备绑定完成。INFO 不占用绑定 connection；只有第一个有效 REQUEST 会采纳连接，其他连接的 REQUEST/CREDENTIAL 在它断开或窗口停止前返回 BUSY。

客户端读取 INFO 获得 revision、URL 字节长度和当前最大 payload，随后按 `(revision, offset, payload_limit)` 写入 REQUEST 并读取一块 CREDENTIAL。每块包含相同 revision、对应 offset、完整 URL 长度和 URL bytes。客户端逐项校验并按收到的 payload 长度推进 offset；空 payload 只在 offset 等于总长度时有效。一个 connection 只有一个待处理 REQUEST，相同请求可幂等重写，尚未读取时另一个请求返回 BUSY；读成功即消费请求，传输失败需要重写 REQUEST 后重试。

线格式以 public header 的版本和小端字段为准。REQUEST 为 13 bytes，默认 ATT MTU 23 可以写入；response 上限为 `min(244, ATT_MTU - 2)`，13-byte header 后默认可带 8 bytes。Response 严格小于 ATT Read Response 的 `MTU - 1`，因此手机的自动 long-read 过程会立即结束，不发 EOF Read Blob probe。手机只用 REQUEST 中的 URL offset，不使用 ATT access offset、long/prepared write 或长 attribute read。这兼容 ESP NimBLE 以 offset 0 获取完整 callback value 的行为，也兼容 BK provider 直接传 ATT offset 的行为。

连接事件在有界表中维护 MTU；未知、窗口打开前已存在或超过跟踪容量的连接保守使用 23。MTU 协商后再次读 INFO 可以扩大 chunk；MTU 下降时即使旧请求限额较大，response 仍按新值缩小。Callback output capacity 比当前允许 frame 更小时再缩小 payload；放不下 header 或一个非空 payload 时返回 NO_SPACE，保留 REQUEST 供重试。原生 Read Blob / Write offset 非零会被拒绝。

Refresh、not-ready、断连、重连或 revision 拒绝后，手机丢弃部分 URL 并从 INFO 重新开始。连接断开会清掉未消费 REQUEST；controller 复用相同 handle 的新连接不能读取先前请求。完整 URL 取得后进入与扫码相同的 HTTPS binding flow。该版本与 LiteLink 旧 `x_proto` / topic `0x28` public device token 协议属于不同合同，手机必须接入这组 UUID 和 framing 才能使用。

## 凭证暴露与离页

每个 key name/revision 的第一次非空 credential response 在返回字节前登记 exposure；只交付一个字节、发送随后失败、手机断开或 URL 尚未完整取得，都保守算作可能已暴露。它不表示手机确认或服务端绑定成功。INFO、空 EOF response、请求校验失败及不包含 credential bytes 的失败不会创建 exposure。

Exposure ledger 最多保留 8 个未消费记录。满时，新 generation 的 read 在复制 secret 前返回 NO_SPACE；相同已记录 key name/revision 的后续 chunk 不重复记录，这份 instance 内的去重状态在 stop/start 和 drain 后仍保留。重复打开同一 key revision 的窗口不会耗尽 ledger，新的 revision 仍创建独立记录。Owner 应持续 drain，并在 `stop` 返回后再次 drain，覆盖读与离页并发的最后一条记录。`close` 在记录未 drain 时返回 BUSY，保留 sealed instance，不能静默丢弃义务。

产品合并二维码已经展示的 key name 与 BLE exposure key name，保留任一渠道可能披露的凭证；当前 key 没有被两条路径披露时继续走既有非阻塞 revoke，刷新在途结果继续使用 revoke-after/orphan 规则。Binding service 不替产品决定 refresh 或 revoke，也不改变 API-key state 既有 close/drain/destroy 语义。

## 验证

Cross-language golden vectors 位于 `libs/gizclaw/tests/fixtures/ble_binding_v1.json`，包含 UUID ATT byte order、HTTPS origin、可选 metadata、UTF-8 percent encoding、INFO/REQUEST/首块与空 EOF response，以及资源耗尽、busy retry、恢复 ready 和 closed 的 INFO 状态。LiteLink Dart/微信客户端可以直接读取这份 JSON，不使用不同协议的旧 device-token vectors。

```sh
bazel test --config=macos_arm64 //libs/gizclaw:h2_gizclaw_ble_binding_test
bazel test --config=macos_arm64 //libs/gizclaw:all
make bazel-build BAZEL_CONFIG=macos_arm64
make bazel-test BAZEL_CONFIG=macos_arm64
make guides-build
```

测试使用真实异步 API-key state/Service 和 fake RPC/BLE PAL，逐字节验证 URL/frame，以及 invalid/stale/busy/closed、刷新 revision、默认/协商 MTU、response capacity、手机自动长读终止、连接隔离和 handle 复用、部分 export、ledger backpressure、共存资源、重开窗口和可重试 cleanup。物理 ESP32-S3/BK7258 与手机发现、GATT 交付、并行 H2Loader 和产品页面的最终绑定验证需要对应硬件与 consumer 集成后单独执行；host 测试不替代这些验收。

`projects/e2e/apps/gizclaw/api_coverage.py` 为八个 BLE Binding API 保留独立的 `device-api` 调用和业务断言要求。Fake-PAL host 测试不计入 live E2E evidence，旧 228 项日志仍会缺少这些要求；真实 Server/BLE/手机 lane 完成前，完整审计应继续报告 missing。

## API key 资源限制的投影

产品读取 binding snapshot 的 `info_flags`、`last_error`、`has_rpc_error` 和 `rpc_error_code`，可区分服务端资源耗尽、当前 busy generation 与其它失败；last_error 不把 key 的历史 quota completion 冒充当前 busy 错误。BLE 手机从 INFO flags 获得相同 primary status，不会只看到无限期 ready=false。协议只报告 canonical 8 的资源耗尽，不编码某个 key-count 数字，也不传递任意 server message。

Quota-rejected refresh(false) 保留的旧 key 仍 stale，因此绑定 URL 不交付；此前的真实曝光记录和去重仍有效。错误不触发删除其它 key 或撤销已披露凭证。调用方明确 retry 后，INFO 转 BUSY，再根据新 completion 进入 READY、EXHAUSTED 或 FAILED；成功必须来自新 create 结果，不能把 retained stale key 直接改为 ready。相关 fake RPC/PAL 回归与 golden vectors 不代表真实 Server count-cap 或手机/板端资格化。
