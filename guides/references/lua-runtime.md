# Lua scripting API

This is the scripting reference for GizOS's standard Lua Runtime Host. The
[Host guide](../zh/developing/lua.md) owns configuration, scheduling, resource
limits, App storage, audio framing and BLE Link lifecycle. The public headers own
the [Display contract](./lua.md) and [numeric contract](./lua-numeric.md).
These contracts describe platform source, not the capabilities of every firmware.

## Environment

The Host runs text-only Lua 5.5 jobs, each with its own VM, copied string-valued
`args` table and memory budget. Standard base, coroutine, table, string, math and
utf8 libraries are available. `debug` contains only `traceback`. `io`, `os`,
`load`, `loadfile`, `dofile`, `string.dump`, `package.loadlib` and native/bytecode
loading are unavailable. `require` resolves built-ins, registered native modules
and Host-provided local text resources; it is not a general filesystem loader.

`print(...)` sends tab-separated values as one bounded Runtime Log message;
exceeding the message limit raises an error. Jobs, callbacks and spawned tasks
share their VM. They are cooperative coroutines, not native threads. The Host
controls task/callback capacities, instruction quantum, timeouts and memory.
Device modules can fail to acquire missing or busy hardware. Cached `require`
does not reopen a deinitialized device.

## runtime

Load with `local runtime = require("runtime")`. Task IDs belong to the current
job. The scheduler field is `runtime.scheduler == "cooperative"`.

| Signature | Parameters, results and constraints |
| --- | --- |
| `runtime.spawn(fn, ...)` | `fn` is a function; remaining arguments are passed to it. Returns an integer task ID. Capacity exhaustion raises a Lua error. |
| `runtime.yield()` | Requeues the current scheduler task; returns no values when resumed. Requires a Host scheduler task. |
| `runtime.sleep(ms)` | Integer `ms` in 0..4294967295. Yields until a monotonic deadline; returns no values. Cancellation does not wait for that deadline. |
| `runtime.status(task_id)` | Integer task ID. Returns `"ready"`, `"waiting"`, `"done"`, `"failed"` or `"cancelled"`; unknown IDs return `nil, "unknown task"`. |
| `runtime.join(task_id)` | Waits for another task in this job. Returns `true, message` on completion or `false, message` on failure/cancellation. It does not forward the function's Lua return values. Invalid/self targets raise an error. |
| `runtime.cancel(task_id)` | Requests cancellation; returns `true` for a live target, `false` for an unknown or terminal task. |
| `runtime.components.get(id)` | Positive Runtime component ID. Returns a cached Button proxy, or `nil, message` for invalid/unknown/unsupported components. IDs come from the launcher, often via string-valued `args`; convert with `tonumber`. |
| `runtime.components.on(id, kind, fn)` | Registers a function for a known component and supported event kind; returns a callback handle. Invalid component/event combinations or exhausted callback capacity raise an error. |
| `runtime.components.off(handle)` | Integer callback handle. Removes the registration and returns `true`, or `false` if absent. |
| `button:get_key_level()` | On the proxy obtained from `runtime.components.get`: returns 1 when pressed, 0 when released, or `nil, message` on a Runtime error. Only one Button component can be bound per job. This is not a `require("button")` constructor. |

`runtime.event` supplies `BUTTON_DOWN`, `BUTTON_UP`, `BUTTON_ACTION`, `NFC_STATE`,
`IMU_GESTURE`, `ERROR` and the Link event constants described in the Host guide.
The app must forward Runtime events to this job. Callbacks run as scheduler tasks
and may yield. Registering a callback does not invent a physical component.
`runtime.components.getByName` and `board_manager` are unavailable.

<!-- example: cooperative-tasks -->
```lua
local runtime = require("runtime")
local child = runtime.spawn(function(label)
    runtime.sleep(10)
    print(label)
end, "child finished")
local ok, message = runtime.join(child)
assert(ok, message)
```

## delay and system

| Signature | Parameters, results and constraints |
| --- | --- |
| `delay.delay_ms(ms)` | Same scheduler-based wait and bounds as `runtime.sleep`; no return values. Load `require("delay")`. |
| `delay.delay_us(us)` | Integer 0..1000000; busy-waits on Runtime monotonic microsecond time, requires a scheduler task. Returns no values; clock failures raise an error. Prefer millisecond sleeps for cooperative waiting. |
| `system.millis()` | Monotonic Runtime milliseconds as a Lua integer. |
| `system.uptime()` | Monotonic Runtime milliseconds divided by 1000, truncated to whole seconds. |
| `system.time()` | Wall-clock Unix seconds, or `nil, pal_result` when the clock is unavailable. |
| `system.date(format)` | Optional format, default `%Y-%m-%d %H:%M:%S`. Uses wall time plus the Host's fixed UTC offset. Supports only `%%`, `%Y`, `%m`, `%d`, `%H`, `%M`, `%S`, `%F`. Returns a string; unsupported formats, unavailable clocks and output beyond 127 bytes raise errors. |
| `system.info()` | Returns a table with `uptime_s`; `time` (Unix seconds) and `date` are present only when wall time is available. |
| `system.ip()` | Returns `nil`; this profile exposes no IP lookup. |

