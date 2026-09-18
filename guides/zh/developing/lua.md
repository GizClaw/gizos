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

当前 build surface 包含 Desktop、Web、ESP32-S3/P4、BK7258 和 BK3633。Embedded
构建不给 upstream Lua 提供 libc 文件或标准流：vendor overlay 强制包含
`h2_lua_embedded_stdio.h`，在 `<stdio.h>` 之后把 `stdin`、`stdout`、`stderr`
以及 Lua 使用的 `fopen`、`getc` 等 stdio 函数重定向到
`//third_party/lua_patch` 中 fail-closed 的 shim，避免 C library 用宏通过
`_impure_ptr` 等 reentrancy 状态展开它们。只有 ESP32-S3/P4 额外链接 `lua_esp_libc_compat.c`，补齐 ESP-IDF picolibc
缺少的 newlib 兼容符号；BK 使用 SDK toolchain 自带的 newlib。
`//libs/lua:lua_firmware_abi` 在每个 embedded 配置中对完整 Lua archive closure
运行 firmware archive ABI 检查。GizOS 自身没有链接 Lua 的 BK image，BK 设备上的
运行验证由消费 Lua 的 firmware 负责。

## Host 和 job

`h2_lua_host_config_t` 的容量均有界：`worker_count`、`worker_stack_size`、`max_jobs`、`max_coroutines_per_vm`、`ready_queue_capacity`、`waiter_capacity`、`event_delivery_capacity`、`callback_capacity_per_job`、`audio_track_capacity_per_job`、`pending_capability_capacity`、`instruction_quantum`、`resume_time_budget_ms`、`source_limit_bytes`、`output_limit_bytes`、`vm_memory_limit_bytes` 和 `vm_heap_bytes`。零使用声明的默认值；ready/waiter 容量不得小于 VM 的 coroutine 上限。`storage` 配置每个 App 的持久化存储，见 [App 存储](#app-存储)；全零表示未配置。

`vm_heap_bytes` 可选地在 `h2_lua_host_create()` 时从 Runtime mem 预留一段 VM 专用堆，
供同一 Host 的所有 job/worker 通过带 PAL mutex 保护的 TLSF 共享。默认 `0` 保持
VM 逐块向 Runtime mem 申请，Web 入口也保持此默认值。适合设备系统堆碎片化、
大字符串或全屏 `display.capture_region` userdata 等大块分配会与其他模块争抢连续空间的场景。
VM 本体、Lua 状态、userdata、字符串和表都使用预留堆；callbacks、events、tasks
和 framebuffer 等仍使用 Runtime mem。

Runtime mem 有足够大的连续块时预留为一整块；否则 Host 每次被拒后把申请大小缩小 1/8、贴近实际最大空闲块，最多取
8 块、每块至少 256 KiB（只有最后的余量可以更小），全部加入同一个 TLSF。单次 VM
分配必须能放进其中一块。在这些限制内凑不够时返回 `H2_PAL_ERR_NO_MEMORY`，不创建
Host，也不泄漏已取得的块；destroy 在所有 job/VM 释放后归还全部块。

`vm_memory_limit_bytes` 仍是独立的每 VM 配额，预留大小不会改变配额检查。预留堆需
覆盖所有同时存活的 VM（包括尚未 release 的已完成 job）以及 TLSF 元数据和每块分配
头；按实际负载测量设定，不要直接等同配额。预留小于配额也允许，此时预留堆先耗尽；
Lua 仍会先尝试 emergency GC，无法满足分配时再报内存错误。非零值的最小值为
`tlsf_size() + tlsf_pool_overhead() + 8 * (tlsf_block_size_min() + tlsf_alloc_overhead())`，
可用池也不得超过 `tlsf_block_size_max()`；越界返回 `H2_PAL_ERR_INVALID_ARG`。

Host 的正常生命周期是：

1. `h2_lua_host_create()` 借用 Runtime 并分配固定容量；
2. 在 start 前注册 native module 和 capability；
3. `h2_lua_host_start()` 冻结 registry 并创建 worker；
4. 通过 text、compiled resource 或 Runtime Filesystem 提交 job，同时给出决定 `storage` 作用域的 app id（可为 `NULL`）；
5. App 消费 Runtime Event queue，并通过 `h2_lua_dispatch_runtime_event()` 定向
   投递给一个 live `job_id`；事件入队返回 `H2_PAL_OK`，事件格式错误或 component
   kind 与 Runtime component 不符返回 `H2_PAL_ERR_INVALID_ARG`，未知或已 release 的
   job 返回 `H2_PAL_ERR_NOT_FOUND`，job 已进入终态返回 `H2_PAL_ERR_CLOSED`，job 内
   未投递事件已达 `event_delivery_capacity` 返回 `H2_PAL_ERR_FULL`，失败时事件不
   入队。job 可能在同一批 Runtime event 之间进入终态，所以同一 job 上 `CLOSED`
   可以紧跟在 `OK` 之后出现；
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
| `display` | `clear`、矩形、线、圆、AA circle、圆角矩形、三角形、多边形、椭圆、保留命令、framebuffer fade、frame、text、`present` 和 `deinit` | 直接使用 Runtime singleton Display API；dirty region 始终裁剪到 framebuffer |
| `lcd_touch` | `read`、`poll`、`sync` 及 upstream touch result fields | 直接使用 Runtime singleton Touch API，不接收 SDK handle |
| Button proxy | `get_key_level` | Runtime normalized Button snapshot，不创建 GPIO button |
| `storage` | `get_root_dir`、`join_path`、`exists`、`stat`、`read_file`、`write_file`、`listdir`、`remove`、`rename`、`get_free_space` | Host 配置的 PAL Filesystem；每个 app id 一个扁平目录，受配额和文件数限制，写入原子替换 |
| `kv` | `get/set/remove/exists/keys` | 与 storage 共享 App namespace、配额和 Host mutex 的持久化标量 KV |
| `audio` | `new_output`（每条 Track 的 `write/info/close`）、`new_input`（`read/level/info/close`） | 直接使用 Runtime singleton Audio System；Track frame 大小取自设备 playback format，Input frame 大小取自设备 mic format；PAL 混合多条 Track，不接收 codec handle |
| `link` | `available`、`host`、`join`、`send`、`send_unreliable`、`write`、`read`、`close`、`state`、`on`、`off` | Launcher 调用 `h2_lua_link_enable()` 后由 `//libs/lua:lua_link` 经 BLE Host PAL（不可靠消息）与 `libs/bleikcp`（可靠消息、字节流）提供；未启用或没有 BLE 时 `available()` 为 `false`，操作返回 `nil, "link: unavailable"` |

`kv`、`link` 与 `runtime` 同属 GizOS 新增 module，不在 ESP-Claw 兼容库存内。

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

`display.draw_circle(cx, cy, radius, color)` 绘制裁剪到 framebuffer 的一像素圆周。圆心允许处于 Display 宽高的一倍负边界到两倍正边界内，半径必须位于 `0..min(display.width, display.height)`；超出范围、Display 未打开或颜色无效时保持现有 Lua argument/error 合同并确定性失败。`clear`、矩形、圆和圆角矩形可以批量写 framebuffer；默认 `present` 提交所有待绘制图元的 dirty bounding union，可显式启用下述 retained 比较模式。

### Display 多边形、椭圆与保留命令

`display.fill_polygon(points, color, offset_x=0, top=0, bottom=height, scale=1)` 接收 3..128 个 `{x,y}` 点。坐标和横向偏移必须有限且位于 ±100000，缩放为 `0<scale<=16`。先缩放顶点，再在整数行使用 even-odd 扫描转换；边的纵向范围下闭上开，每对交点覆盖 `ceil(left)..floor(right)`，包含水平区间的两个端点。交点取整后才将横向偏移按 `floor(value+0.5)` 加入，不等于在顶点变换阶段平移。支持凹多边形、自交、重复点和退化边；后两者不产生除零。

`display.fill_ellipse(cx, cy, rx, ry, color, offset_x=0, top=0, bottom=height)` 使用相同坐标范围，要求 `rx>=0`、`0<ry<=2048`。局部 y 从 `-ry` 开始以 1 递增到 `+ry`；目标行是 `floor(cy+y+0.5)`，半宽是 `rx*sqrt(max(0,1-y*y/(ry*ry)))`。左端是 `floor(cx-half_width+offset_x+0.5)`，宽度是 `floor(2*half_width+1.5)`；小数半径保留这一像素采样规则，不隐式缩放 framebuffer。

`display.compile_commands(commands)` 把最多 16384 条六字段命令复制为 VM 所有的不可变 userdata。`{0,x,y,width,height,color}` 表示矩形，`{1,x,y,x2,y2,color}` 表示线段。kind 必须是整数；四个数值字段必须有限、位于 ±100000，向零截断为像素整数；矩形尺寸不能为负。空列表合法。编译后修改或释放原表不影响命令；保留 userdata 使它跨 GC 存活，释放最后一个引用后可回收。复制体计入该 job 的 VM 内存预算，不创建系统堆缓存，也不自动扩容。

`display.draw_commands(handle, top=0, bottom=height, offset_x=0, offset_y=0, scale_x=1, scale_y=scale_x, color_override=nil)` 按原顺序重放。偏移必须有限且位于 ±100000，两个缩放均为 `0<scale<=1000`；端点按 `floor(offset+coordinate*scale+0.5)` 计算。矩形使用半开区间，线在 Bresenham 迭代前裁剪；因此远在屏幕外的端点不造成按距离增长的光栅循环。颜色覆盖不修改原命令。颜色沿用 Display 字符串或 `{r,g,b}` 合同；`green` 为 RGB(0,128,0)，满亮度绿色须显式传入 RGB(0,255,0)。

以上绘制使用 `[top,bottom)` 行裁剪和 framebuffer 列裁剪，要求整数 `0<=top<=bottom<=height`，在转换为 native int 前检查；空裁剪和零面积矩形不修改像素。参数解码完成后才开始绘制，非法参数或 Display 已关闭时抛 Lua 错误；RGB 表的 getter 仍遵循 Lua 元方法语义，其自身的副作用不属于绘制的原子性保证。编译失败不会返回部分句柄，OOM 后释放临时数据即可再次尝试较小批次。绘制只标记 dirty union，不隐式 present；Display deinit 后保留命令不持有 framebuffer，也不允许继续绘制。重复调用缓存的 `require('display')` 不代表重新打开设备。

### Display indexed rectangles and palettes

`display.compile_rects(records)` 复制具名 `{x,y,width,height,color_index}` 记录为不可变 userdata，`display.compile_palette(colors)` 将既有颜色字符串或具名 `{r=...,g=...,b=...}` 转为固定长度 RGB565 userdata。两者最多 16384 项，空列表合法，数据计入 VM；不改变已有 commands/mesh 接口。Lua 颜色索引从 1 开始，绘制时验证实际 palette 长度。

`display.blend_palette(output,a,b,progress)` 要求三套 palette 长度一致，进度是 `0..256` 的整数，输出可与输入相同。`display.draw_rects(batch,palette[,left,top,right,bottom])` 使用已打开的 Display，四个半开 clip 整数要么全部提供，要么全部省略。成功返回零个值；调用不隐式 present，不增长容量、不分配内存。完整参数与像素合同见 [Lua API](../../references/lua.md) 和 [Raster2D](./raster2d.md)。

C core 完整校验后绘制，adapter 再逐矩形标记已有 dirty/background damage。没有新缓存或损伤对象。构造和 palette blend 不自行打开 Display；`require('display')` 仍沿用既有 acquisition，关闭后的 proxy 不可绘制。参数 getter 自身可以有副作用；构造过程不写 framebuffer，后续 draw 必须重新检查 Display 状态。GC、取消、job release 与 Host teardown 继续走已有回收流程。

### Display 笔画与有界缓存

`display.stroke_path(points,widths,color,offset_x=0,top=0,bottom=height,cache=false,fast=false,smooth=false,scale=1,tolerance=0)` 接受 `2..256` 个有限 ±100000 的点对、恰好 `n-1` 个 `0..1000` 宽度，以及单色或 `n-1` 个颜色。cache/fast/smooth 必须为 boolean；scale 为 `0<scale<=16`，先作用于坐标和宽度；offset 有限且位于 ±100000。top/bottom 沿用屏内整数半开行裁剪。所有参数、颜色 getter 和分配在绘制前完成并重新检查 Display。返回 `(cache_hit,fast_segment_count)`，不是帧率。

首参数也可为 `{buffer=xy,count=n}`，其中 xy 是容量至少 `2*n` 的 f64 packed xy buffer，n 为 `2..256` 整数，坐标沿用有限 ±100000 限制；descriptor 不得混入数字键点数组。字段使用 raw 读取，每次复制并验证当前坐标后再绘制。稳定 descriptor 表持有原 normals cache，widths 表持有原 span cache；坐标值、数量和样式变化会失效，不能只比较 buffer 身份。其他参数、返回值、像素算法与显式 present 行为不变。buffer 在本次调用期间保活，颜色 getter/GC 后仍检查 Display 生命周期，不跨调用保存裸指针。

默认 hard 模式为每段宽度居中的四边形与中心线，长度小于 `.01` 时绘制取整方块；非退化零宽度段保留中心线。fast 使用带整数边界误差检查的 float/FMA 四边形，不满足条件时回退双精度几何。width-independent 双精度法线缓存随点表存活，并比较全部坐标。cache 随宽度表保留最多 2048 条有序扫描段/中心线记录；键包含缩放后坐标、宽度、颜色、偏移、裁剪、viewport 和模式。容量不足时继续绘制完整结果但使缓存失效，不能截断画面。两类缓存都计入 VM，释放点/宽度表后可以 GC 回收，不持有 framebuffer。

smooth 显式启用圆端点连续覆盖，每像素只混合最大 alpha 一次，相同 alpha 保留先前段颜色，零宽度跳过。像素中心为 `(x+.5,y+.5)`；覆盖为 `clamp(radius+.5-distance,0,1)`，取整到 `0..255` 后使用现有 RGB565 blend。端点范围不超过 4096 时保留 screen-local float 快速覆盖，极端坐标使用双精度回退；它不承诺与 hard 模式相同像素。颜色/覆盖 scratch 按裁剪区域分配、在同一 job 内复用，并在 Display/job/Host 关闭时释放引用。tolerance 是默认关闭的 `0..0.25` 屏幕像素弦误差，仅用于 smooth；保留样式边界、拒绝回折并检查所有省略点，不改变世界物理节点或时间步。

通用多边形使用 float edge-slope/integer-boundary 检查，不能确定相同 floor/ceil 时回退原双精度交点表达式。参考像素测试覆盖边界与确定性随机输入，不把有限样本当作数学证明。笔画通过 Utils 公共 `h2_f32_math.h` 消费单份数值辅助；编译器和浮点环境约束见 [Utils](./utils.md)，不要对绘制或物理库启用 fast-math。

### Display 快照、背景恢复与 retained 提交

`display.capture_region(x,y,width,height,key=nil,reuse=nil)` 捕获 framebuffer 中的正尺寸区域，宽高各不超过 4096，位置和尺寸必须为整数且完整位于屏内。它只保存已绘制的 RGB565 像素，不加载贴图或文件。省略 key 保存不透明区域；指定 key 时压缩每行两侧透明边距，并预编译非透明连续段。透明捕获先取得 VM 内完整临时副本，再对不可变副本压缩，防止 allocation-triggered GC 改变两次扫描之间的像素。reuse 仅接受同尺寸的不透明快照，且本次不能指定 key；返回同一 userdata，不重新分配像素存储。重新捕获当前背景会使恢复基线失效，下次完整恢复。

`display.draw_region(region,x=0,y=0,top=0,bottom=height,key=nil,left=0,right=width)` 按原生像素尺寸重放，不缩放。整数 x/y 范围为 ±100000；整数裁剪边界构成屏内半开矩形。省略 key 表示不透明重放，包括还原压缩时省略的边距颜色；不同的重放 key 同样正确还原捕获内容。重放 key 等于捕获 key 时直接复制预编译连续段。空裁剪不写像素。颜色 getter 完成后检查设备状态，错误参数不造成绘制写入，但 getter 自身副作用仍属于 Lua 行为。

`display.restore_background(region)` 仅接受完整屏幕、不透明快照，并保留 VM 引用。首次绑定或恢复基线失效时完整复制；之后把所有绘制操作标记的 16×16 脏 tile 合并成相邻行段，仅恢复这些区域，然后清空背景损伤标记。背景恢复与上一帧提交是独立状态，不能用“已提交”代替“已恢复”。`display.release_background()` 幂等解除引用；快照本身仍可重放，最后一个引用释放后由 GC 回收。

`display.present(options=nil)` 和 `end_frame(options=nil)` 返回实际提交的 `(pixel_count, rectangle_count)`。options 是普通表，字段用 raw lookup 读取：`retained` 为 boolean，显式启用或禁用上一成功帧比较，省略则沿用当前模式；`bounds` 为 boolean，当前调用合并为一个包围矩形；`merge_gap` 为 `0..8` 整数，允许 tile 行段合并跨过指定数量的未变化 tile。retained 首帧或失效后完整提交，之后先完成候选 tile 的像素比较，再提交变化区域，完全静止时返回 `(0,0)`。即使没有像素变化，也调用 PAL present 并传播其错误。任何 draw/present 失败都使提交基线失效并要求下次完整重试，部分成功的矩形不能作为完整成功帧。禁用 retained 时释放比较存储，并完整提交一次再恢复 dirty union 模式。这些统计是软件提交量，不是实机 FPS。

快照、背景损伤标记、retained 比较图和基线像素都计入 VM 内存，原工作 framebuffer 保留 PAL ownership。Lua deinit、job release 和 Host teardown 在释放 framebuffer 或执行 VM finalizer 前断开全部显示缓存引用。teardown 期间不能重新打开 Display；正常 deinit 后旧 proxy 的绘制调用失败。OOM 不返回部分快照，释放其他 VM 数据后可重试；已有快照不因另一次捕获失败而失效。

### Display 保留几何与 native 更新

`display.compile_mesh(vertices, primitives, vertex_capacity=#vertices, primitive_capacity=#primitives)` 创建 VM 所有的保留几何；`display.update_mesh(handle, vertices, primitives)` 在固定容量内替换所有活动数据。顶点是 `{x,y}`，primitive 是 `{kind,first,count,color}`；kind 0 为 3..128 顶点多边形，kind 1 为两顶点线段。Lua 的 first 从 1 开始，允许重叠的连续顶点范围。最多 65536 顶点、4096 primitives；坐标必须有限且在 ±1000000 内。空数据和零容量合法。Lua 更新先在 VM 临时存储中完整解码，任何后续参数错误都不会让本次更新只写入一部分；成功后替换活动长度、拓扑、颜色并使派生坐标失效。调用方颜色 getter 自身的副作用仍按 Lua 语义执行。

`display.draw_mesh(handle, options=nil)` 按原顺序绘制。options 的字段使用 raw lookup，缺省值不从元表获取：

| 字段 | 合同 |
| --- | --- |
| `matrix` | `{a,b,c,d,tx,ty}`，默认单位矩阵；计算 `x'=(a*x+c*y)+tx`、`y'=(b*x+d*y)+ty`，字段有限且在 ±1000000 内 |
| `transform` | 可选 `{x=...,y=...,scale=...,angle=...}`，四项均必填且 raw 读取；与显式 matrix 互斥。x/y/angle 有限且在 ±100000 内，`0<scale<=100`，必须显式提供 1..16 的 grid |
| `grid` | matrix 路径为整数 0..16，默认 0，不吸附；非零时用 `floor(value/grid+0.5)*grid` 吸附变换后的顶点。transform 路径为必填整数 1..16；选择策略由应用提供 |
| `offset_x` | 有限且在 ±100000 内，默认 0；多边形在交点取整后横移，线段在连续裁剪前横移，不与 matrix 平移合并 |
| `left,top,right,bottom` | 默认整个 framebuffer 的整数半开裁剪矩形，范围必须完全位于 framebuffer 内；空矩形合法 |
| `color` | 可选 Display 颜色覆盖，不修改保留颜色 |
| `cache` | boolean，默认 false；按需保留有序扫描段/线记录，最多 8192 条；容量从 512 条起按需增长，重放后收缩到实际条数 |

所有派生顶点在光栅化前验证为有限且在 ±16000000 内。多边形沿用上述 even-odd 扫描和交点取整规则；线先连续裁剪再按 `floor(endpoint+0.5)` 取整并执行 Bresenham。绘制不隐式 present，关闭 Display 后拒绝绘制。派生顶点缓存以内容更新、matrix／transform 和 grid 为失效条件；裁剪、颜色、offset 每次绘制应用，不能因坐标缓存命中而跳过。可选 span 缓存还比较裁剪、viewport、offset 和 recolor，命中时按原顺序重放并标记 dirty/background damage；容量溢出仍完整绘制，但不发布部分缓存。成功的 native/Lua 更新要求重新验证派生坐标，但保留上一次成功绘制的 span 候选。完整验证后，只有活动顶点／primitive 数量、primitive 类型／范围／颜色、最终坐标和上述绘制参数全部相同时才能复用；不能只比较地址或包围盒。连续更新、失败调用和不保留缓存的绘制不会覆盖候选快照，相同输入的热调用复用已验证的比较结果。首次启用缓存时分配 512 条记录，并按声明容量分配一份顶点／primitive 快照及对齐／固定元数据。一次光栅化超出容量时本帧仍完整绘制但不发布缓存，下一次绘制在入口把缓存换成 4 倍容量（最多 8192 条）。缓存至少被重放一次后，若剩余空位不少于 256 条，下一次绘制在入口把它换成恰好容纳现有记录的缓存，复制记录与快照；收缩后的缓存若再次溢出，直接恢复为 8192 条并不再收缩，动画网格不会反复分配。增长、收缩或恢复都只在入口分配；分配引起的 finalizer 重入若改变了缓存，则放弃替换并沿用重入留下的缓存。此后热绘制不分配。数据和缓存计入 VM；引用释放后由 GC 或 VM teardown 回收。

显式 transform 保留 `(x+(vx*cos(angle)-vy*sin(angle))*scale)/grid` 及 y 对应式的运算顺序，不预乘为 affine matrix。三角函数在参数不变时复用。每轴使用原版 float 表达式和 `64*FLT_EPSILON` 误差界：远离半格点且误差小于 0.25 时走 float 取整，否则使用原版 double 表达式；源顶点超出 ±100000 时也回退。应用决定 grid、pose、布局和复用时机；库内没有尺寸阈值或额外输出缩放。

精确单位矩阵且 `grid=0` 时，绘制直接读取 mesh 自有、已在创建／更新阶段验证的顶点，不再复制到派生坐标缓存或逐点重复计算单位变换，也不借用调用方缓冲区。既有派生坐标存储仍为一般变换保留，不增加容量或分配。此快路径不使用近似比较；非单位矩阵或非零 grid 仍在修改坐标缓存和像素前完整验证变换结果，错误、像素和缓存失效合同不变。

非 identity 变换在活动顶点不超过 1024 时，可使用独立的 Display 共享暂存区一次计算、完整验证后提交。暂存区按 `min(vertex_capacity,1024)*16` 字节 payload 加 userdata 开销计入 VM，多个 mesh 复用容量；首次使用或增长容量可能分配，增长期间旧、新分配可能同时存活。Display release 或 VM teardown 释放共享引用。活动顶点超过 1024 时仍使用原来的验证／变换两遍路径，公开 65536 顶点上限不变。暂存区不借用源顶点、已提交位置或完整 span 候选；所有可能重入的分配之后重新读取 mesh 与 Display 状态，完整验证到发布之间不分配、不回调。失败不撤销用户 finalizer 自身的合法修改，也不得用外层旧状态覆盖它们。

私有 native 计算模块通过生产公共头 `h2_lua_display.h` 创建／更新同一种 userdata，再交给 `display.draw_mesh`。C API 的完整参数、错误和 ownership 合同见从该头 Doxygen 生成的 [Lua Display API Reference](/references/lua)；native indices 从 0 开始，不同于 Lua 表。native 更新不分配、不增长 Lua stack，调用方预留两个空栈槽；创建通过受保护的 Lua 调用处理 OOM，失败恢复栈。它们不打开 Display、不绘制、不暴露内部存储地址，模块不得获取 job/framebuffer 或 include runtime 私有头。鱼身变形、场景投影、分色、网格选择等策略由应用先计算。

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

### BLE peer link

`link` 让两台相邻设备通过 BLE 配对，在同一条连接上提供三种传输：可靠消息、不可靠
消息和字节流。它只使用 BLE Host、Task、Sync、Time、Mem 和 System Event PAL，不
使用 Wi-Fi、Netif 或任何网络 API，产品在对局中关闭 Wi-Fi 不影响链路。

**启用。** `//libs/lua:lua_link` 是独立 target，不使用 link 的 image 不链接 BLE
iKCP。Launcher 在 `h2_lua_host_start()` 前调用
`h2_lua_link_enable(host, &(h2_lua_link_config_t){adv_type, scan_type})`，按板级
BLE stack 选择 legacy 或 extended advertising/scan。Extended 广播对 legacy scanner
不可见，所以需要互通的设备必须选同一种 advertising 类型；link 广播只有 21 字节，
legacy 可以放下。ESP 板级 BLE 需要开启 `CONFIG_BT_NIMBLE_EXT_ADV`：host 使用 advertising set
API，NimBLE 在该选项关闭时不提供它，`host()` 会以 `LINK_ERROR "ble"` 结束。Runtime 没有 `ble_host`、
没有 `system_event`、或接入的是 canonical unsupported object 时返回
`H2_PAL_ERR_UNSUPPORTED`，link 保持不可用；start 之后或重复调用返回
`H2_PAL_ERR_INVALID_STATE`。Launcher 负责启动 BLE Host，并保证它存活到
`h2_lua_host_destroy()` 返回。

**GATT profile。** 所有 link 共用一个固定 service，UUID 是库内常量，不可配置：

| 用途 | UUID | 属性 |
| --- | --- | --- |
| Service | `0685b801-18da-449c-88a2-66c491b17772` | primary |
| KCP TX | `0685b802-18da-449c-88a2-66c491b17772` | notify（bleikcp） |
| KCP RX | `0685b803-18da-449c-88a2-66c491b17772` | write / write-no-rsp（bleikcp） |
| Datagram | `0685b804-18da-449c-88a2-66c491b17772` | write-no-rsp / notify |

每次 `host()` 都用同一组 UUID 和同样三个 characteristic 调用
`h2_bleikcp_server_open()`，session 结束时 `h2_bleikcp_server_close()` 调用
`h2_pal_ble_unregister_gatt_service()`，只解绑 link service。对只增不减的 GATT table（ESP NimBLE 最多
4 个 service、每个 3 个 characteristic，unregister 只解绑回调），再次注册已存在的
service UUID 且 characteristic 布局相同时，backend 复用保留的 service slot，重新绑定
回调并写回 handle，因此连续多次 host 始终只占一个 slot；这正是 service UUID 必须固定
的原因。bleikcp server 通过 `extra_characteristics` 把 Datagram 放进同一个 service。`tag` 不进入 GATT：host 广播 session UUID
`0221d1f2-9dce-4921-bac4-eeb9XXXXXXXX`，末 4 字节为 tag 的 FNV-1a hash，join 按它
过滤扫描结果；连接后双方在 KCP 上交换 `HELLO`（版本 + 完整 tag），不符报
`"mismatch"`。ESP/BK7258 上 bleikcp server close 只解绑 link service，
launcher 常驻管理 service 的回调保持有效。只有 provider 返回 `H2_PAL_ERR_UNSUPPORTED`
时才回退到全局 `h2_pal_ble_unregister_gatt_services()`；这类 provider 的共存仍由 launcher 负责。

**Lua API。**

- `link.host({tag=, timeout_ms=})`：开始广播并接受一个 peer；`timeout_ms` 省略或
  为 `0` 时一直广播到 close。
- `link.join({tag=, timeout_ms=})`：扫描并连接同一 tag 的 host；默认 10000 ms，
  上限 60000 ms，覆盖 scan、connect 和握手。`tag` 为 1..32 字节，选项非法时抛出
  Lua argument error。
- 可靠消息 `link.send(msg)`：1..256 字节，走 KCP，有序、不丢，端到端流控；本地
  KCP 发送缓冲放不下整条消息返回 `nil, "link: busy"`。
- 不可靠消息 `link.send_unreliable(msg)`：1..244 字节，直接写 Datagram
  characteristic（host 用 notify，join 用 write-no-rsp），不排队、不重试；超过本连接
  `max_datagram`（ATT MTU − 3）返回 `nil, "link: too large"`，BLE Host 暂时发不出
  返回 `nil, "link: busy"`。接收端事件环满时丢弃。
- 字节流 `link.write(bytes)` 返回本次接收的字节数，发送缓冲满时可能小于长度甚至为
  `0`，App 从该偏移续写；`link.read(max, timeout_ms)` 在 Lua 协程内等待，返回
  1..`max`（上限 4096）字节，超时返回 `""`，断开后先读完剩余字节再返回
  `nil, "link: closed"`。流字节在 KCP 上以 `STREAM` 帧承载，不保留写入边界，与可靠
  消息共享 KCP 顺序。接收缓冲 4096 字节，未读满时 reader 停止，KCP 窗口随之关闭，
  对端 `write` 返回较小值。
- 未连接时 `send`、`send_unreliable`、`write`、`read` 返回
  `nil, "link: not connected"`。
- `link.close()` 幂等、不阻塞；丢弃该 session 未投递的事件和未读的流字节，本地关闭
  不再产生事件。`link.state()` 返回 `"idle"`、`"hosting"`、`"joining"`、
  `"connected"`，未启用时为 `"unavailable"`。
- 进程内同一时间只有一个 session；已有 session 时 `host`/`join` 返回
  `nil, "link: busy"`，已结束的 session 由下一次 `host`/`join` 回收；收到 `LINK_DISCONNECTED` 或
`LINK_ERROR` 之后立即调用不会返回 `busy`。`close()` 或 job 结束之后
  session task 仍在发送 `BYE` 和释放 BLE（通常不超过约 1 s，连接建立中最长为一次
  connect 超时 5 s），期间 `state()` 已为 `"idle"`，但 `host`/`join` 仍返回 `busy`，
  App 应稍后重试。

**事件。** `link.on(kind, fn)` 返回 handle，`link.off(handle)` 或
`runtime.components.off(handle)` 注销；handle 与 `runtime.components.on` 共用
`callback_capacity_per_job`。`kind` 为：

- `runtime.event.LINK_CONNECTED`：`role`（`"host"`/`"join"`）和 `max_datagram`。
- `LINK_MESSAGE`：`data` 和 `reliable`（可靠消息为 `true`，Datagram 为 `false`）。
- `LINK_DISCONNECTED`：`reason` 为 `"peer_closed"` 或 `"lost"`，以及 `result`。
- `LINK_ERROR`：`reason` 为 `"timeout"`、`"not_found"`、`"mismatch"` 或 `"ble"`，
  以及 PAL `result`。

事件表同时带 `event_type`、`sequence`、`timestamp_ms`，`component_id` 与
`component_kind` 为 `0`。回调作为同一 VM 的 scheduler task 运行，受相同 quantum、
取消和超时约束；上一个事件的回调全部结束后才投递下一个，可靠消息因此保持到达顺序。
Datagram 可能先于对端 `HELLO` 到达，握手完成前最多暂存 4 条，排在 `LINK_CONNECTED`
之后投递。没有注册回调的事件被丢弃，App 应在 `host`/`join` 前注册。
`LINK_ERROR` 或 `LINK_DISCONNECTED` 之后 session 结束。

终态事件发布后，`link.read()` 仍先返回断开前已接收的流字节；缓冲耗尽后返回 `nil, "link: closed"`，不依赖后台 session task 是否已经返回。Lua 回调可能先于该 task 的最终退出运行，因此不能用 task 退出状态代替已发布的 session 终态。

**连接与协议。** Join 扫描时每 1.5 s 重启一次 scan：controller 的 duplicate filter
在一次 scan 内对同一地址只上报一次，host 在开始 `host()` 之前已经用同一地址广播其他
内容（例如 H2Loader 管理服务）时，不重启就永远看不到 link 广播。Join 以 30 ms
interval、2000 ms supervision timeout 连接；掉电或离开范围在一个 supervision timeout
内报告 `"lost"`。Host 可以重新协商连接参数：运行 H2Loader BLE 命令服务的 App image
会把每个 peripheral 连接改为 15 ms interval、4000 ms supervision timeout，此时
`"lost"` 约 4 s 后到达。bleikcp 使用 244-byte datagram、16-segment
window、32 帧输入队列和 4096-byte TX/RX buffer，关闭 congestion window。输入队列在 bleikcp worker 一个 slice 内被一个 window 加其重传塞满时只丢帧、由对端 KCP 重传，不会结束 session。KCP 上的帧
为 `[type u8][len u16 big-endian][payload]`：`HELLO`（双方先发，5000 ms 内校验）、
`BYE`（close、job 结束或 Host stop 时发送并最多 flush 400 ms，对端立即报告
`"peer_closed"`；BYE 是有界的尽力而为，预算内未送达时对端报告 `"lost"`）、`MESSAGE`（一条可靠消息）和 `STREAM`（最多 512 字节流数据）。
超长或未知帧按协议错误结束为 `"lost"`。

**线程与回收。** 每个 session 一个 `$lua/link` task 负责建立连接；join 的读循环在该
task 上，host 的读循环在 bleikcp server handler 上。锁顺序为 job mutex → link
mutex，session task 只取 link mutex，并在该 mutex 内、确认未关闭后用
`h2_lua_host_wake_job()` 唤醒 job；Datagram 的 GATT 回调和 system event 只短暂持有
link mutex，从不阻塞 BLE Host。`link.close()`、job 进入终态、`h2_lua_host_stop()`
和 `h2_lua_job_release()` 在持有 job mutex、槽位释放或复用之前请求关闭；session
task 随后发送 `BYE`，停止广播/扫描，关闭 server 或 stream 并断开连接。
`h2_lua_host_destroy()` join session task 并释放 provider 后才返回。

## ESP-Claw profile

兼容库存固定到 ESP-Claw commit `fb7b248114bb1b12ba0fe8e03d4b59bdbec292c1` 的 36 个 module ID。`json` 和 `capability` 为 `full`；`delay`、`system`、`display`、`lcd_touch`、`audio` 和 `storage` 为 `profile`；`button` 为 `component-adapted`，表示物理 constructor 被 Runtime component acquisition 取代、获取后的必需操作保持兼容；其余 module 为 `unavailable`，`require()` 必须确定性失败。`runtime`、`link`、`kv` 等 GizOS 模块不进入该固定兼容库存。

## App 存储

`storage` 让 Lua App 在重启后保留少量数据，例如最高分和设置。Board 或宿主通过 `h2_lua_host_config_t.storage` 提供一个借用的 PAL Filesystem、其命名空间中的 root 目录（例如 ESP LittleFS `data` 分区上的 `/data/lua`）、每个 App 的内容字节配额和文件数上限；字段、默认值和上限以 `h2_lua.h` 中 `h2_lua_storage_config_t` 的 Doxygen 为准。fs 必须提供 `mkdir`、`open`、`read`、`write`、`close`、`stat`、`remove` 和覆盖目标的 `rename`，否则 `h2_lua_host_create()` 返回 `UNSUPPORTED`；root 或上限非法时返回 `INVALID_ARG`。Root 不需要预先存在，首次写入时逐级创建。一个 storage root 同一时间只能由一个 Host 使用，Host 用一个 mutex 串行化所有 job 的存储操作。

每个 `h2_lua_job_submit_*()` 都接收 app id。App id 和文件名都是 `1..32` 字节的 `a-z`、`0-9`、`_`、`-`、`.`，且不能以 `.` 开头，因此绝对路径、`/`、`..`、反斜线、大写字母和隐藏文件都会被拒绝，大小写不敏感的文件系统也不会让两个名字指向同一个文件。非法 app id 使提交返回 `INVALID_ARG`。相同 app id 的 job 共享 `<root>/<app_id>/` 下的文件，不同 app id 互相不可见。

`storage` 始终可以 `require`。Host 未配置 fs 或 job 没有 app id 时，除 `join_path` 外的调用都返回 `nil, "storage: unavailable"`，`exists` 返回 `false`，App 可以继续运行。存储可用时 `exists(name)` 对存在的文件返回 `true`、对不存在的合法名字返回 `false`，非法名字和 I/O 错误返回 `nil, message`。

模块保持 ESP-Claw `storage` 的函数名和成功返回值，差异如下：

- 目录是扁平的。`get_root_dir()` 返回 `""`，因此 `join_path(get_root_dir(), name)` 得到裸文件名；`join_path` 与 ESP-Claw 一样只拼接字符串。没有 `mkdir`；`listdir()` 只接受省略或 `""`，其他合法名字返回 `not found`。
- `stat(name)` 只返回 `type`（固定 `"file"`）和 `size`，`listdir()` 的 entry 另有 `name`，都没有 `mtime` 和 `mode`。
- `get_free_space()` 返回本 App 配额的 `{ total, free, used }`，不是分区容量。
- 失败不抛 Lua error，而是返回 `nil, message`，message 为 `storage: ` 加上 `invalid name`、`not found`、`quota exceeded`、`too many files`、`no space`（文件系统已满）、`busy` 或 `io error`。参数类型错误仍按 Lua 惯例抛错。

`write_file(name, data)` 先检查文件数和配额（替换已有文件只计算新大小），再把内容写入 App 目录中的临时文件，经 `sync`、`close` 后用 `rename` 覆盖目标。任一步失败时目标保持旧内容或不存在，临时文件被删除。

PAL Filesystem 无法列目录，因此每个 App 目录有一个同样原子替换的 `.index` 名字列表：新名字先进入 index 再创建文件，删除时先删文件再更新 index。中断的操作最多留下没有文件的 index 项，下次读取 index 时被清理；未被 index 记录的普通文件不会出现在 `listdir()` 中，也不计入配额；保留的 `.kv` 文件单独统计，共享 App 配额但不展示。存储操作在 owning worker 上同步执行，单次数据量受配额约束，`read_file` 的缓冲区计入 VM 内存上限。

## App KV 存储

`require("kv")` 是 `libs/lua` 的 C 模块，复用 `h2_lua_host_config_t.storage`、job app id 和同一把 Host storage mutex。相同 app id 的 job 共享数据，不同 app id 隔离；没有跨 App 或自定义 namespace 参数。原生 PAL 调用不受此 Lua 层隔离保护。模块名称保留，不能注册同名自定义模块；不改变 ESP-Claw 兼容库存。

| Lua 调用 | 成功或缺失 | 失败 |
| --- | --- | --- |
| `kv.get(key)` | 存在返回 value；缺失返回单个 nil | `nil, err` |
| `kv.set(key, value)` | true | `nil, err` |
| `kv.remove(key)` | 已删除 true；缺失 false | `nil, err` |
| `kv.exists(key)` | true / false | `nil, err` |
| `kv.keys()` | 按 key 字节序升序排列的数组 | `nil, err` |

Key 必须是 string，1..32 字节，仅允许 `a-z`、`0-9`、`_`、`-`、`.`，不能以 `.` 开头，不能包含 NUL，也不把数字隐式转成字符串。最多 `H2_LUA_KV_MAX_KEYS`（128）个 key，更新已有 key 不占新名额。Value 支持 string、boolean、有限 number；保留 integer/float 类型、整数精度与浮点负零。字符串允许空串、UTF-8 和任意二进制字节。Nil、table、function、userdata、thread 不支持；`set(key, nil)` 不表示删除，NaN 和 Infinity 被拒绝。

```lua
local kv = require("kv")
local volume, err = kv.get("volume")
if err then
    print(err)
elseif volume == nil then
    volume = 80
end
local ok, write_err = kv.set("volume", 60)
if not ok then print(write_err) end
```

参数类型错误抛 Lua 参数异常，OOM 沿用 Lua 内存错误。其他错误返回 `nil, "kv: <reason>"`，reason 为 `unavailable`、`invalid key`、`invalid value`（非有限数）、`quota exceeded`、`too many files`、`too many keys`、`no space`、`busy`、`io error`、`corrupt data` 或 `unsupported version`。未配置 storage fs 或 job 无 app id 时，所有有效调用（包括 exists/keys）都报告 unavailable，不把不可用当成 key 缺失；原有 `storage.exists` 行为不变。

### 文件与配额

每 App 保存 `<root>/<app_id>/.kv` 快照，`.kv.tmp` 用于原子替换。公开 storage API 拒绝点开头的文件名，不能直接操作 KV 文件，`storage.listdir()` 也不展示它。`.index` 格式不变，KV 文件通过单独 stat 纳入共享统计：完整编码字节计入 App 配额，存在的快照占一个文件名额，`storage.get_free_space()` 包含 KV 占用。删除最后一个 key 移除快照，释放字节和文件名额。缺失快照是空存储；读取和删除缺失 key 不创建文件。

KV 和普通 storage 写入在同一 mutex 下检查配额和提交。临时文件不计入逻辑配额，但写入需要额外的实际磁盘空间。沿用 storage 的 temp-write、sync、close、rename 保证，掉电持久性取决于 PAL provider。旧固件兼容、降级和历史数据迁移不在 KV 范围内；未知版本仍确定性拒绝，不自动清空或修复数据。

### 执行、锁与内存

操作在 owning worker 同步执行。一个 Host 的所有 App 共用 `storage_mutex`；内部 `_locked` helper 要求调用者持锁，不递归加锁。KV mutation 在一个临界区内读取最新快照、校验、修改、编码、检查共享配额并提交；不同 key 的并发更新不会丢失，同 key 后提交者覆盖。连续 get/set 不组成事务或原子自增。没有跨调用缓存、TTL、批量事务或 clear API。

先短暂持锁查询大小，解锁后分配 Lua userdata 缓冲区，再持锁复查大小并读取最新内容。读操作使用一份快照；mutation 使用旧、新两份缓冲区，顺序扫描记录，不建立 C 对象树。并发增长超过缓冲容量时解锁并重新分配，最多四次尝试，耗尽返回 busy。同尺寸变更仍会重新读取。锁内不调用 Lua、不动态分配；返回值在解锁后构造，OOM 不遗留锁。

快照 userdata 全部计入当前 VM 内存预算，解除引用后由 GC 回收，并非立即释放。重试垃圾、返回字符串/数组和脚本输入也占内存，两份有效快照不是严格峰值上限。不强制全局 GC，也不绕过 VM allocator；磁盘配额足够仍可能 OOM。

### 快照 v1 格式

所有多字节字段为小端序，逐字段编码，不直接落盘 C struct。文件固定开销 20 字节：16 字节 header、记录区、4 字节 checksum。

| Header 字段 | 编码 |
| --- | --- |
| magic | ASCII `GZKV`，4 字节 |
| version | uint16，1 |
| flags | uint16，0 |
| entry_count | uint32，最多 128 |
| payload_length | uint32，记录区总字节数 |

记录按 key 字节序严格升序排列，每条包含 uint8 key_length、uint8 value_type、uint32 value_length、key 原始字节、value 原始字节。类型 1 为字符串原始字节；2 为单字节 boolean（0/1）；3 为 8 字节有符号二进制补码整数；4 为 8 字节 IEEE 754 binary64，保留负零，拒绝非有限数。整数和浮点表示在编译时检查；不允许静默精度截断。

末尾 uint32 为 CRC-32/ISO-HDLC，覆盖 header 和记录区，以小端序保存。多项式 0x04C11DB7（反射形式 0xEDB88320），init/xorout 均为 0xFFFFFFFF，输入/输出反射，`123456789` 的校验值为 0xCBF43926。编码总长为 20 加上每条的 `6 + key_length + value_length`。

截断、溢出、长度不符、非法 key/type/value、重复或乱序 key、非零 flags、尾随字节和 CRC 不符返回 corrupt data；可识别 header 的未知版本返回 unsupported version。损坏和未知版本不得被普通 mutation 覆盖。CRC 检测数据损坏，不代替原子提交或访问控制。

## Source loading and failure

`h2_lua_resource` 默认将 `.lua` 源文件逐字节嵌入 C resource；需要减少固件只读存储占用的 consumer 可以显式设置 `compact = True`。精简发生在构建期，仅删除注释、行首/行尾空白和多余横向空白，不重命名标识符、不改写表达式、不生成 bytecode，也不增加运行时解压或 buffer。短字符串（包括转义）、任意等号层级的长字符串保持原始字节；token 间仍保留必要分隔，字符串之外的换行按 Lua 的 CR/LF 配对规则归一化以保留原源码行号。含 NUL 的输入（包括注释内）、短字符串内未经转义的换行，以及未终止的字符串或长注释在生成阶段报错；完整语法仍由现有 Lua 文本加载器验证。默认生成行为、C symbol 与 Host lifecycle 不变，`source_size` 和 source limit 以实际嵌入文本计。

生成器的 Python 回归覆盖 token 分隔、字符串/注释边界、换行和默认兼容性；`//libs/lua:compact_resource_test` 在实际 VM 中对比原始与精简后的同一 fixture，验证结果和错误行号。业务 consumer 仍需对自己的精简资源执行功能回归和 exact firmware build，不能把构建期节省直接写成 VM 内存或运行时性能收益。

嵌入资源走 `luaL_loadbufferx` 文本加载，不提供 `luaL_loadfilex` 的 shebang 首行跳过行为；精简不会把 `#!` 首行当注释删除，也不会把 `1..2` 等非法数字改写为合法表达式。空源文件或精简后为空的资源使用一个零字节作为 C backing array，逻辑 size 仍为零；非空资源的默认生成内容不变。

所有入口只加载 Lua 文本。绝对路径、空段、`.`/`..`、反斜线、非受限 root、
bytecode、超限或 malformed chunk 都失败关闭。`package.cpath` 为空，
`package.loadlib` 不存在；local `require()` 只能读取当前 Skill root 下的 `.lua`
文本或 immutable compiled resource。

job 只有一个终态：success、failed、cancelled、timed out 或 Host stop 产生的
stopped。事件、callback、job、coroutine 和 capability request 全部有界；full、
unsupported、invalid payload、missing component/capability 和 late completion 都
返回明确错误，不静默成功。取消后的 capability 在 job release 前保留 CLOSED 语义，
release 时归还 bounded request 槽位。

## Job 返回值

`h2_lua_job_get_result(host, id, buffer, capacity, &size, &has_result)` 只在 `SUCCEEDED` 时读取主 chunk 的第一个返回值，忽略后续返回值。转换遵循 `h2_lua_vm_execute_text()`：string 原样保留（包括内嵌 NUL），number 使用 Lua 数字字符串，`nil` 为 `"nil"`，boolean 为 `"true"`/`"false"`；table 使用 `__tostring`，未定义时为 Lua 默认 `table: ...` 表示，其地址部分不稳定，不能用作 序列化协议。无 return 时 `has_result=0, size=0`；空字符串则 `has_result=1, size=0`。 转换异常使 job 失败；转换不支持 yield。

结果保留在主 task 的 Lua 栈上，计入 `vm_memory_limit_bytes` 和 status 的 `memory_used`，有效期到 `h2_lua_job_release()`。超过 Host 的 `output_limit_bytes`（不含终止 NUL）使 job 进入 `FAILED`，message 为 `H2_LUA_VM_OUTPUT_TOO_LARGE`；等于上限允许成功，不静默截断。

调用方拥有 buffer，`size` 返回不含终止 NUL 的完整字节数。`size` 与 `has_result` 输出指针必填。`buffer=NULL, capacity=0` 查询长度并返回 `OK`； 读取有结果的 job 需要 `capacity > size`，否则返回 `H2_PAL_ERR_NO_SPACE`， 仍报告长度和存在标志，buffer 不变。成功复制后补终止 NUL。 非 `SUCCEEDED` 状态（含 failed/cancelled/timed-out/stopped）返回 `H2_PAL_ERR_INVALID_STATE`，未知或已 release 的 id 返回 `H2_PAL_ERR_NOT_FOUND`； 无效参数或 `H2_LUA_JOB_ID_NONE` 返回 `H2_PAL_ERR_INVALID_ARG`。 这些错误将有效的输出长度和存在标志清零。

## Validation

```sh
bazel test //libs/lua:all
bazel test //libs/lua:lua_link_test
bazel query 'somepath(//libs/lua:lua_core, //libs/runtime:runtime)'
rg -n 'h2_runtime_(poll|wait)_event' libs/lua/src
bazel run //projects/e2e/targets/cc_binary/lua-runtime:e2e-lua-runtime
```

query 和 `rg` 都应为空。E2E 的九个固定 case 见 [E2E 测试 App](/apps/e2e)。

单个 Lua 脚本可以用 `//libs/lua/web:lua_web_app.bzl` 的 `h2_lua_web_app()` 直接生成浏览器页面、`:serve` 与
`:browser_test`，不写 C 入口；见 [Web](/apps/web)。

## 借用 Display 与 UI 交接

已有 UI 持有 Display 时，Host 配置的 `borrow_display` 可借用已打开设备。
调用方先暂停其他写屏者，并保证同一时间只有一个使用 Display 的 Lua job。
库仍分配和释放自己的 framebuffer；模块 `deinit`、初始化失败、job release、
取消后 Host stop/join/destroy 都不调用 Display PAL open/close。
调用方必须等待任务释放或 Host 停止、join 和销毁完成后才恢复 UI 写屏。
默认配置保留独立运行时由 Lua 打开和关闭 Display 的行为。

MP4 播放器配置也支持同名选项。启动动画可以借用同一个 Display，阻塞播放
返回后交还 UI；失败和协作取消同样只清理播放器自己的资源。
借用选项不会自动暂停 LVGL，也不提供多个写屏者之间的调度。

## 内置 vmath 与 geometry

所有 Host（Desktop、设备、Wasm/browser）默认提供 `require('vmath')` 和 `require('geometry')`，不需要 App 注册 native module。`vmath` 不覆盖标准 `math`；两者都不依赖 Display 设备。API 的完整参数和边界契约见生产头文件 `libs/lua/include/h2_lua_numeric.h` 和 [Lua 数值 API](../../references/lua-numeric.md)。

`vmath.buffer(count[,kind])` 创建固定容量 userdata，最多 65536 个数值。kind 为 `"f64"`（默认，nil 也使用默认值）或 `"f32"`，分别存储 binary64 或 binary32。存储及等长事务 scratch 都通过 VM allocator 计费，分别为 `16 * count` 或 `8 * count` 字节，加固定 metadata/userdata 开销。索引从 1 开始；点布局为连续 `x,y` 或 `x,y,z`，没有嵌套表。在初始化时分配缓冲区、mesh writer 和 mesh，帧内复用。成功的批量调用不分配；错误消息可以分配。所有数值及结果必须有限且绝对值不超过 1e6，越界、错误类型、无效拓扑或容量不足都会抛出 Lua error，已发布的缓冲区和 mesh 不变。

除下文明确规定的 prepared 位移/系数入口外，同一次调用的所有缓冲区必须使用相同 kind，包括系数、权重、相机、mask、索引、tag 和 mesh topology；混用会在发布结果前抛出 `mixed numeric buffer kinds (f32/f64)`，空前缀也不例外，不做隐式转换。全 f32 调用使用 float 运算和单精度数学函数；Lua 标量先验证有限性及 ±1e6 范围，再在调用入口转换为 float（load 对每个导入元素转换一次）。参数区间按所选精度检查；存储和运算按该精度舍入，极小值可能下溢为零。get/dot 返回普通 Lua number，纯标量 clamp/lerp/smoothstep/spring 及标准 math 保持 double 语义。

数学模块提供标量插值/夹取/弹簧步进，缓冲区线性组合、逐元素乘除、多项式、点积、三维长度/归一化和通道 gather/scatter，以及批量 Verlet、XPBD 距离约束和位移阻尼。物理输入显式传入加速度、逆质量、约束边与 compliance；零逆质量固定节点，时间步范围是 `[1e-6,.1]` 秒。`relax` 每次将 lambda 清零，可选择双向距离或仅张力约束，最多 256 点、512 边和 32 次交替迭代；它不包含碰撞、材质或游戏规则。

`relax_sweep` 用每边的两个权重执行一次正向或反向约束扫描，保留 lambda，允许调用方在扫描间组合额外约束；`damp_edges` 按边顺序更新上一帧位置以衰减分离方向的轴向位移。`map` 提供 abs/sqrt/sin/cos/floor，`select_le` 做逐分量条件选择，`take` 按一基行索引重排固定宽度数据。参数、容量、别名和失败原子性遵守生产公共头。

几何模块提供二维/三维仿射、按权重位移和旋转、位移前缀和、折线展开、轴平面切分与近裁面裁剪投影。相机是 `{fx,fy,cx,cy,near}`，在相机空间沿 +Z 看，投影为 `(cx+fx*x/z, cy+fy*y/z)`，`near >= .001`；可用负 `fy` 翻转屏幕 Y。切分输出 `{side,source_index}`，投影输出源 segment 索引，Lua 可据此决定颜色。游戏公式、镜头参数、材质、颜色和时间步策略仍由 Lua 组合。

`geometry.mesh(vc,pc)` 返回 writer 和现有公共 Display mesh。 `geometry.update_mesh(writer,xy,topology,nv,np)` 将结果直接复制到 mesh，返回同一 mesh；topology 每行是 `{kind,first,count,rgb565}`，kind 0 为 3..128 点多边形， kind 1 为两点线段。xy 和 topology 可以同时使用 f32；writer 和公共 Display mesh 不绑定数值精度。该操作不绘制、不 present；使用现有 Display 批次绘制接口。f64 保留更小的位移，f32 将数据及 scratch 空间减半并使用单精度运算，批量调用消除逐点 Lua/C 边界开销；尚不承诺 S3 帧率或不同平台结果逐位一致，设备侧应按实际点数和迭代数测量。

## 嵌入分层与源码包

`//libs/lua:lua_runtime` 是 portable 下层：包含 Lua Core、Host、modules、`//libs/runtime` 和 PAL headers/inline wrappers，以及 Bazel 固定版本的 Lua 5.5、yyjson。下层的平台访问只依赖 PAL interfaces，不能依赖具体 provider、board、SDK 或 `bleikcp`。`//libs/lua:lua_core` 继续只依赖 upstream Lua。`//libs/lua:lua` 保留现有入口；平台组装由现有 firmware、Desktop、Web launcher 或外部 embedder 完成。可选 `//libs/lua:lua_link` 在上层接入 `bleikcp`，不进入 portable 源码包；未启用时仍遵守既有 `link: unavailable` 合同。

**PAL vtables 就是 embedding hooks。** Embedder 构造既有 `h2_pal_*_api_t` 的 `user + vtable`，填入 Runtime config；不需要另一套 callback ABI。Runtime 初始化要求完整 API surface，不支持的能力使用 canonical unsupported API object。源码包因此同时带上 `//libs/pal:unsupported` 的 portable 实现，供 embedder 填充默认值；这不会为 `lua_runtime` 增加 provider 依赖。Display、Button、Touch、Audio 可以来自宿主 UI，Memory、Task、Queue、Sync、Time、Timer、Filesystem 可以来自已有 provider 或宿主自己的 C 实现。

### 线程与生命周期

Vtable 函数可能从 Runtime input task、Lua worker 或 PAL 自己的线程调用，不保证在 UI/main thread。API 的 `user`、vtable 及其底层资源必须覆盖 Runtime 和 Host 的整个使用期；先停止输入与外部 completion producer，再对 Host 执行 stop/join/destroy，最后 deinit Runtime 并释放 provider。不要从 PAL 回调直接重入同一个 Lua VM。

对于只允许异步通知的 UI bridge，C vtable 应复制像素、rect、音频或其他仅在当前调用期间有效的 buffer 到宿主拥有的有界队列，然后按 PAL 合同及时返回。容量不足时返回对应的错误或 backpressure，不能保留借用指针、等待 UI 回调，或虚报实际未接收的数据。需要同步结果的 Memory、Sync、Queue 等服务必须在 native 层实现；单纯的异步 UI callback 不能满足这些 vtable。原有 PAL 的返回值、timeout、partial-write 和生命周期语义保持不变。

Capability 在 `h2_lua_host_start()` 前注册，start 后 registry 冻结。异步 call callback 复制 input/options 和 request ID 后返回 `H2_PAL_ERR_WOULD_BLOCK`；宿主完成工作时调用 `h2_lua_capability_complete()`，由 owning worker 恢复 Lua。Lua 收到既有的 `ok, output, error` 三元组。宿主必须处理 cancel callback，及时释放其排队工作，并在销毁 Host 前停止所有 completion producer；晚到或重复 completion 会被拒绝。`h2_lua_capability_name_at()` 按注册顺序枚举名字，越界返回 `NULL`；名字借用到 Host 销毁，注册/start/destruction 与枚举之间由调用方串行化。

### 构建与 manifest

在 macOS 或 Linux host 构建源码包：

```sh
bazel build //libs/lua:runtime_sources
python3 libs/lua/tests/test_source_package.py bazel-bin/libs/lua/gizos-lua-runtime-src.tar.gz
bazel test //libs/lua:all
bazel query 'deps(//libs/lua:lua_runtime) union deps(//libs/lua:lua_core)'
bazel query 'filter("//libs/pal/providers/|//libs/bleikcp|//boards/|//native_component_src/", deps(//libs/lua:lua_runtime))'
```

`gizos-lua-runtime-src.tar.gz` 根目录包含 `manifest.json` 和 GizOS `LICENSE`，其余 C/H 文件保持 package-relative 路径；external repository 文件放在 `external/<repository>/` 下。文件清单、include dirs、defines 和各 translation unit 的编译参数由 Bazel aspect 从已配置的依赖图生成，不手工复制维护。包使用与 GizOS native build 相同的 Lua source selection 和受限标准库；固件专用 stdio/newlib shim 仍由原 embedded build 配置选择，不把 ESP libc 兼容代码加入 native host。它不包含预编译库、Bazel toolchain、PAL provider 或 board code。

Manifest schema version 1：

| 字段 | 合同 |
| --- | --- |
| `schema_version` | 整数 `1`；consumer 拒绝未知版本 |
| `gizos_commit` | 源码 revision；开发包为 `@GIZOS_COMMIT@`，发布方必须替换为实际打包源码的 commit |
| `runtime_profile_id` | 固定为 `runtime.lua.gizos` |
| `sources` | 所有需编译一次的 `.c`，相对解包根目录 |
| `include_dirs` | 相对解包根目录的 include search paths |
| `defines` | 公共 compiler definitions，不含 `-D` 前缀 |
| `cflags` | 公共 C compiler arguments，JSON array 中每项为独立参数 |
| `compilation_units` | 分组的 `sources`、附加 `cflags`、附加 `defines`；各 source 恰好属于一个分组 |
| `per_os` | OS 名到附加 `link_flags` 的映射；`linux`、`darwin`、`android`、`ios` 的数学库为 `-lm` |

Consumer 对每个 source 应用公共参数及所属分组参数，按自身目标工具链追加 architecture、sysroot、PIC、visibility 和 deployment target，再链接所有 object 与该 OS 的 extras。参数必须逐项传给 compiler，不通过 shell 拼接解析。当前包表达 GCC/Clang C11 编译合同；Windows/MSVC 和 WebAssembly 工具链需单独适配，不由该 manifest 声明支持。Profile ID 标识 Lua surface，不替代 commit pin；不同 revision 的源码、header 和 binding 不应混用。

独立测试只依赖 Python 3.11.8+（支持 tar extraction filter）、`cc`/Clang 和 pthread。它在临时目录解包，根据 manifest 编译全部 C sources，链接测试自己填写的 OS、240×240 Display、Touch、`ok`/`back` Button vtables。测试验证 async echo、显示 dirty rect 与全部像素、Runtime push edge 到 Lua callback、pending job cancellation、start 后拒绝注册和 capability 名称枚举。Python runner 不调用 Bazel，也不从 checkout 查找 runtime source；`CC` 可指定兼容 compiler。Bazel test 只是把源码包和 harness 作为测试输入交给同一个 runner。

Embedder 执行 App method 时，让主 chunk `return app[method](...)`，等待 job 成功后查询长度、分配宿主 buffer、调用 `h2_lua_job_get_result()` 复制结果，最后 release。Flutter/cgo 都通过同一公开 accessor 读取，不需要私有 native module 转存返回值；复杂值应由 App 显式编码为 JSON 等稳定格式。

### Flutter 与 cgo

Flutter package 将解包后的源码和 manifest 随包分发，由 native assets build hook 读取 manifest，用 Flutter 选择的每个目标 C toolchain 编译各 translation unit 并链接 native asset；不能依赖 GizOS 的 Bazel archive 或预编译 library。通过 `dart:ffi` 调用现有 Host/Runtime API，native bridge 拥有 PAL objects 和所需的同步 OS 服务；UI 操作通过复制后的消息交给 Dart，再由 Dart 渲染。FFI binding 必须匹配随包 header 的 struct layout 与 callback signatures。

Go/cgo consumer 同样在自己的构建步骤中读取 manifest，用目标 C compiler 编译包内 sources 与自有 PAL bridge，再把 object/archive 接入 cgo linker。cgo 不会递归编译这些子目录中的 C 文件，也不能忽略不同 source group 的 flags。Go 层通过 C bridge 发起 job、推送输入与完成 capability；PAL `user` 可由 C 分配的 context 或受管理的 opaque handle 表示，不能把生命周期不受控的 Go 指针留给 worker。宿主的 pthread、UI framework 等依赖由上层 bridge 自己声明，不属于 portable runtime manifest。

### Prepared 数值阶段与共享几何

workspace 默认通过 `load` 复制状态；可分发 Lua app 也可以创建标准 f64 numeric buffer，通过 `bind` 明确保留当前与历史位置。绑定后，公共 buffer 写入与 `node` 更新相互可见，绘制直接消费当前位置，无需每子步完整导出。每个 buffer 只能被一个活 workspace 绑定；workspace 强引用保活 buffer，弱 owner 记录不阻止 workspace 回收。重新绑定重置活动拓扑、lambda、span 与 sweep；成功 `load` 安全复制后退出绑定，失败保留旧状态。各阶段继续使用私有暂存区和原子提交；绑定状态不能同时作为阶段系数、bounds、mobility 或 `copy` 输出。`multipliers` 只返回选定边与 span 的数学结果，张力、出线与游戏规则仍由 Lua 解释。

`vmath.constraints` 提供固定容量、VM 计费的阶段 workspace。`load/begin` 明确重置 lambda 与交替 sweep 次序；`integrate` 执行调用方提供的两级 gain 和增量，`node/edge/span` 只读取或更新明确的状态，`solve` 每轮依次执行边、span、位置 bounds，`damp` 执行邻居位移和有序轴向阻尼。Lua 在 integration 之后执行依赖端点的应用规则。每个阶段独立原子提交，失败不撤销上一成功阶段，也不清空已有 lambda。完整参数、范围、两浮点补偿与 float 位移规则以 [numeric production header 生成的 API](../../references/lua.md) 为准。

`geometry.rotations` 只保存调用方给出的段、权重和旋转轴；端点 reduction 在明确误差域内使用 18 moments/17 次多项式，域外执行完整循环。它不生成形状、权重函数或受力模型。`geometry.batch` 保存不可变二维顶点、拓扑和两个可选位移权重通道；`geometry.pose` 保存按原顺序计算的变形、旋转、缩放、平移和可选参数化平面透视结果。缓存键含全部 evaluate 输入与不可变 geometry，位于 layer/color 之前，应用自行决定共享时机。

`display.draw_pose` 直接消费 pose，通过既有 polygon/line raster 绘制。`display.polyline` / `compile_line_style` / `draw_polyline` 保留源段身份、方向、共享端点投影、严格异号切分及近裁剪。颜色在原始端点求值，先 RGB888 插值和 floor，再转 RGB565；触平面及共面段用调用方显式提供的 boundary style。点、camera、plane、order 或 style 改动会重新准备 transient fragments，不保留可过期的跨 draw 投影缓存。各 draw 的 clip、layer、颜色只影响本次重绘，不使已发布的 pre-layer pose 失效。完整绘制参数与相机布局见 [Display API](../../references/lua.md)。

这些对象及 scratch 均由 VM userdata 持有，成功暖调用不分配或逐元素回调 Lua；构造失败与 GC/VM teardown 回收所有引用。绘制在参数解析后重新检查 Display acquisition，完整验证最终坐标和样式后才写像素，并沿用 dirty/background bookkeeping；不会隐式 present。游戏状态、标量受力方程、材质转换、形状通道生成、相机 recipe、层语义和采样时钟仍属于可分发 Lua app。公共功能只做原始算法拆分；实机逐阶段帧率对齐属于下游成对验证，Host/Web 测试不能代替。

Prepared workspace 的 `displacements` 将指定范围的 double 位置差在相减后转成 f32，写入可复用的 packed xyz 输出前缀。`displacement-f32` 积分的 before/gain0/gain1 可分别使用 f32 或 f64，mobility/after/bounds 保持 f64；环境分支及系数公式仍由 Lua 决定。显式 `vmath.length3_refined` 使用原版 float 开方种子与一次 double 修正，适用范围、误差与 fallback 见 numeric Public Header；不改变原有 `length3` 或 `normalize3`。
