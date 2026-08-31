# App Test

`libs/app_test` 用同一份 C scenario 验证 App 的生产接线：

```text
scenario operation
→ App Test driver
→ Runtime test control
→ Runtime queue / component state
→ App production loop step
→ App state
→ production LVGL subject
→ paired snapshot
```

这里测试的是 Runtime 已识别的 public event/state 到 App state 和 LVGL subject
的映射。PAL 如何把物理按键识别为 Runtime event 属于 Runtime/PAL 单元测试和
真机测试；widget 像素、真实网络、真实音频和物理外设也不由 Host 结果代替。

Public Header 是最终 API source of truth：

- [`h2_app_test.h`](/references/app_test)
- `libs/app_test/include/h2_app_test_case.h`
- `libs/app_test/include/h2_app_test_memory.h`
- `libs/runtime/include/h2_runtime_test.h`

内部 ownership、容量、barrier 和 cleanup 见
[App Test 实现](./app_test/implementation.md)。

## Public API

### App adapter

每个 App 只实现 lifecycle 和 observation，不实现 test input：

```c
typedef struct h2_app_test_app_vtable {
  h2_pal_result_t (*reset)(
      void *user,
      const h2_app_test_fixture_t *fixture);
  h2_runtime_t *(*runtime)(void *user);
  h2_pal_result_t (*run_step)(
      void *user,
      uint32_t timeout_ms);
  h2_pal_result_t (*snapshot)(
      void *user,
      h2_app_test_snapshot_writer_t *writer);
  void (*stop)(void *user);
} h2_app_test_app_vtable_t;
```

- `reset()` 只配置 Runtime、PAL、preference、provider fake 和 production
  initialization 输入；不能写入某一步预期的最终 App state。
- `runtime()` 返回 session 当前借用的真实 `h2_runtime_t *`。
- `run_step()` 调用固件也使用的 production loop step，不接收 event、state 或
  test-only bytes。
- `snapshot()` 分别读取 authoritative App state 和 production subject。
- `stop()` 按 production ownership 逆序释放资源，并且可重复调用。

Public API 没有 `h2_app_test_runtime_input_t`、App input callback 或
`runtime_input_set_state()`。Scenario input 由 driver 交给 Runtime，不会传给
App adapter。

### Fixture

Fixture 是有版本的环境配置：

```c
typedef struct h2_app_test_fixture {
  uint32_t schema;
  size_t size;
  uint8_t data[H2_APP_TEST_FIXTURE_MAX];
} h2_app_test_fixture_t;

h2_pal_result_t h2_app_test_fixture_init(
    h2_app_test_fixture_t *fixture,
    uint32_t schema,
    const void *data,
    size_t size);
```

App 自己定义 payload，但 payload 只能描述输入源。例如：

```c
#define EXAMPLE_FIXTURE_V1 1u

typedef struct example_fixture_v1 {
  h2_pal_battery_reading_t battery;
  h2_pal_wifi_sta_status_t wifi_status;
  h2_pal_result_t preference_commit_result;
  bool saved_wifi_present;
  bool provider_available;
} example_fixture_v1_t;
```

以下字段不应出现在 integration fixture：

```c
/* 错误：这些是 scenario 应通过 operation 到达的结果。 */
bool detail_page_open;
uint32_t selected_index;
bool request_pending;
unsigned int request_generation;
int32_t expected_subject_value;
```

需要直接构造 reducer state 的测试可以保留为普通 unit test，但不能登记为 App
Test Host integration evidence。

### Runtime test control

Memory driver 使用 Runtime 的 test-support API：

```c
h2_pal_result_t h2_runtime_test_control_open(
    h2_runtime_t *runtime,
    h2_runtime_test_control_t **out_control);

h2_pal_result_t h2_runtime_test_emit_event(
    h2_runtime_test_control_t *control,
    h2_runtime_event_kind_t kind,
    h2_runtime_component_t component,
    h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t timestamp_ms,
    const void *payload,
    size_t payload_size);

h2_pal_result_t h2_runtime_test_set_component_state(
    h2_runtime_test_control_t *control,
    h2_runtime_component_id_t component_id,
    const void *state,
    size_t state_size);
```

