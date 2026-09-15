# 2D 骨骼动画

`libs/skeleton2d` 提供不依赖 Lua、Display、Runtime 或 SDK 的骨架计算。调用方定义父子拓扑、局部 TRS、动作通道和部件层级；库执行采样、双路混合、世界仿射和稳定绘制排序。具体角色资源、行为、朝向状态和遮挡策略属于应用。公共类型、容量和错误合同以 [API Reference](/references/skeleton2d) 的生产头为准。

## 组合与内存

C 调用方先查询 definition/instance storage 字节数，再提供满足 `max_align_t` 对齐的独立存储。Definition 深拷贝骨架、部件和动作数组，可以被多个实例共享；实例借用 definition，销毁顺序为实例在前。没有 allocator、线程或隐式时钟。大小查询失败清零输出；姿态更新和求值失败保留已发布内容。

Lua 的 `require('skeleton2d')` 在所有 Host 内置，不能被自定义模块覆盖。Definition、actor、writer、几何和事务 scratch 均由 VM 计费。初始化时创建资源和 buffer，帧内复用。`definition:bytes()`、`actor:bytes()` 返回其 userdata 字节需求；`writer:bytes()` 返回 writer 的 native 数据字节数，不含 Lua table/userdata header 和 Display mesh 内部存储。总体峰值须从 VM allocator 和 Display/PAL 实际计费取数，不能把这三个 accessor 之和当作整个游戏的 RAM。

数据流为：应用定义 → C 采样/混合 → 可选局部覆盖 → 世界矩阵/有序部件 → Lua writer 转换几何 → 现有 `display.draw_mesh` → `display.present`。Writer 持有返回 mesh 的强引用，复制资源数据并从变换顶点计算 bounds；C core 不持有资源形状或第二份 AABB。Lua numeric buffer 的内部复用仅发生在 `libs/lua` 内，不向 C core 或应用泄漏私有结构。

## 动画与朝向

局部矩阵为 T×R×S，子节点使用 parent_world×local。矩阵使用 Display 的六元素布局；不能直接传给行布局的 `geometry.affine2`。非均匀缩放导致的 shear 保留在世界仿射中。零缩放和二维镜像合法；镜像过零的混合会压扁，不希望压扁时由应用离散切换。

时间使用整数微秒。Clamp 包含 duration 端点，repeat 为 `[0,duration)`，负时间使用非负余数。轨道严格按 key 时间排序；缺失通道回默认姿态，轨道范围外保持端值。Step 在命中 key 时切换；linear 支持 shortest-angle 或显式多圈 unwrapped。两路混合角度采用 `[-π,π)` 的最短差，weight 0/1 精确复现对应输入。层级、资源和 visible 不参与数值混合，应用成批设置它们。

正/侧/背造型需要不同资源与层级输入，不是 2D 旋转能够生成的三维视图。纹理附件通过下述 Display 适配器绘制；不包含多骨蒙皮、IK、动画事件或行为状态机。连接处采用应用提供的重叠端盖/覆盖片；库不保证任意资源自动无缝。

## 绘制与像素

Writer 合并可见部件为一个固定容量 mesh，更新前检查全部变换、资源 ID 和容量，成功后发布 mesh/bounds。`display.draw_mesh` 使用 identity 和世界顶点；动态 mesh 默认关闭 span cache。不变的部件矩阵/资源/顺序跳过 mesh 更新；调用方只能绘制返回的 mesh，不得通过其他 producer 改写它，否则会破坏 writer 的缓存 ownership。层级相同按 part ID 排序，不根据骨架深度猜遮挡。

`grid=0..16` 只在最终绘制时吸附顶点，不是完整粗像素离屏渲染。默认 grid=0，骨架计算保留精度。应用若实现局部重绘，脏区应包含旧/新 bounds 并增加 grid+1 px 边距；涉及多个角色时恢复背景后按顺序重绘相交角色，避免后一个角色恢复背景擦除前一个。示例使用完整背景清除，明确测量该路径成本。帧中途的 Display/PAL 失败不承诺屏幕回滚。

