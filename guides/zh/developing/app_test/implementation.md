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
| App adapter | PAL/provider/preference fixture、production init/step/snapshot/stop |
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
  event = BUTTON_DOWN

button_up
  state = released
  event = BUTTON_UP

button_action
  state = released
  event = BUTTON_ACTION
```

`button_action()` 携带 `pressed_at_ms`、`released_at_ms` 和 `click_count`，
`click_count` 由调用方显式给出（`H2_APP_TEST_BUTTON_ACTION` 固定为 1，
`H2_APP_TEST_BUTTON_ACTION_COUNT` 可给出连续点击序号，0 被拒绝），App 收到的
payload 与 Runtime 生产路径一致；没有 hold helper，长按阈值属于产品策略，
scenario 用 `button_down()` 加时间推进，或者设置 Button state 的
`pressed`/`pressed_at_ms` 表达仍按住的按键。每个 helper 失败时不能只改 state
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
