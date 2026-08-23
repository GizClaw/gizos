# Game Runtime

`libs/game_runtime` 将 PixelRoot32 的 portable game 能力适配到 GizOS，向 app 提供 scene、display、time tick 和 input event 入口。

## API Reference

[API Reference](/references/game_runtime)

`libs/game_runtime/include` 中实际参与项目构建的头文件是 Game Runtime 的生产 Public API contract。

## 依赖和边界

Game Runtime 使用 PAL display，不直接访问具体 display controller、touch controller 或 board input。`third_party/pixelroot32.BUILD.bazel` 只暴露选定 source/header groups；`libs/game_runtime` 以 C++17 编译 `pixelroot32` 与 `pixelroot32_audio`，选择 feature definitions，并通过 `compat/` 提供必要的兼容 header。

这个 library 是 game-specific runtime，不替代负责整个 app 硬件访问与 event/state 聚合的 `libs/runtime`。

PIXA Games library family 的 host button mapping、PixelRoot scene ownership 和 PIXA asset 组织见 [PIXA Games](/apps/pixa_games)。

## Framebuffer 提交

Game Runtime 每帧在内存中的 RGB565 framebuffer 完成合成，再以 8 × 8 pixel cell 的 hash 与上一次成功提交的画面比较。第一帧提交完整 framebuffer；后续帧只向 PAL display 的 `draw_bitmap` 提交覆盖所有变化 cell 的最小矩形；画面完全未变时跳过 `draw_bitmap` 和 `present`。Display backend 负责决定局部矩形最终对应的物理刷新行为。

Game 的大地图不应依赖逐 pixel camera follow 来制造持续整屏变化。慢速 display 上应优先使用固定 viewport page：角色在当前 page 内移动时保持地图坐标不变，只在角色跨出 page 边界或切换 room 时改变 viewport；普通移动因此只需要提交角色旧位置和新位置附近的区域。

## UTF-8 文本渲染

`h2_game_text_api_t` 是 Game Runtime 的 portable 文本边界。它保持 PAL API 相同的 `user + const vtable` 形式，同步测量或绘制一个借用的 `data + byte_len` UTF-8 span。Wrapper 在调用 provider 前验证完整 UTF-8 序列；provider 再按 Unicode scalar 和实际 glyph advance 计算宽度，不能把 byte length 当作字符数。Metrics 分开返回可见 `width_px` 与下一段起点 `advance_px`，Host/game 用前者居中单段、用后者组合 label 和动态数值。

绘制目标是一个借用的 RGB565 surface，包含 width、height、stride 和 pixel capacity。Provider 必须裁剪负坐标和超出边界的 glyph，并且不能在调用返回后保存 text 或 surface pointer。所有调用都发生在 Game Runtime render thread；contract 不承诺并发访问。

Game Runtime 提供 `h2_game_text_builtin_5x7()`，用于 H2Loader、Desktop 和测试中的现有英文 UI。它复用 PixelRoot32 的 printable ASCII 5 × 7 glyph，支持 8 px 整数倍 line height，并保持现有 spacing、centering 和 RGB565 color。遇到缺失 glyph 时返回明确错误，不打包 fallback 中文 glyph。

Host 可以提供其他实现，例如使用 App 自己持有的 LVGL `lv_font_t` 或 `lv_tiny_ttf` font。`lv_font_t`、TTF data/path、glyph cache、locale 和销毁时机都归 Host；Game Runtime 和 game library 的 public header 不包含 LVGL 类型，也不加载外部字体文件。

Provider 和 catalog 的生命周期必须覆盖 game instance。创建 game 时缺少 provider、vtable、catalog，或 catalog 含非法 UTF-8，属于 invalid configuration。Measure 或 draw 在某一帧失败时，只跳过受影响的文字；gameplay 和后续帧继续。PixelRoot `Scene::draw()` 返回 `void`，因此这里没有虚构的 Runtime error result channel；Host provider 可以使用自己的日志能力记录诊断。

## 构建与测试

```sh
bazel test //libs/game_runtime:all
```