## 本地验证

```sh
bazel test --config=macos_arm64 //libs/skeleton2d:skeleton2d_test //libs/lua:lua_skeleton2d_test
bazel test --config=macos_arm64 //libs/lua:lua_test //libs/lua:source_package_test
bazel test //projects/example/targets/pkg_tar/lua_skeleton2d:browser_test //projects/example/targets/pkg_tar/lua_skeleton2d:facing_browser_test //projects/example/targets/pkg_tar/lua_skeleton2d:landmark_browser_test //projects/example/targets/pkg_tar/lua_skeleton2d:controls_browser_test
bazel run //projects/example/targets/pkg_tar/lua_skeleton2d:serve
bazel run --config=macos_arm64 -c opt //libs/skeleton2d:benchmark_skeleton2d
```

浏览器入口由 portable App 脚本和共享 Lua Web launcher 组成，机械臂、人形行走、链条和小狗小跑调用同一 C 引擎。所有角色拓扑、外观与动作均是 App 自有 fixture；公共库没有人形、四足或其他物种预设。页面启动后暂停在固定姿态，Canvas 下方支持 PLAY/PAUSE、前后逐帧、场景、DEBUG、grid、混合、速度、层级及 BENCH；时间轴可 seek。

机械臂和链条直接使用 skeleton2d writer 验证二维渲染。人形和小狗的空间预览读取同一真实 actor 的世界矩阵，把 App 定义的部件局部坐标和深度组合为空间点；随后调用现有 `geometry.affine3` 进行角色 yaw 和相机 pitch 变换，提取屏幕坐标并保留深度，通过 `geometry.mesh/update_mesh` 绘制。该路径不反解二维局部 TRS，也不提供公共三维骨架接口。CHECK 会另外调用 skeleton2d writer 验证两类角色的二维路径，空间预览不能代替 writer 合同测试。

ROTATE 每次改变角色朝向 45 度，覆盖侧面、正面、另一侧、背面与斜向；CAMERA 切换水平和 45 度俯视。机械臂或链条上使用这些控件会进入人形场景。角色朝向与相机俯角互相独立，改变视角保留动作时间和混合权重，复用初始化时的 definition、actor、空间资源及 buffer/mesh，不重新编译动作或主动 GC。MIRROR、ZOOM、PAN、RESET 控制屏幕镜像、缩放、平移和复位。

空间几何包含顶面和底面；App 根据投影面的朝向和当前相机深度处理可见性与绘制顺序，深度相同按稳定 face ID 排序。它是针对示例部件的正交展示，不承诺任意相交网格的正确遮挡。DEBUG 使用同一相机变换显示骨点、骨线、可见部件 AABB 和该部件最后一个绘制面的序号；二维路径显示 skeleton2d draw item 顺序。CHECK 使用独立的闭式关节坐标 oracle 验证人形/小狗、六个动作时刻、八个朝向和两个俯角的 192 组组合。测试 JS 仅操作控件和读取原始 Canvas，验证顶面与正背面躯干像素。


Lua 示例源码由 `projects/example/apps/lua_skeleton2d/app/src/skeleton2d.lua` 拥有，通过 `app/BUILD.bazel` 导出给现有 Lua Host launcher。`targets/pkg_tar/lua_skeleton2d` 只负责 Web 产物与验证接线；其他 launcher 可以消费同一个导出脚本，不需要增加 C App 包装层。

## 性能测量

C benchmark 对 16/32/64/128 bones 分别预热 300 次、采样 3000 次、重复三轮，测双采样、混合、世界矩阵和排序。Web BENCH 对 16/24/192/1、32/48/384/2、64/96/768/4 的 bones/parts/vertices/actors 测量；前三项是每个 actor 的资源数量，整场景总 bones 为 16/64/256、总 vertices 为 192/768/3072，每部件两个 primitive。所有阶段耗时均覆盖整场景。所有阶段采样使用 `system.micros()`：计算包括应用 part-state 写入，几何包括 bounds/mesh 复制，raster 包括对应活动区域的背景恢复，present 为当前 PAL 接收语义。输出独立 p95、端到端 p50/p95/p99/max、超 33.33 ms 帧数和平均提交像素/矩形。

