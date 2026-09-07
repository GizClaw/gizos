# App Test 实现

本页记录 `libs/app_test` 和 Runtime test control 的内部 ownership。使用方先读
[App Test](../app_test.md)。

## 目录

```text
libs/runtime/
├── include/h2_runtime_test.h
├── src/h2_runtime_test.c
└── tests/test_runtime.c

libs/app_test/
├── include/
│   ├── h2_app_test.h
│   ├── h2_app_test_case.h
│   └── h2_app_test_memory.h
├── src/
│   ├── core/h2_app_test.c
│   └── memory/h2_app_test_memory.c
└── tests/test_app_test.c
```

Runtime test control 属于 `libs/runtime`，因为 event queue、component state、
sequence、drop 和 wakeup 都由 Runtime 拥有。`libs/app_test` 不能维护 shadow
queue 或复制 Runtime state。

## Ownership

| Owner | 内容 |
| --- | --- |
| Runtime | public event schema、sequence、queue、component state、drop/wakeup |
| App Test core | driver/session generation、operation validation、snapshot storage |
| Memory driver | headless LVGL lifecycle、Runtime control lifetime、operation barrier |
| Testing PAL | PAL 输入、失败注入、独立 evidence 与内部资源；不拥有 Runtime state |
| App adapter | 组装 Testing PAL 和产品 provider/fixture、production init/step/snapshot/stop |
| App | App state、production subjects、workers 和 result slots |
| Scenario | operation sequence 和 expected `app.*` / `ui.*` 值 |

Driver dispatch 不会把 `h2_app_test_operation_t` 传给 App：

```c
static h2_pal_result_t memory_execute(
    void *user,
    const h2_app_test_operation_t *operation,
    uint32_t generation,
    uint32_t timeout_ms,
    h2_app_test_snapshot_t *snapshot) {
  /* 1. operation → Runtime test control */
  h2_pal_result_t rc =
      memory_inject_runtime_operation(user, operation);
  if (rc != H2_PAL_OK) {
    return rc;
  }

  /* 2. App 只运行 production step。 */
  snapshot->step_result =
      user_app(user)->vtable->run_step(
          user_app(user)->user,
          timeout_ms);

  /* 3. barrier 后直接读取 App state 与 subject。 */
  return memory_snapshot(user, generation, snapshot);
}
```

实际实现还处理 RUN operation、错误优先级和 cleanup；上面只展示 ownership。

## Testing PAL

`src/pal/` 实现两个独立于 execution driver 的 target：`testing_audio` 包装 Audio PAL，`testing_pal` 提供确定性 fake。对应 public headers 为 `include/h2_app_test_audio.h`、`include/h2_app_test_audio_fake.h`、`include/h2_app_test_crypto.h`、`include/h2_app_test_display.h`、`include/h2_app_test_fault.h`、`include/h2_app_test_fs.h`、`include/h2_app_test_modem.h`、`include/h2_app_test_periph.h`、`include/h2_app_test_power.h`、`include/h2_app_test_pref.h`、`include/h2_app_test_time.h` 和 `include/h2_app_test_wifi.h`；每种 provider 的实现使用同名 `.c`，fault helper 为 header-only。两个 target 仅依赖 PAL。测试环境在 Runtime 初始化前组装这些 API，App 继续使用真实 Runtime。

### 对象、容量与状态

普通 fake 由调用方持有，init 后地址稳定，配置、调用与 evidence 读取串行；它们可以在同一 libco executor 中使用，不提供 native 多线程同步。Preference、FS 和 Audio fake 借用 Memory PAL 分配内部资源，活动句柄阻止 deinit。Product component mapper、preference key、业务 provider 和 fixture 属于 adapter/scenario；Testing PAL 不复制 Runtime queue 或 component state，也不把请求成功解释为异步完成。

