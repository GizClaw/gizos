# SDL3 Provider

`//libs/pal/providers/sdl3:sdl3` 把仓库固定版本的 SDL3 适配为 Display PAL 与 Touch PAL。它是 reusable provider，不属于 Desktop OS backend，也不依赖 LVGL。

## Ownership

Provider 的 opaque `h2_sdl3_t` 实例拥有一次 SDL video lifecycle、一个 window、renderer、RGB565 texture 和 pointer snapshot。`h2_sdl3_create()` 复制 title/尺寸配置；`h2_sdl3_destroy()` 关闭 Display PAL 并反向释放全部 SDL resource。调用方不能同时为同一实例创建第二个 window owner。

Display PAL 实现 `open`、`get_info`、partial `draw_bitmap`、`present`、brightness 和 `close`。Touch PAL 返回与 display viewport 相同坐标系的 pointer state。Close、focus、key、text 和 wheel 通过 `h2_sdl3_event_t` 暴露，public header 不出现 `SDL_*` 类型。

键盘与滚轮目前没有公共 PAL contract。它们到 LVGL 或 simulator button 的映射属于 Desktop `app_support` policy；SDL3 provider 只提供 SDL-free event。LVGL 通过 Display/Touch PAL 使用同一 provider，不允许 SDL3 与 LVGL 产生直接依赖。

## Validation

```sh
bazel test --config=macos_arm64 //libs/pal/providers/sdl3:all
bazel test --config=linux_x86_64 //libs/pal/providers/sdl3:all
```

测试覆盖 config validation、Display/Touch accessor、partial update、event translation、brightness、关闭与重复 lifecycle。真实窗口行为仍由 Linux/macOS integration graph 验证。