二维基准覆盖 100% 和 25% 活动区域（背景恢复及绘制裁剪范围）、静止/动画、无混合/双路混合、固定/变化层级，背景恢复成本另列 `recovery_p95_us`。活动区域比例不代表角色多边形实际填充比例；以日志中的提交像素量衡量实际 Display 工作。

BENCH 在固定规模二维基准后，分别测人形/小狗在 yaw 0/45 度与 pitch 0/45 度的四种组合，每组合预热 300 帧、记录 3000 帧、重复三轮；另测固定姿态下的视角切换耗时。日志分开报告 SKELETON BENCH 和 SKELETON SPATIAL，VM 快照不是 allocator 峰值，也不能当作设备 RAM 需求。

Host/WASM 结果不能推断 ESP32-S3 或 BK7258 的帧率、屏幕完成时间或 RAM 余量。首档待设备验证目标为 30 FPS：计算/几何 p95 各 1.5 ms、raster 8 ms、present 12 ms、端到端 p95 33.33 ms，超时帧比例低于 1%。计时分辨率不足时不判定子阶段目标。真机报告须记录芯片/核心/频率/编译器/优化、屏幕总线、分配峰值、栈高水位、负载和完成信号；尚未授权设备测试时明确 SKIP。

### Host 测量记录

2026-09-14 在 Apple M2 Ultra、64 GiB、macOS 26.5.2 上测量提交 `5b07a759` 的生产代码。WASM 使用 `--config=macos_arm64 -c opt`（实际 `-O2`）和仓库 Chromium harness。每配置 300 帧预热、3000 帧采样、重复三轮；下表为三轮 p95 的最大值，空间场景同时取四种 yaw/pitch 组合中的最大值。

| 整场景工作负载 | 端到端 p95 | 视角切换 p95 |
| --- | ---: | ---: |
| 第一档，1 actor，动态双混合及变化层级 | 0.300 ms | 不适用 |
| 第二档，2 actors，动态双混合及变化层级 | 0.400 ms | 不适用 |
| 第三档，4 actors，动态双混合及变化层级 | 5.200 ms | 不适用 |
| 人形，14 bones / 456 spatial vertices / 114 faces | 5.900 ms | 5.500 ms |
| 小狗，15 bones / 504 spatial vertices / 126 faces | 6.200 ms | 5.800 ms |

完整八种二维配置加八种角色/视角配置，共 144000 个记录帧中，没有超过 33.33 ms 的帧。全矩阵最大单帧为 10.100 ms。浏览器时钟大量量化到约 100 微秒，不能把子阶段记录的 0 当作零成本；Host 调度使 p95 与 p50 存在明显差距。该数据包含实际 raster 和 PAL 接收，不包含物理屏幕完成信号。

小型 raw Lua 合同 fixture 的 VM 分配峰值为 53740 B；成功的采样、混合、局部覆盖、求值及 writer 更新在预热后连续 10000 帧分配次数为 0，关闭 VM 后计费为 0。它不代表完整示例或设备总 RAM。完整 CSV、阶段分位数、内存范围及测量源码摘要随性能报告提供；ESP32-S3/BK7258 的预算继续保持未验证。

## 仿射纹理附件

纹理采样与 RGBA→RGB565 混合由 `libs/raster2d` 统一实现，Lua Display 管理纹理、引用和计费；Skeleton2D Lua adapter 只把已发布的资源 ID 和矩阵映射到固定容量批次。C core 不持有图片、几何或 AABB。