Preference 限制为 8 个 namespace、每个 64 个 key、16 个同时打开的 handle；名称/key 最多 63 字节、值最多 1024 字节。每个 namespace 只允许一个 writer，修改在 commit 前只对 writer 可见；commit 失败保留 staged 值，close 放弃未提交修改。FS 支持 16 个 exact path regular file、每条路径最多 127 字节、默认每文件 1 MiB；每文件只允许一个活动 handle，支持短读，目录操作 unsupported。Periph registry 最多 32 项。Audio fake 和 decorator 各最多 4 条 track，decorator capture scratch 最多 8192 字节。具体容量常量和借用约束由 public headers 定义。

### 时间、barrier 与 evidence

Testing Time 的 sleep 仅推进虚拟时钟，不调度其他 task。生产 Runtime 的 resident worker 使用 libco Time，Testing Time 接入 executor 的 `now_ms`/`time_source`；根循环显式 advance 并 schedule。需要锁的 Runtime 操作、test control 和 cleanup 必须在 executor task 中执行。时钟推进不能替代 production-aware completion barrier；worker result 被 production loop 消费之后，才读取 App snapshot 和 PAL evidence。

Audio decorator 借用 delegate、Time、Memory 和 PCM，mic lifecycle 委托给底层，采集 scratch 读取后清零，再按 monotonic time 返回 fixture PCM；EOF 后输出静音。物理采集错误单独观测，不覆盖 fixture 的业务输入。Evidence 支持并发读取，但多字段不是原子快照。Mic lifecycle/read 串行，speaker lifecycle 串行，每条 track 独立串行，销毁前所有调用者 quiescent。

Audio decorator 默认输出 fixture。后台麦克风泵持续读取的产品应在启动 Runtime 前暂停 fixture，再由公开 observation callback 发布实际 capture 状态；暂停期间继续采样真实麦克风健康，但 read 立即返回 WOULD_BLOCK 和零字节，不消耗 PCM。恢复保留样本位置与 EOF，从首次启用的 read 重新建立 pacing epoch，不补发暂停期间的帧；重复发布同一状态不重置时钟。暂停／恢复即使发生在两次 read 之间或 pacing sleep 内也会被检测。控制状态跨 mic stop/start 与 fixture 更换保留，mic start 仍回绕样本。调用方保持输出暂停后，可在后台 mic 持续运行时更换同格式 fixture；替换与在途帧复制互斥，成功后旧 PCM 可释放，进度与 EOF 清零，真实采集健康保留。替换需与 mic start/stop 串行，不在 observation callback 中执行。该控制只发布原子状态，不调用 PAL 或获取 fixture lock；已越过最终状态检查的在途 read 仍可能输出一帧，因此它不是停止上传的 completion barrier。产品采集状态、素材选择和业务断言由 consumer 拥有。

### 失败和 cleanup

Fault 在有效调用到达对应操作时计数，零初始化默认成功；持续失败和有界失败均可配置。Preference commit filter 仅统计匹配 namespace/key 的调用。Wi-Fi connect 和 Modem call 不自动生成完成事件，Power transition 不重启 Host 或增加 boot count，Crypto fixture 不提供真实密码算法。

Cleanup 先停止 App workers、关闭 Runtime test control 和 mic/speaker/track、销毁 Runtime，再销毁 decorator 和底层 fake。Audio track close、mic/speaker stop 和 FS close 失败时保留 ownership 供重试；不能因为某次失败就丢弃句柄。未成功 init 的动态 fake 可 deinit，重复 deinit 和 NULL deinit 成功；init 不允许覆盖活动对象。普通 fake 无动态资源，不能提前结束其调用方存储生命周期。

Provider 测试覆盖事务可见性、失败重试、短读、容量、错误参数和资源计数；Audio decorator 测试覆盖 pacing、EOF、真实采集故障及输出代理。Runtime/libco 集成测试覆盖 semantic event、component mapping、worker 持久化失败重试、时钟推进与 task 内 cleanup。这些 Host evidence 不替代 H106 产品迁移或真机验收。

## Runtime Test Control