Runtime 校验 public schema、分配 sequence，并使用正式的 bounded event queue。
`emit_event()` 只注入 event，不会从 event 猜测当前 component state；需要模拟
完整按键输入时使用 Button helper，同时更新 Runtime button state 和发出对应
event，避免构造互相矛盾的状态：

```c
h2_runtime_test_button_down(control, button_id, 100u);
h2_runtime_test_button_up(control, button_id, 100u, 140u);
h2_runtime_test_button_action(control, button_id, 200u, 240u, 1u);
```

打开 control 时，Runtime 会在 private writer boundary 暂停 input task，清空
当前 production event queue 和 physical input source cache，并切换为独占 test
input writer；control active 时 input task 不读取物理 source，state injection
通过同一套只读 snapshot publication 提供给 App。关闭后下一次 input tick 会
重新发现物理 source。

普通 production App 不包含或持有 `h2_runtime_test_control_t`。

## App 接入

下面代码只保留主要函数和接线。`example_production_init()`、
`example_loop_step()` 和 `example_production_deinit()` 必须同时被正式 App
入口使用，不能复制一份 test reducer。

### Adapter state

```c
typedef struct example_app_test_adapter {
  example_state_t state;
  example_test_environment_t environment;
  h2_runtime_t *runtime;
  bool subjects_ready;
  bool active;
} example_app_test_adapter_t;
```

`example_test_environment_t` 持有 deterministic PAL/provider/preference fake。
它可以保存“下次 provider 调用返回什么”，但不保存最终 route、focus 或 subject
值。

### reset

```c
static h2_pal_result_t reset(
    void *user,
    const h2_app_test_fixture_t *fixture) {
  example_app_test_adapter_t *adapter = user;
  example_fixture_v1_t inputs;

  if (adapter == NULL || fixture == NULL ||
      fixture->schema != EXAMPLE_FIXTURE_V1 ||
      fixture->size != sizeof(inputs)) {
    return H2_PAL_ERR_FORMAT;
  }
  memcpy(&inputs, fixture->data, sizeof(inputs));

  h2_pal_result_t rc =
      example_test_environment_init(&adapter->environment, &inputs);
  if (rc != H2_PAL_OK) {
    return rc;
  }

  rc = example_test_runtime_create(
      &adapter->environment,
      &adapter->runtime);
  if (rc != H2_PAL_OK) {
    return rc;
  }

  rc = example_production_init(
      &adapter->state,
      adapter->runtime,
      example_test_environment_config(&adapter->environment));
  if (rc != H2_PAL_OK) {
    return rc;
  }

  rc = example_subjects_init(&adapter->state);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  adapter->subjects_ready = true;
  adapter->active = true;
  return H2_PAL_OK;
}
```

注意：`reset()` 没有 `adapter->state.route = fixture->route` 之类的赋值。
Preference 或 provider 数据必须由 production initialization 读取。

### runtime 和 run_step

```c
static h2_runtime_t *runtime(void *user) {
  example_app_test_adapter_t *adapter = user;
  return adapter != NULL && adapter->active
             ? adapter->runtime
             : NULL;
}

static h2_pal_result_t run_step(
    void *user,
    uint32_t timeout_ms) {
  example_app_test_adapter_t *adapter = user;
  if (adapter == NULL || !adapter->active) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return example_loop_step(&adapter->state, timeout_ms);
}
```

`run_step()` 不得调用 handler、domain `apply_result()` 或 subject publication
来代替 production loop。

### snapshot

同一个可见字段写一对 probe：

```c
static h2_pal_result_t snapshot(
    void *user,
    h2_app_test_snapshot_writer_t *writer) {
  example_app_test_adapter_t *adapter = user;
  h2_pal_result_t rc = h2_app_test_snapshot_write_i32(
      writer,
      "app.navigation.route",
      adapter->state.route);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  return h2_app_test_snapshot_write_i32(
      writer,
      "ui.navigation.route",
      lv_subject_get_int(&adapter->state.subjects.route));
}
```