```lua
local display, sk = require('display'), require('skeleton2d')
-- rgba 是应用加载的原始 RGBA8 字节串；PNG 等格式需由资源流程解码。
local texture = display.texture(32, 32, rgba)
local writer, batch = sk.textures(definition, {
  {texture=texture, x=0, y=0, width=32, height=32, anchor_x=16, anchor_y=24},
})
local root = {1, 0, 0, 1, 120, 120}
-- 帧内复用 definition、actor、writer、batch 和 root。
actor:sample(1, time_us, 'repeat')
actor:evaluate(root)
sk.update_textures(writer, actor)
display.draw_textures(batch)
display.present()
```

图集坐标以纹理左上角为原点，锚点相对于选定区域的左上角。纹理格中心在 `(x+0.5,y+0.5)`；目标像素中心逆变换到局部区域后取 floor，源区域采用半开区间。整数 identity/平移逐格对齐，支持旋转、非均匀缩放、shear、镜像；行列式恰为零时不画，非零但逆矩阵非有限时报错。最近邻不提供抗锯齿。透明源不改背景，半透明在 RGB565 通道内 source-over；RGB8 截断到 5/6/5 位，不能承诺转换后与 RGBA 字节相同。

Display 复制 RGBA 字符串及附件描述，批次强引用纹理；writer 强引用 definition 和批次。热更新先校验全批再发布，失败保留旧批次。返回批次仅供绘制，不能手工更新。完整源纹理约占 `4*w*h` 字节，批次含资源描述及两份容量数组；Lua header、引用表、初始化字符串、帧缓冲和 PAL 存储另计，预算以 allocator 实测峰值为准。当前 damage 保守标记整个 clip，透明或零缩放也可能导致多余提交；局部重绘由应用指定 clip 并恢复旧背景。

公共用例 `//projects/example/targets/pkg_tar/lua_texture2d:browser_test` 读取真实 WASM Canvas，对照 80 组独立/骨骼绘制结果。纹理性能由该用例单独测量，不能使用上方旧 mesh 数据。MCU 性能未实测，状态为 **SKIP**。资源拆分、关节覆盖、朝向贴图和关键帧一致性仍由应用验证，静态整角色贴图回放不能作为骨骼动画完成证据。

### 纹理验证记录（2026-09-15）

本次测量使用 Apple M2、16 GiB、macOS 26.6.2，仓库 WASM/Chromium harness，`-c opt --copt=-UNDEBUG`。以下数字只覆盖通用测试纹理，不能代替私有角色验收。生产源码摘要为 `304909b75817cfd18305d5a86910e6fd736eea077d793d7ddede965c7d79eebe`（按路径排序，对 Raster2D C/头、Lua texture C/私有头、skeleton adapter C 和 modules C 的 `路径+NUL+内容` 连接计算 SHA-256）。

每个工作负载为 1 bone、1 或 16 个附件，共享一张 32×32 RGBA8 纹理（4096 B），包含不透明、半透明及全透明像素。附件有重叠，骨骼角度持续采样；edge=120/240 是屏幕左上 25%/100% 的恢复及绘制 clip。每组预热 300 帧、记录 3000 帧、重复三轮，共 36000 个记录帧。表中取三轮 p95 的最大值，单位 μs。

| 附件数 | clip edge | 恢复背景 | 纹理 raster | present | 端到端 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 120 | 低于时钟分辨率 | 100 | 101 | 200 |
| 1 | 240 | 100 | 100 | 201 | 299 |
| 16 | 120 | 低于时钟分辨率 | 101 | 101 | 200 |
| 16 | 240 | 100 | 200 | 201 | 301 |

采样/求值和适配分别计时，但 p95 低于浏览器约 100 μs 的有效分辨率，不能记为零成本。所有记录帧中没有超过 33.33 ms，最大端到端值为 6600 μs；该轮与其他本地构建检查并行，调度尾延迟不等于渲染器独占成本。每帧提交 1 个矩形，edge=120/240 分别提交 14400/57600 像素。当前 conservative damage 的成本已包括在内；present 仅是 PAL 接收，不是物理屏幕完成。