一个 Runtime 同时只能有一个 active control。`open()` 验证 Runtime 已初始化，
并在 Runtime private writer boundary 暂停 input task，清空 production event queue
和 input source cache，再建立 Runtime 独占的 test input writer session。Control
active 时 input task 不读取物理 source。`close()` 发布空 snapshot 并释放 test-only
producer 状态；下一次 input tick 会重新发现物理 source。Runtime deinit 在 control
仍 active 时不释放 Runtime，避免悬空引用。

### Event

`h2_runtime_test_emit_event()` 的顺序是：

1. 校验 control、Runtime 和 public event schema。
2. 复制 payload 到 Runtime-owned bounded storage。
3. 从 Runtime 唯一 sequence source 分配 sequence。
4. 写入 production bounded queue。
5. 使用 production wakeup。

Queue full 使用 production 的 drop-newest 行为：保留旧 event、增加 Runtime
drop counter，并返回 production producer 的结果。Sequence 和已经发布的 state
不回滚。这个 API 只表示 event injection，不根据 event payload 隐式改写
component state；需要模拟同一次物理输入的 state + event 时必须使用对应的
semantic helper。

### Component state

`h2_runtime_test_set_component_state()` 使用 Runtime component registry 的大小和
类型约束。Button semantic helper 先形成一致的 Runtime state，再发出对应 event：

```text
button_down
  state = PRESSED
  event = BUTTON_ACTION(pressed_at_ms, released_at_ms=0)

button_up
  state = released
  event = BUTTON_ACTION(pressed_at_ms, released_at_ms)

button_action
  state = released
  event = BUTTON_ACTION(pressed_at_ms, released_at_ms)
```

`button_action()` 只携带 `pressed_at_ms` 和 `released_at_ms`，App 收到的 payload
与 Runtime 生产路径一致。Scenario 的 `button_down()` 投影释放时间为 0 的 action，
`button_up()` 投影释放时间非零的 action；更细的 held sample 可以直接通过 Test
Control 的 two-timestamp action helper 注入并提供观察时间。长按阈值属于产品策略。每个 helper 失败时不能只改 state
或只入队 event。

## Session State Machine

```text
driver_init
  → session_open
      → App reset
      → App runtime
      → Runtime control open
  → execute operation 1
  → execute operation 2
  → ...
  → session_close
      → Runtime control close
      → App stop
  → driver_deinit
```

Session generation 由 framework 单调递增。Scenario 不传手工 generation；
每个成功 barrier 的 snapshot 记录本次 generation。

错误规则：

- invalid operation 不调用 App。
- Runtime injection 失败不生成成功 snapshot。
- `run_step()` 的结果写入 `snapshot.step_result`；driver API 仍可成功返回 snapshot，
  让 case 断言预期的 production error。
- snapshot 写入失败返回第一个错误。
- 任意失败后 session close 仍安全。

## Snapshot

Snapshot 使用固定容量，不借用 App 字符串：

- probe 最多 `H2_APP_TEST_PROBE_COUNT_MAX` 个；
- name 和 string 分别复制进 aggregate storage；
- probe name 在一个 snapshot 内唯一；
- getter 要求 name 和 type 同时匹配；
- snapshot 完成后不可变。

Pair probe 示例：

```c
h2_app_test_snapshot_write_i32(
    writer,
    "app.menu.focus",
    state->menu_focus);

h2_app_test_snapshot_write_i32(
    writer,
    "ui.menu.focus",
    lv_subject_get_int(&state->subjects.menu_focus));
```

Adapter 不能用同一个计算函数生成两边值，否则无法发现 subject publication
遗漏。

## Headless LVGL

Memory driver 的 LVGL ownership 顺序：

```text
platform init
→ lv_init
→ App reset / subject init
→ scenario operations
→ App stop / subject deinit
→ lv_deinit
→ platform deinit
```

