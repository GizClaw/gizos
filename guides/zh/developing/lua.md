# Lua Runtime

`libs/lua` 提供 Lua 5.5 VM Core 和借用 `h2_runtime_t` 的 Runtime Host。Core
只负责文本 chunk、Lua stack、GC、coroutine 与受限标准库；Host 负责 allocator、
worker、Timer、Filesystem、事件投递、原生 module 和每个 Skill 的隔离生命周期。

## Ownership

```text
App owns Runtime event consumption
        │ explicit copied event + job_id
        ▼
Runtime Host ── one job / one lua_State ── fixed Runtime worker
        │
        ├── Runtime Memory / Time / Timer / Task / Queue / Sync / Filesystem
        ├── private yyjson conversion
        ├── Runtime Display / Touch / Audio System singleton modules
        └── Runtime physical component proxy

VM Core ── upstream Lua 5.5 only; no Runtime or PAL dependency
```

Host 借用 Runtime；调用方必须保证 Runtime 存活到 `stop`、`join`、`destroy` 全部
完成。一个 VM 从发布到销毁只由固定 worker 串行进入；多个 VM 可以分配给多个
Runtime Task。Lua coroutine 是同一 VM 内的协作任务，不分配 PAL Task stack，也
不跨 CPU 并行。Web Task provider 在一个浏览器线程中协作推进，Desktop 和设备
provider 可以让不同 VM 在多个 worker 上并行。

当前 stable build surface 包含 Desktop、Web 和 ESP32-S3/P4。BK3633 与 BK7258
在完成 repository-owned BK build contract 前显式标记为 incompatible，不通过
ESP/newlib portability shim 假装支持。

## Host 和 job

`h2_lua_host_config_t` 的容量均有界：`worker_count`、`worker_stack_size`、
`max_jobs`、`max_coroutines_per_vm`、`ready_queue_capacity`、`waiter_capacity`、
`event_delivery_capacity`、`callback_capacity_per_job`、
`audio_track_capacity_per_job`、`pending_capability_capacity`、`instruction_quantum`、
`resume_time_budget_ms`、`source_limit_bytes`、`output_limit_bytes` 和
`vm_memory_limit_bytes`。零使用声明的默认值；ready/waiter 容量不得小于 VM 的
coroutine 上限。

Host 的正常生命周期是：

1. `h2_lua_host_create()` 借用 Runtime 并分配固定容量；
2. 在 start 前注册 native module 和 capability；
3. `h2_lua_host_start()` 冻结 registry 并创建 worker；
4. 通过 text、compiled resource 或 Runtime Filesystem 提交 job；
5. App 消费 Runtime Event queue，并通过 `h2_lua_dispatch_runtime_event()` 定向
   投递给一个 live `job_id`；
6. `stop()` 拒绝新 job、取消等待，`join()` 等待 worker 退出，最后 `destroy()`。

`h2_lua_host_step()` 只用于提示 worker 有新工作，不会让调用线程进入 VM。
Timer callback 和异步 capability completion 只记录完成并唤醒 worker；真正的 Lua
resume、callback 和资源释放都在 owning worker 或 stop/join 后执行。每次 resume
记录独立开始时间；instruction hook 按 `instruction_quantum` 检查取消和 job 总超时，
并在达到 `resume_time_budget_ms` 后 yield，使 CPU-bound Lua 归还 owning worker。
每个 Runtime event callback 也作为同一 VM 内的短生命周期 scheduler task 执行，
因此可以 yield，并受相同的 quantum、取消和超时约束。

Button `ACTION` 的共享 Runtime payload 只有 `pressed_at_ms` 和 `released_at_ms`。Lua Runtime 在投影到 VM 时自行计算 `duration_ms`，并保持 PR #82 的 Button `gesture_kind` 接口：普通按压为 1、短按为 2、长按为 3，兼容阈值为 500 ms。这个分类只属于 Lua adapter，不进入共享的 `libs/runtime` Button payload；Lua App 无需因底层 Runtime 删除 gesture policy 而更改接口。

## Lua surface