`ui.*` 必须读取 production `lv_subject_t`，不能从 App state 重新计算。

### stop 和 vtable

```c
static void stop(void *user) {
  example_app_test_adapter_t *adapter = user;
  if (adapter == NULL) {
    return;
  }
  if (adapter->subjects_ready) {
    example_subjects_deinit(&adapter->state);
    adapter->subjects_ready = false;
  }
  example_production_deinit(&adapter->state);
  example_test_runtime_destroy(adapter->runtime);
  adapter->runtime = NULL;
  example_test_environment_deinit(&adapter->environment);
  adapter->active = false;
}

static const h2_app_test_app_vtable_t s_app_test_vtable = {
    .reset = reset,
    .runtime = runtime,
    .run_step = run_step,
    .snapshot = snapshot,
    .stop = stop,
};

h2_app_test_app_t example_app_test_app(
    example_app_test_adapter_t *adapter) {
  return (h2_app_test_app_t){
      .app_id = "example.main",
      .user = adapter,
      .vtable = &s_app_test_vtable,
  };
}
```

## 写 Scenario

Case 是 C 代码。一个 case 可以有任意长度的 input sequence，不是“一次测试只能
给一个 input”。

```c
H2_APP_TEST_CASE(saved_wifi_connect_and_delete) {
  example_fixture_v1_t inputs = {
      .saved_wifi_present = true,
      .preference_commit_result = H2_PAL_OK,
  };

  H2_APP_TEST_OPEN(
      test,
      "example.main",
      EXAMPLE_FIXTURE_V1,
      &inputs);

  /* Runtime action → production list handler → detail subject. */
  H2_APP_TEST_BUTTON_ACTION(test, EXAMPLE_BUTTON_OK, 100u, 140u);
  H2_APP_TEST_EXPECT_I32(
      test, "app.wifi.view", EXAMPLE_WIFI_DETAIL);
  H2_APP_TEST_EXPECT_I32(
      test, "ui.wifi.view", EXAMPLE_WIFI_DETAIL);

  /* Detail 的 Right 执行连接，不是 list 直接删除。 */
  H2_APP_TEST_BUTTON_ACTION(test, EXAMPLE_BUTTON_RIGHT, 200u, 240u);
  H2_APP_TEST_EXPECT_BOOL(test, "app.wifi.connected", true);
  H2_APP_TEST_EXPECT_BOOL(test, "ui.wifi.connected", true);

  H2_APP_TEST_BUTTON_ACTION(test, EXAMPLE_BUTTON_LEFT, 300u, 340u);
  H2_APP_TEST_EXPECT_I32(
      test, "app.wifi.view", EXAMPLE_WIFI_DELETE_CONFIRM);
  H2_APP_TEST_EXPECT_I32(
      test, "ui.wifi.view", EXAMPLE_WIFI_DELETE_CONFIRM);

  H2_APP_TEST_BUTTON_ACTION(test, EXAMPLE_BUTTON_OK, 400u, 440u);
  H2_APP_TEST_EXPECT_BOOL(test, "app.wifi.saved_present", false);
  H2_APP_TEST_EXPECT_BOOL(test, "ui.wifi.saved_present", false);
}
```

`H2_APP_TEST_BUTTON_ACTION` 注入 `click_count == 1`、phase 为 `RELEASED` 的完整 action；需要连续点击
序号（例如 tapdoki 十连击 reset）时使用
`H2_APP_TEST_BUTTON_ACTION_COUNT(test, component_id, pressed, released, count)`，
或直接调用 `h2_app_test_session_button_action()` 传入 `click_count`。

`H2_APP_TEST_BUTTON_DOWN` / `UP` 在 App Test scenario 中投影为 phase 分别为
`PRESSED` / `RELEASED` 的 semantic action，使 scenario 与 App 当前消费的契约一致。
需要直接验证 Runtime raw `BUTTON_DOWN` / `BUTTON_UP` event 的测试应使用 Test
Control event injection。长按等 gesture 仍由 scenario 根据 action phase 和时间表达：