Subject backing storage 必须由 App state 持有，并且比 observer 生命周期长。
Headless profile 不创建 screen、display、font、asset 或 widget。需要验证 widget
文字、焦点样式、像素或动画时，另建 Desktop widget/render profile，不能把它塞进
Memory subject driver。

## Async Barrier

Runtime event 被 `run_step()` drain 并不一定代表 App-owned worker 已完成。
Adopter需要提供 production-aware bounded barrier：

```text
Runtime operation accepted
→ production loop dispatches command
→ deterministic worker consumes provider fake
→ worker publishes production result slot
→ production loop tick consumes current generation
→ no current command/result remains pending
→ snapshot
```

Barrier 可以观察 production command/result slot 的完成条件，但不能：

- sleep 固定毫秒数；
- 直接调用 domain `apply_result()`；
- 修改 pending/generation 来“完成”操作；
- 复制 worker 或 reducer；
- 在 snapshot 前手工 publish subjects。

超时返回 deterministic timeout，并保留可诊断的 App/worker state。

## Partial Cleanup

Memory driver 和 adapter 都按“已经获得的 ownership”释放：

| 已完成阶段 | cleanup |
| --- | --- |
| LVGL platform only | platform deinit |
| App reset 部分成功 | App `stop()` |
| Runtime control open | close control，再 App `stop()` |
| 完整 session | close control、App stop、LVGL deinit |

`stop()`、session close 和 driver deinit 在 public contract 允许的位置必须幂等。
Repeated full-suite run 用来发现 global active adapter、observer、task、queue 或
Runtime state 泄漏。

## Test Matrix

`libs/runtime` 至少覆盖：

- malformed/unknown event schema；
- sequence 单调性；
- queue full/drop；
- component state size/type；
- button state/event consistency；
- duplicate control、close 和 Runtime cleanup。

`libs/app_test` fake App 至少覆盖：

- invalid App/schema/fixture；
- multi-operation sequence；
- event/component/button/RUN dispatch；
- production step result snapshot；
- duplicate/overflow/type-mismatch probe；
- reset/runtime/control/snapshot 各阶段失败；
- repeated session 和 partial cleanup。

Adopter至少覆盖：

- 正式 App loop 与 Memory 调用同一个 loop-step；
- 每个 Host case 的 paired App/subject probe；
- stale、cancel、boundary 和 failure；
- deterministic PAL/provider worker result；
- 完整 registry 重复执行；
- 不把 Host 证据描述成 device/render/audio/network acceptance。


GizClaw E2E 的公共支撑新增 `h2_app_test_mem`、`h2_app_test_task`、`h2_app_test_sync`、`h2_app_test_webrtc`，对应同名 public header 与 `src/pal/` 实现，归属 `testing_pal`。Memory 对每个分配维护对齐头和存活链表；失败的 realloc 保留旧块。Task/Sync 只验证串行生命周期，生产并发继续用真实或 libco PAL。Testing Time 可在单调读之后显式步进，成功 sleep 后可通知借用的场景观察函数；故障 helper 支持跳过指定次数后再注入。

WebRTC decorator 每个实例拥有自己的 peer/channel wrapper，无全局实例。每次成功 poll 返回可释放的包装事件，释放时把原始事件完整交还 delegate；callback 只同步借用事件。每个 peer 最多保留 16 个不同 channel handle，直到 peer close。活动 peer 或未释放 event 阻止 decorator destroy。调用方串行访问同一 peer 及其 channel，释放事件后再关闭 peer；不同 peer 的观察 callback 可并发，需要 consumer 自行同步。

Audio decorator 支持 NULL fixture 的仅播放模式：不暴露 mic，直至安装有效 fixture。成功的 S16LE write 记录原子最大幅值（含 -32768），字节数与 digest 保持原语义。Audio fake 可借用 `playback_time`，成功写入按 PCM 时长向上取整等待；失败不记录已播放字节。GizClaw Device 的 PAL 销毁 hook 在所有 actor 停止后执行，保留 stop/close 失败的所有权。