| 模块 | 首期接口 | backing / 边界 |
| --- | --- | --- |
| 标准库 | base、coroutine、table、string、math、utf8、受限 package、`debug.traceback` | 无 `io`、`os.execute`、`load`、`string.dump`、native loader 或 bytecode |
| `args` | 每个 job 的独立参数表 | 提交时复制为 Lua string |
| `print` | 有界单行诊断 | Runtime Log |
| `runtime` | `components.get/on/off`、`spawn/yield/sleep/status/join/cancel`、`event.*` | 数字 component ID；VM coroutine；不暴露 periph ID |
| `json` | `encode`、`decode` | 私有 yyjson provider；深度和输出有界 |
| `capability` | `call(name, payload, options)` | 冻结的 C registry；支持 immediate/pending/cancel/late completion |
| `delay` | `delay_ms`、`delay_us` | ESP-Claw profile；毫秒等待 yield，微秒等待使用 Runtime monotonic time |
| `system` | `time`、`date`、`millis`、`uptime` | Runtime Time；固定 UTC offset |
| `display` | drawing、frame、text、AA circle、framebuffer fade 和 `deinit` | 直接使用 Runtime singleton Display API；dirty region 始终裁剪到 framebuffer |
| `lcd_touch` | `read`、`poll`、`sync` 及 upstream touch result fields | 直接使用 Runtime singleton Touch API，不接收 SDK handle |
| Button proxy | `get_key_level` | Runtime normalized Button snapshot，不创建 GPIO button |
| `audio` | `new_output`（每条 Track 的 `write/info/close`）、`new_input`（`read/level/info/close`） | 直接使用 Runtime singleton Audio System；Track frame 大小取自设备 playback format，Input frame 大小取自设备 mic format；PAL 混合多条 Track，不接收 codec handle |

`runtime.components.getByName()`、`board_manager`、SDK handle 和动态 C module
不属于首期合同。Display、Touch 和 Audio 保持 ESP-Claw 的 module acquisition，内部
直接调用 Runtime singleton PAL API；只有 Button 等物理外设把 constructor 改成
`runtime.components.get(id)`。`audio.new_output()` 每次创建独立 PAL Track；同一 job
可以在 `audio_track_capacity_per_job` 上限内同时写入多条 Track，由 Audio System
负责混音。每个持有 Track 的 job 获取一个 Host 级 speaker user；关闭该 job 的最后
一条 Track 或回收 job 时释放它，只有同一 Host 的最后一个 speaker user 释放后才停止
Speaker。一个 job 结束不能中断另一个仍在写 Track 的 job。`audio.new_input()`
镜像同一套 ref-counted 生命周期：job 获取一个 Host 级 mic user，
`close()`、job cancel/timeout/stop 或 job release 释放它，只有最后一个 mic
user 释放后才停止 Runtime microphone；一个 job 结束不能中断另一个仍在读取的
job。

### Display AA 与 framebuffer fade

`display.fill_circle_aa(cx, cy, radius, color)` 使用有界 supersample coverage 混合 RGB565 framebuffer，`radius` 限制为 `0..64`。`display.fade_to_black(amount)` 对完整 framebuffer 衰减，`display.fade_rect_to_black(x, y, width, height, amount)` 只衰减完全位于 framebuffer 内的正尺寸矩形；`amount` 均为 `0..255`。三者只标记实际 clipping 后的 dirty region，不隐式 `present`。小于一个 RGB565 channel step 的 fade 使用固定、有界的 spatial phase，避免高 FPS 下暗色 trail 永远不消失。

### Audio Track 的帧契约

`audio.new_output()` 只接收 script 指定的 `sample_rate`、`channels`、
`bits_per_sample`（固定 16）和 `volume`；Track 的 frame 大小不由 script 决定，
而是由模块向 Runtime Audio System 查询 playback format 后填入。ES8311 等由
PAL mixer 支撑的 Audio System 只接受 frame 大小与设备一致的 Track，也只接受
整数个设备 frame 的写入，因此 script 不需要、也不应该猜测这个值。

- `output:info()` 返回 `role`、`opened`、`sample_rate`、`channels`、
  `bits_per_sample`、`bytes_per_frame` 和 `frame_samples`。`frame_samples` 是
  设备每个 frame 的每通道 sample 数；设备没有固定 frame 时为 `0`。
  `frame_samples * bytes_per_frame` 是一个设备 frame 的字节数。
- `output:write(pcm)` 接受长度为 `bytes_per_frame` 正整数倍的非空 PCM。同一条
  Track 上的连续 `write` 属于同一条流：`frame_samples` 非 `0` 时模块按设备 frame
  切分写入，末尾不足一个 frame 的部分**留在 Track 内，与下一次 `write` 的开头
  拼接**，因此 script 不必按 frame 对齐也不会被插入静音。`frame_samples` 为 `0`
  时整块一次写入，此时 frame 数不得超过 `65535`。
- `output:close()` 把仍不足一个 frame 的残留补零后尽力写出，使流的结尾不被丢弃；
  Track 忙时该残留被丢弃，close 不因此失败或阻塞。job 回收时同样处理。
- `write` 成功返回 `true`。被留作残留的字节也算已接收。
- 输入本身不合法时返回两个值 `nil, "audio output: invalid frame"`，不返回
  `written`。空字符串属于这一类：虽然长度 `0` 也是 `bytes_per_frame` 的整数倍，
  但没有可写入的 frame。Track 已关闭或长度不是 `bytes_per_frame` 整数倍同样落入
  这一类。
