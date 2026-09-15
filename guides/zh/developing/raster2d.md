# Raster2D

`libs/raster2d` 提供 RGB565 调色板插值、轴对齐矩形重放及 RGBA8 仿射纹理批量采样。普通 C/C++ App 和 Lua Display adapter 调用同一份 C 实现；核心不依赖 Lua、Runtime、Display provider 或具体 board。

## API 与存储

生产声明、字段和错误合同来自 [Raster2D API](../../references/raster2d.md)。调用方提供 surface、矩形和调色板数组；surface 的容量与 stride 均以像素计，调色板容量以条目计。核心不分配内存、不保留指针、不同步线程，也不提交屏幕。调用方串行化同一输出的写入，并保证输入和描述符在调用期间有效且不变。

矩形使用整数位置、无符号尺寸和从零开始的 palette index，按输入顺序覆盖。矩形与显式 clip 都使用半开区间；clip 必须完全位于 surface 内。完整批次在写入前校验，包括屏外或零面积矩形的颜色索引。裁剪后的逐行填充不重复执行参数校验。空输入不写像素；越界索引、地址算术溢出和容量不足不产生部分绘制。

RGB565 是本机像素值，不是资源文件编码。进度为整数 `0..256`，直接对 R5/G6/B5 通道计算 `(a*(256-t)+b*t+128)>>8`，端点精确，半值向上取整。性能优先，不转换到 RGB888、不做 gamma 校正、不要求匹配其他过渡实现。输出可以与任意完整输入 palette 相同，但部分重叠被拒绝。

## Ownership

核心仅拥有矩形、颜色索引、palette blend 和最小 span 填充。内部 `span_internal` target 仅允许 Lua adapter 引用 unchecked span helper，header 不进入公共 include 路径。该 helper 要求 caller 已校验像素存储并完成裁剪；它不是第三个公共 API。Lua 的旧 commands/mesh 保留原有几何、裁剪、变换和缓存，只通过原 wrapper 共用填充。

Lua adapter 拥有 VM 数据、Display acquisition、dirty tile、背景恢复和关闭流程。应用拥有图案、配色、进度与提交时机。静态背景可复用既有 snapshot/restore；颜色改变后由应用重画并更新背景基线，核心没有动画调度或背景缓存。

## 验证与性能

```sh
bazel test //libs/raster2d:all
bazel test //libs/lua:all
bazel run -c opt --copt=-UNDEBUG //libs/raster2d:benchmark_raster2d
bazel run -c opt --copt=-UNDEBUG //libs/lua:lua_test -- --raster-benchmark
bazel test //projects/example/targets/pkg_tar/raster2d:browser_test
bazel run //projects/example/targets/pkg_tar/raster2d:serve
```

独立 C/WASM 示例显示两套颜色的端点、中间值、裁剪与顺序覆盖；它不加载 Lua。C 单测用独立像素 oracle 检查输出和 guard，并穷举 R5/G6/B5 通道对及进度验证插值。Lua 测试使用真实 Host，验证错误、背景损伤、OOM 与成功热路径零分配。

基准使用独立生成的通用图案，主 workload 为 240×240、1536 个不重叠矩形与共享几何的三套 palette；更小/更大批次以及裁剪重叠场景用于观察规模变化。纯 C benchmark 单独报告 palette 与 replay，Lua harness 对比逐块 fill_rect、旧 commands 与新 replay，分开统计初始化、palette、填充及损伤、恢复和提交。记录 warm-up、样本数、p50/p95、内存、调用与分配，不把初始化或首个缓存建立算作稳态分配。

Host CPU/WASM 结果不等于嵌入式结果。30 FPS 仅是候选预算，不是性能承诺；显示提交时间也不是边界调用数的函数。设备测量必须记录 board、CPU/总线、toolchain、内存位置、提交方式和 operator 授权；未具备条件时记录 SKIP 及剩余风险，不自行 flash/reset。

## 仿射纹理

`h2_raster2d_draw_sprites` 借用 RGBA8 纹理和有序附件数组，校验全批后写入 RGB565 surface；`h2_raster2d_sprite_validate` 供 producer 发布前验证。矩阵、图集、锚点、透明混合、量化和极端输入规则以生产头为准。只在变换后的包围矩形与 clip 相交区域逐像素取样；无 allocator、缓存、资源加载、damage 或 present。

`//libs/raster2d:texture_test` 用前向变换纹理格的独立 oracle 验证采样、stride guard、裁剪、透明及整批失败原子性。Lua/WASM 组合和内存预算见 [Skeleton2D](./skeleton2d.md#仿射纹理附件)。原矩形 replay 基准不是纹理性能证据。
