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

`h2_lua_host_config_t` 的容量均有界：`worker_count`、`worker_stack_size`、`max_jobs`、`max_coroutines_per_vm`、`ready_queue_capacity`、`waiter_capacity`、`event_delivery_capacity`、`callback_capacity_per_job`、`audio_track_capacity_per_job`、`pending_capability_capacity`、`instruction_quantum`、`resume_time_budget_ms`、`source_limit_bytes`、`output_limit_bytes` 和 `vm_memory_limit_bytes`。零使用声明的默认值；ready/waiter 容量不得小于 VM 的 coroutine 上限。`storage` 配置每个 App 的持久化存储，见 [App 存储](#app-存储)；全零表示未配置。

Host 的正常生命周期是：

1. `h2_lua_host_create()` 借用 Runtime 并分配固定容量；
2. 在 start 前注册 native module 和 capability；
3. `h2_lua_host_start()` 冻结 registry 并创建 worker；
4. 通过 text、compiled resource 或 Runtime Filesystem 提交 job，同时给出决定 `storage` 作用域的 app id（可为 `NULL`）；
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
| `display` | `clear`、矩形、线、圆、AA circle、圆角矩形、三角形、多边形、椭圆、保留命令、framebuffer fade、frame、text、`present` 和 `deinit` | 直接使用 Runtime singleton Display API；dirty region 始终裁剪到 framebuffer |
| `lcd_touch` | `read`、`poll`、`sync` 及 upstream touch result fields | 直接使用 Runtime singleton Touch API，不接收 SDK handle |
| Button proxy | `get_key_level` | Runtime normalized Button snapshot，不创建 GPIO button |
| `storage` | `get_root_dir`、`join_path`、`exists`、`stat`、`read_file`、`write_file`、`listdir`、`remove`、`rename`、`get_free_space` | Host 配置的 PAL Filesystem；每个 app id 一个扁平目录，受配额和文件数限制，写入原子替换 |
| `audio` | `new_output`（每条 Track 的 `write/info/close`）、`new_input`（`read/level/info/close`） | 直接使用 Runtime singleton Audio System；Track frame 大小取自设备 playback format，Input frame 大小取自设备 mic format；PAL 混合多条 Track，不接收 codec handle |
| `link` | `available`、`host`、`join`、`send`、`send_unreliable`、`write`、`read`、`close`、`state`、`on`、`off` | Launcher 调用 `h2_lua_link_enable()` 后由 `//libs/lua:lua_link` 经 BLE Host PAL（不可靠消息）与 `libs/bleikcp`（可靠消息、字节流）提供；未启用或没有 BLE 时 `available()` 为 `false`，操作返回 `nil, "link: unavailable"` |

`link` 与 `runtime` 同属 GizOS 新增 module，不在 ESP-Claw 兼容库存内。

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

### Display 笔画与有界缓存

`display.stroke_path(points,widths,color,offset_x=0,top=0,bottom=height,cache=false,fast=false,smooth=false,scale=1,tolerance=0)` 接受 `2..256` 个有限 ±100000 的点对、恰好 `n-1` 个 `0..1000` 宽度，以及单色或 `n-1` 个颜色。cache/fast/smooth 必须为 boolean；scale 为 `0<scale<=16`，先作用于坐标和宽度；offset 有限且位于 ±100000。top/bottom 沿用屏内整数半开行裁剪。所有参数、颜色 getter 和分配在绘制前完成并重新检查 Display。返回 `(cache_hit,fast_segment_count)`，不是帧率。

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
| `grid` | 整数 0..16，默认 0，不吸附；非零时用 `floor(value/grid+0.5)*grid` 吸附变换后的顶点，与游戏尺寸无关 |
| `offset_x` | 有限且在 ±100000 内，默认 0；多边形在交点取整后横移，线段在连续裁剪前横移，不与 matrix 平移合并 |
| `left,top,right,bottom` | 默认整个 framebuffer 的整数半开裁剪矩形，范围必须完全位于 framebuffer 内；空矩形合法 |
| `color` | 可选 Display 颜色覆盖，不修改保留颜色 |
| `cache` | boolean，默认 false；按需保留最多 8192 条有序扫描段/线记录 |

所有派生顶点在光栅化前验证为有限且在 ±16000000 内。多边形沿用上述 even-odd 扫描和交点取整规则；线先连续裁剪再按 `floor(endpoint+0.5)` 取整并执行 Bresenham。绘制不隐式 present，关闭 Display 后拒绝绘制。派生顶点缓存以内容更新、matrix 和 grid 为失效条件；裁剪、颜色、offset 每次绘制应用，不能因坐标缓存命中而跳过。可选 span 缓存还比较裁剪、viewport、offset 和 recolor，命中时按原顺序重放并标记 dirty/background damage；容量溢出仍完整绘制，但不发布部分缓存。成功的 native/Lua 更新同时使两类缓存失效。数据和缓存计入 VM；引用释放后由 GC 或 VM teardown 回收。

精确单位矩阵且 `grid=0` 时，创建／更新阶段已验证的顶点直接复制到既有派生坐标缓存，不逐点重复计算单位变换，也不增加缓存容量或分配。此快路径不使用近似比较；非单位矩阵或非零 grid 仍在修改坐标缓存和像素前完整验证变换结果，错误、像素和缓存失效合同不变。

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
window、32 帧输入队列和 4096-byte TX/RX buffer，关闭 congestion window。KCP 上的帧
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

兼容库存固定到 ESP-Claw commit `fb7b248114bb1b12ba0fe8e03d4b59bdbec292c1` 的 36 个 module ID。`json` 和 `capability` 为 `full`；`delay`、`system`、`display`、`lcd_touch`、`audio` 和 `storage` 为 `profile`；`button` 为 `component-adapted`，表示物理 constructor 被 Runtime component acquisition 取代、获取后的必需操作保持兼容；其余 module 为 `unavailable`，`require()` 必须确定性失败。`runtime` 是本 Feature 唯一新增的 GizOS Lua module。

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

PAL Filesystem 无法列目录，因此每个 App 目录有一个同样原子替换的 `.index` 名字列表：新名字先进入 index 再创建文件，删除时先删文件再更新 index。中断的操作最多留下没有文件的 index 项，下次读取 index 时被清理；未被 index 记录的文件不会出现在 `listdir()` 中，也不计入配额。存储操作在 owning worker 上同步执行，单次数据量受配额约束，`read_file` 的缓冲区计入 VM 内存上限。

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