Load `require("system")`. `system.heap` is an unsupported ESP introspection
compatibility stub, not a portable heap API. No timezone database is exposed.

## json and capability

| Signature | Parameters, results and constraints |
| --- | --- |
| `json.encode(value)` | Returns JSON text for supported scalar/table values and `json.null`. Cycles, excessive nesting, non-finite numbers and unsupported types raise errors. Output is bounded by the Host output limit. |
| `json.decode(text)` | Parses a JSON string into Lua values/tables. JSON null becomes `json.null`, not Lua nil. Malformed input, nesting beyond 16 and input beyond the Host source limit raise errors. |
| `capability.call(name, payload, options)` | String name in the Host's frozen registry. Optional payload is nil, a table or a JSON string; optional options is a table. Nil becomes `{}` and tables are JSON encoded. Returns `true, output_string, nil` on success, or `false, nil, error_string`. Pending calls yield the scheduler task. Unknown names return the failure tuple. |

Load `require("json")` and `require("capability")`. Capability names, payload
schemas and authorization belong to the embedding app; GizOS does not provide a
universal device-command catalog. Successful capability output is a string, not
an automatically decoded object.

<!-- example: json-roundtrip -->
```lua
local json = require("json")
local text = json.encode({ label = "GizOS", enabled = true, missing = json.null })
local value = json.decode(text)
assert(value.label == "GizOS" and value.missing == json.null)
```

## storage and kv

These modules require a Host filesystem configuration and a submitted app ID.
They share the same App namespace, quota, file count and Host mutex. Filenames
and keys use 1..32 bytes of lowercase letters, digits, `_`, `-`, `.` and cannot
start with `.`. The storage namespace is flat. See the Host guide for atomic
replacement, persistence, quotas, failure reasons and the KV snapshot format.
Ordinary operational errors return `nil, message`; type errors raise Lua errors.

| Signature | Parameters, results and constraints |
| --- | --- |
| `storage.get_root_dir()` | Returns `""` for an available App namespace; unavailable storage returns `nil, message`. |
| `storage.join_path(...)` | Joins string arguments with `/`, skipping empty parts and normalizing separator edges; this helper does not grant directory access. |
| `storage.exists(name)` | Returns boolean for a legal name; unavailable storage returns `false`, invalid names/I/O failures return `nil, message`. |
| `storage.stat(name)` | Returns `{type="file", size=bytes}` or `nil, message`. |
| `storage.read_file(name)` | Returns file bytes as a Lua string or `nil, message`. |
| `storage.write_file(name, data)` | String bytes, atomically replaces the file; returns `true` or `nil, message`. |
| `storage.listdir(path)` | Path omitted or `""`; returns entries with `name`, `type`, `size`. Other names do not provide subdirectories. |
| `storage.remove(name)` | Returns `true` on successful deletion or `nil, message`. |
| `storage.rename(old_name, new_name)` | Renames within this App's namespace; returns `true` or `nil, message`. |
| `storage.get_free_space()` | Returns `{total, free, used}` for the App quota, not physical partition capacity. |
| `kv.get(key)` | Returns the stored scalar, a single nil for a missing key, or `nil, message` on error. |
| `kv.set(key, value)` | Stores string, boolean or finite number; returns `true` or `nil, message`. Nil is not deletion. Maximum 128 keys. |
| `kv.remove(key)` | Returns `true` if deleted, `false` if absent, or `nil, message`. |
| `kv.exists(key)` | Returns boolean or `nil, message`. Unavailable storage is an error, not a missing key. |
| `kv.keys()` | Returns keys sorted by byte order or `nil, message`. |

<!-- example: persistent-setting -->
```lua
local kv = require("kv")
local volume, err = kv.get("volume")
if err then
    print(err)
elseif volume == nil then
    local ok, write_err = kv.set("volume", 60)
    if not ok then print(write_err) end
else
    print("saved volume", volume)
end
```

## Display, touch and audio

Load `require("display")`, `require("lcd_touch")` or `require("audio")` only when
the embedding Runtime provides those devices. Display and touch acquisition can
raise errors. Display colors are supported names or `{r=..., g=..., b=...}`
with integer RGB888 channels. `green` is RGB(0,128,0); bright green is explicit
RGB(0,255,0). Drawing changes the framebuffer; the app explicitly presents it.
The public Display header documents advanced drawing, regions, asynchronous
submission, ownership, bounds and failure behavior.