- 输入合法但设备侧未能全部接收时返回三个值 `nil, err, written`，其中 `err` 为
  `"audio output: busy"` 或 `"audio output: write failed"`，`written` 是本次调用
  中已被 Track 接收的字节数。script 应从 `written` 偏移处续写剩余数据，不要重放
  已被接收的前缀；失败时已有的残留保持不变，重传同一 buffer 是安全的。

### Audio Input 的帧契约

`audio.new_input()` 不接收参数，一个 job 同一时间只能持有一个 Input；已打开时
再次调用返回 `nil, "audio input: already open"`。打开成功时按设备 mic format
（`sample_rate`、`channels`、固定 16 `bits_per_sample`、每通道 `frame_samples`）
分配一次性的 job 私有缓冲区；Runtime 不支持 mic、mic format 不是 16-bit PCM
时返回 `nil, "audio input: unavailable"`，缓冲区分配失败返回 `nil,
"audio input: no memory"`。返回的闭包表带有一个内部 generation 标记：`close()`
或 job 结束后旧闭包的方法一律返回 `nil, "audio input: closed"`，不能再操作已
释放、或已被下一次 `new_input()` 复用的缓冲区。

- `input:info()` 返回 `role`（固定 `"input"`）、`opened`、`sample_rate`、
  `channels`、`bits_per_sample`、`frame_samples` 和 `bytes_per_frame`；未打开
  时除 `role` 外全部为 `0`/`false`。
- `input:read(timeout_ms)` 读取一个完整设备 frame，成功返回该 frame 的 PCM
  string。`timeout_ms` 省略时为 `0`（非阻塞，立即返回是否已有 frame）。
  设备侧超时（含 `timeout_ms=0` 且当前无数据）返回 `nil,
  "audio input: busy"`；读到的数据格式或大小不合法返回 `nil,
  "audio input: read failed"`。
- `input:level(timeout_ms)` 读取一个 frame 后返回三个值：RMS 响度、峰值
  （归一化到 `[0, 1]`）和相邻样本差分估算的高频占比；错误路径与
  `input:read` 相同。
- `input:close()` 释放缓冲区、递减 Host 级 mic user 并使当前闭包的
  generation 失效；已关闭时再次调用仍返回 `true`（幂等）。
- Runtime microphone 一旦没有 frame 会持续阻塞底层 PAL 调用，因此 `read`/
  `level` 按有界时间片轮询 PAL、在每个时间片之间检查 job 取消和 Host
  stopping 状态，而不是把整个 `timeout_ms` 一次性透传给阻塞的 PAL 调用；
  job 被取消或 Host 正在停止时立即返回 `nil, "audio input: cancelled"`，
  不会让调用方的大超时（包括逼近 `UINT32_MAX` 的取值）拖住 Lua worker 或
  阻塞 Host shutdown。

## ESP-Claw profile

兼容库存固定到 ESP-Claw commit
`fb7b248114bb1b12ba0fe8e03d4b59bdbec292c1` 的 36 个 module ID。`json` 和
`capability` 为 `full`；`delay`、`system`、`display`、`lcd_touch` 和 `audio` 为
`profile`；`button` 为 `component-adapted`，表示物理 constructor 被 Runtime
component acquisition 取代、获取后的必需操作保持兼容；其余 module 为
`unavailable`，`require()` 必须确定性失败。`runtime` 是本 Feature 唯一新增的
GizOS Lua module。

## Source loading and failure

所有入口只加载 Lua 文本。绝对路径、空段、`.`/`..`、反斜线、非受限 root、
bytecode、超限或 malformed chunk 都失败关闭。`package.cpath` 为空，
`package.loadlib` 不存在；local `require()` 只能读取当前 Skill root 下的 `.lua`
文本或 immutable compiled resource。

job 只有一个终态：success、failed、cancelled、timed out 或 Host stop 产生的
stopped。事件、callback、job、coroutine 和 capability request 全部有界；full、
unsupported、invalid payload、missing component/capability 和 late completion 都
返回明确错误，不静默成功。取消后的 capability 在 job release 前保留 CLOSED 语义，
release 时归还 bounded request 槽位。

## Validation

```sh
bazel test //libs/lua:all
bazel query 'somepath(//libs/lua:lua_core, //libs/runtime:runtime)'
rg -n 'h2_runtime_(poll|wait)_event' libs/lua/src
bazel run //projects/e2e/targets/cc_binary/lua-runtime:e2e-lua-runtime
```

query 和 `rg` 都应为空。E2E 的九个固定 case 见 [E2E 测试 App](/apps/e2e)。