raw Lua allocator 从 VM 创建计量到关闭：含原 mesh fixture 和新增 32×32 纹理、两附件 writer，峰值 62471 B，预热后的 10000 帧更新分配次数为 0，关闭后计费为 0。真实 Host 的 32×32/16 附件初始化阶段测得 VM 峰值 76065 B（包含当时已存在的合同测试资源和输入字符串）；该 fixture 的 10000 帧 update/draw 也通过零分配断言，但绘制 clip 为 8×8。Web benchmark 的约 267 KiB VM 快照包含采样数组，**不是 allocator 峰值**。这些数值均不包含完整 PAL/framebuffer/线程栈，不能直接作 MCU RAM 预算。

最终软件验证：16 项优化测试通过，覆盖 C texture oracle、原 Raster2D/Skeleton2D、Lua Host/raw VM、source package、原 vector/skeleton Web 和新 texture Web。C 独立前向纹理格 oracle 覆盖 44 组变换/锚点/clip 组合；另有透明、越界/溢出/奇异输入、整批失败及 guard 检查。ASan/UBSan 的 C 测试通过。Web 对整个 Canvas 逐像素对照 80 组独立/骨骼纹理结果及失败后旧批次内容。

```sh
bazel test --config=macos_arm64 \
  --repo_env=DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  -c opt --copt=-UNDEBUG \
  //libs/lua:all //libs/raster2d:all //libs/skeleton2d:all \
  //projects/example/targets/pkg_tar/lua_texture2d:browser_test \
  //projects/example/targets/pkg_tar/lua-script-vector:browser_test \
  //projects/example/targets/pkg_tar/lua_skeleton2d:browser_test
```

完整图也已执行，但未全部通过：macOS 全图测试有 481 项通过、39 项跳过，纹理迭代期间的两个失败由上述最终回归覆盖；文档因缺少 Doxygen/Docker 无法生成。macOS 全图 build 被旧示例所需的 `macosx10.11` SDK 阻塞，ESP32-S3 全图分析缺少 Pion 的 C++ toolchain；BK 全图编译器为 x86_64，在本机处于持续等待后中止。不能将这些结果写成全仓 green。ESP32-S3/BK7258 实机性能、RAM、屏幕完成与栈高水位均为 **SKIP**。

自审结论：C core 无渲染依赖变化；RGBA/引用/事务 scratch 均由 Display owner 管理，adapter 不复制采样器；非法批次在写入前完整校验，旧批次保留；typed 尾部数组保证 32 位/WASM 的矩阵对齐；资源 GC 和关闭归零有真实计费验证。迭代发现并修复了 32 位对齐、过严的极小行列式校验，以及测试计费钩子对 Lua 5.5 长字符串 allocator 生命周期的误用。修复后重新审阅，没有遗留的已知功能性负面发现；上面的工具链/设备验证限制仍成立。


### CI 基准分组

`//projects/example/targets/pkg_tar/lua_skeleton2d:benchmark_browser_test` 汇总九个独立浏览器测试。App 的可选启动参数 `benchmark_group` 为 1 时执行八种二维配置（72000 个记录帧），为 2..9 时依次执行 human/dog 的四种视角，每组 9000 个记录帧。未设置或为 0 时，交互 BENCH 仍完整执行所有组。每个配置保持三轮，每轮 300 帧预热、3000 帧记录，所有帧仍调用生产骨骼求值、几何更新、真实 raster 和 present；空间视角切换测量也保持不变。

拆分依据是 PR #380 提交 `331afab3` 的 macOS fastbuild CI：二维组已完成，空间组仍在推进时，单次 harness 达到 2400 秒上限。各组现在独立拥有 2400 秒运行预算，避免所有组共享一次超时；不减少总计 144000 个记录帧，也不取消任何平台验收。各浏览器独立初始化，因此跨组 VM/GC 历史不同；并行 CI 的时间数据不能直接用于替换前述受控运行的性能测量。

本次分组变更在 Apple M2 本地以 `-c opt --copt=-UNDEBUG` 通过该目标子树的全部 23 项浏览器测试；九组日志包含全部 48 个配置/轮次结果，即 144000 个记录帧。该验证不代表 macOS CI fastbuild 已通过，远端结果须按最新提交确认。