| Signature | Parameters, results and constraints |
| --- | --- |
| `display.clear(color)` | Clears the framebuffer to a supported color; no return values. |
| `display.fill_rect(x, y, width, height, color)` | Integer pixel rectangle, nonnegative dimensions; clipped to framebuffer. No return values. |
| `display.draw_line(x1, y1, x2, y2, color)` | Integer endpoints; draws a clipped line, no return values. |
| `display.draw_text(x, y, text, options)` | Integer position and string; optional table accepts `font_size` (default 24) and `color` (default white). Draws the built-in bitmap glyphs at integer scale `(font_size+6)//7`, with scale 1..8; returns no values. Invalid size/scale or excessive text length raises an error. |
| `display.present(options)` | Optional table: `retained` boolean, `bounds` boolean, `tiles` boolean, `merge_gap` integer 0..8. Waits for transport and returns `pixel_count, rectangle_count`; failures raise an error. Retained mode is initially disabled, then preserved unless explicitly changed. |
| `display.submit(options)` | Same options, with retained mode defaulting to true. Returns `sequence, planned_pixels, planned_rectangles` after the copy handoff; transport remains asynchronous. Full mailbox/fault returns `nil, pal_result`. At most one submission may be in flight. |
| `display.flush()` | Polls the last accepted submission: returns a status table, `nil, display.BUSY` while pending, or `nil, pal_result` on failure. It does not wait for transport. |
| `display.status()` | Returns submission counters, timestamps, `clock_valid`, `pixels`, `rects`, `busy`, `closing`, `error`, `completion_kind="transport"`. Transport completion does not prove display scanout. See the Display header contract for each field. |
| `display.deinit()` | Drains/releases the acquisition; success returns no values, pending/fault returns `nil, pal_result`. Faulted resources remain retained for checked Host cleanup. Cached module proxies do not reopen it. |
| `lcd_touch.read()` | Returns the cached touch result without polling. |
| `lcd_touch.poll()` | Polls one event; returns a table with `pressed`, `just_pressed`, `just_released`, `x`, `y`, `dx`, `dy`, `moved`, `held_ms`. No pending event returns cached state with zero movement/edge flags. Errors raise Lua errors. |
| `lcd_touch.sync()` | Resets coordinate initialization and polls one event, suppressing the first delta. |
| `audio.new_output(options)` | Required table; defaults: `sample_rate=16000`, `channels=1`, `bits_per_sample=16`, `volume=90`. Volume is 0..100; format must be supported by Runtime. Returns a Track or `nil, message`. Frame sizing comes from Runtime, not a script-selected field. |
| `output:write(pcm)` | Nonempty 16-bit PCM bytes aligned to `bytes_per_frame`; returns `true`, `nil, message` for invalid input, or `nil, message, written` on partial device acceptance. Retry only the unaccepted suffix. Partial device frames are retained across writes. |
| `output:info()` | Returns role/open state, sample format, `bytes_per_frame` and per-channel `frame_samples`. |
| `output:close()` | Closes the Track; a trailing partial device frame is zero-padded and written best-effort. |
| `audio.new_input()` | Returns one Input for this job or `nil, message`; Runtime must provide 16-bit PCM mic frames. |
| `input:read(timeout_ms)` | Optional integer timeout, default 0; returns one PCM frame or `nil, message`. |
| `input:level(timeout_ms)` | Reads a frame and returns RMS, peak (normalized 0..1) and a high-frequency estimate, or `nil, message`. |
| `input:info()` | Returns input role/open state and device format fields. |
| `input:close()` | Idempotently releases this Input; returns `true`. Old Input closures cannot access a subsequently opened Input. |

<!-- example: display-frame -->
```lua
-- Requires a Runtime Display; the launcher chooses its size and driver.
local display = require("display")
display.clear("black")
display.fill_rect(8, 8, 24, 24, { r = 80, g = 200, b = 255 })
local pixels, rectangles = display.present()
assert(pixels ~= nil, rectangles)
```

## Link and optional modules

`require("link")` is always present, but BLE operations require the launcher to
link and enable `//libs/lua:lua_link` before starting the Host. The Host guide
owns the complete Link API, tag/message/stream bounds, return values, event
payloads, session lifecycle and teardown. `link.available()` returns a boolean;
check it before starting a session. Availability is not a promise about any
particular device or published firmware.

| Signature | Result |
| --- | --- |
| `link.available()` | Boolean: whether the launcher enabled Link for this Host. |
| `link.on(kind, fn)` | Registers a supported Link event callback and returns its integer handle. Register before host/join. |
| `link.off(handle)` | Removes a callback handle; returns boolean, sharing the Runtime callback registry. |

The pinned ESP-Claw inventory distinguishes full, profile, component-adapted and
unavailable modules. In particular, `gpio`, `wifi`, `http`, `lvgl` and arbitrary
upstream modules must not be assumed to exist. Use the generated module index,
this Runtime's registry and the embedding firmware's declared capabilities.