```c
H2_APP_TEST_BUTTON_DOWN(test, EXAMPLE_BUTTON_RECORD, 500u);
H2_APP_TEST_EXPECT_BOOL(test, "app.recording", true);
H2_APP_TEST_EXPECT_BOOL(test, "ui.recording", true);

H2_APP_TEST_BUTTON_UP(test, EXAMPLE_BUTTON_RECORD, 900u);
H2_APP_TEST_EXPECT_BOOL(test, "app.recording", false);
H2_APP_TEST_EXPECT_BOOL(test, "ui.recording", false);
```

### System event

非 button event 使用正式 Runtime payload：

```c
h2_runtime_system_event_wifi_sta_t wifi = {
    .state = H2_RUNTIME_SYSTEM_WIFI_STA_CONNECTED,
};
h2_app_test_event_t event;

H2_APP_TEST_REQUIRE(
    test,
    h2_app_test_event_init(
        &event,
        H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_CONNECTED,
        H2_RUNTIME_COMPONENT_SYSTEM,
        EXAMPLE_WIFI_COMPONENT,
        1000u,
        &wifi,
        sizeof(wifi)));
H2_APP_TEST_STEP_EVENT(test, event, 1000u);
H2_APP_TEST_EXPECT_BOOL(test, "app.network.available", true);
H2_APP_TEST_EXPECT_BOOL(test, "ui.network.available", true);
```

Runtime 会拒绝错误的 kind/component/payload 组合。不要为了让测试通过而传
`NULL` payload。

### Runtime component state

只允许设置 Runtime-owned component state：

```c
h2_runtime_button_state_t power = {
    .pressed = true,
    .pressed_at_ms = 1200u,
};

H2_APP_TEST_SET_COMPONENT_STATE(
    test,
    EXAMPLE_POWER_BUTTON,
    &power);
```

App-owned route、pending、generation 或 domain result 不是 Runtime component
state，不能通过这个 API 写入。

## 异步 PAL 和 Service

App-owned completion 走完整 worker 路径：

```text
fixture 配置 deterministic fake
→ scenario operation 触发 production command
→ production worker 调 PAL/provider
→ production result slot
→ production loop/tick
→ App state
→ production subject
```

错误示例：

```c
/* 错误：adapter 收到 completion blob 后直接调用 domain apply。 */
adapter->staged_result = *result;
example_apply_result(&adapter->state, &adapter->staged_result);
example_subjects_publish(&adapter->state);
```

正确做法是让 fake provider 在 production worker 调用时返回确定结果。当前
operation 的 barrier 必须等 worker result 被 production loop 消费后再 snapshot；
不能依靠固定 sleep。

## Memory Driver

Host 测试入口：

```c
int main(void) {
  example_app_test_adapter_t adapter = {0};
  h2_app_test_driver_t driver = {0};

  assert(h2_app_test_memory_driver_init(
             &driver,
             example_app_test_app(&adapter)) == H2_PAL_OK);
  assert(saved_wifi_connect_and_delete(&driver) == H2_PAL_OK);
  h2_app_test_memory_driver_deinit(&driver);
  return 0;
}
```

Memory driver 初始化 headless LVGL core，不创建 display、window、font、asset 或
widget tree。App 的 subject init/publish/deinit 必须能独立运行。

## 验证

```bash
bazel test //libs/runtime:all
bazel test //libs/app_test:all
```

Adopter 还要运行自己的完整 scenario registry、production unit tests、Guide
build 和 coverage。Coverage 必须排除 adapter、fake、test control 和 test source，
并保留稳定的 production denominator。

H106 把现有 case 编译为一个 aggregate Bazel test binary，并只链接一次
production test core。Aggregate runner 必须隔离执行每个 case，完整传播任一 case
的失败。它是完整 Linux Host automatic test graph 的一个 contributor，不单独拥有
repository coverage denominator。Coverage CI 使用 `--cache_test_results=no` 真实
执行包括该 aggregate 在内的每个 compatible test，不能用 Bazel test cache 替代
coverage side effect。

Host 结果只能报告为 Runtime-to-App-to-subject evidence，不能报告为真机、
rendered pixel、perceived audio、真实网络或产品验收。
