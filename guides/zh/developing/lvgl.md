# LVGL

`libs/lvgl` 集成 `@h2_vendor_lvgl`，并通过 GizOS PAL 提供 LVGL 所需的 OS abstraction。

## API Reference

[API Reference](/references/lvgl)

`libs/lvgl/include` 中实际参与项目构建的头文件是 LVGL 的生产 Public API contract。

## 依赖和边界

这个 library 负责 LVGL portable source、OSAL glue，以及 PAL/Runtime 到 LVGL 的 target-independent adapter。它不负责 display controller、Touch provider、GPU、window、board wiring 或 App UI lifecycle；具体 display/input backend 由 component、BSP 或 desktop adapter 提供。

## PAL 集成

`h2_lvgl_platform_init()` 在调用 `lv_init()` 前绑定 Runtime 提供的 Memory、Task、Sync、Queue 和 Time PAL API。LVGL 的 custom malloc ABI 由 `libs/lvgl` 实现，所有 widget、TinyTTF glyph cache、filesystem cache 和 LVGL internal object 都通过绑定的 Memory PAL 分配；target 不能回退到 libc heap。调用方必须在 `lv_deinit()` 完成后再调用 `h2_lvgl_platform_deinit()`，保证 allocator 的生命周期覆盖全部 LVGL object。

文件资源通过 `h2_lvgl_fs_register()` 注册为 LVGL drive。Adapter 把 `P:/...` 这类 LVGL path 映射到调用方注入的 PAL Filesystem，并在 backend 不支持 seek 时使用有界 scratch buffer 实现 forward seek 或 reopen。字体、图片和其它 consumer 只使用 LVGL path，不能直接依赖 POSIX、ESP-IDF、Armino 或 Desktop 文件 API。

`h2_lvgl_touch_create()` 把一个已校准到 display viewport 的 Touch PAL 注册成 LVGL pointer indev，不知道 evdev、GPIO、controller 或 board identity。`h2_lvgl_button_bind()` 解析 App Button component 到 mapped `PUSH_EDGE` periph，再把 widget 的 pressed/released edge 写入 Runtime；它不在 LVGL callback 中自行识别 click 或 long press。Adapter 对同一 Runtime/periph ID 强制唯一 live producer，重复 bind 返回 `H2_PAL_ERR_BUSY`；widget delete 释放 ownership 后才允许 rebind。

ESP-IDF、BK7258 和 Desktop build adapter 都编译同一份 `h2_lvgl_osal.c` 与 `h2_lvgl_fs.c`。TinyTTF 是否启用、glyph cache 容量和 translation 等 feature 由各 target 的 `lv_conf.h` 决定；这些配置不能通过修改 verified upstream archive 实现。

`third_party/lvgl.BUILD.bazel` 只把固定 upstream source set 暴露为 source filegroups；它不依赖任何 first-party target。GizOS-owned ESP-IDF/BK7258 `lv_conf.h` 完整文件位于 `third_party/lvgl_patch/config/`，由 vendor repository 通过 `overlay_files` 装配。`libs/lvgl/BUILD.bazel` 拥有可编译的 `lvgl`、`firmware`、`lvgl_mobile`、`lvgl_web` 与 `lvgl_desktop` variant，并分别选择 config 和其他平台 dependency。`firmware` 使用固定 source set，把 upstream、`h2_lvgl_osal.c` 和 `h2_lvgl_fs.c` 编译为一个 Bazel archive；ESP-IDF 与 BK7258 component 只注册 include/SDK dependency 并导入该 archive。Web variant 使用 single-thread config，并声明 Emscripten libc 提供 `strnlen` 所需的 POSIX feature level；Desktop variant 只选择 `core_sources`，不编译 LVGL upstream SDL2 driver。

`//tools/lvgl/font_conv:font_conv.bzl` 公开 `lvgl_font` rule，并由 Bazel 固定的官方 `lv_font_conv` 生成单一字号的静态 LVGL C 字体。Consumer 提供自己的 TTF/OTF/WOFF、UTF-8 symbol source、Unicode range、字号、bpp 和 C symbol；GizOS 拥有 converter 与 Node dependency pin、host execution 和 fail-closed output validation。Rule 只收集输入文件中的非 control Unicode character，不解析 App source language 或取得 UI string ownership；生成 target 可以同时进入 `cc_library.srcs` 和 `firmware_native_component.native_srcs`。输出默认使用未压缩 bitmap；consumer 只能在 runtime 已启用 `LV_USE_FONT_COMPRESSED` 时设置 `compressed = True`。需要多个字号时由 consumer 明确声明多个 target，不能在运行时调整静态字体。

不使用 LVGL OSAL、直接把 LVGL 接到 Display PAL 的 consumer 可以复用 `libs/lvgl:single_thread_config`，它固定 `LV_OS_NONE`、libc allocator 和 RGB565。Bazel Mobile artifact 依赖 `//libs/lvgl:lvgl_mobile` 编译上游 source，Web artifact 依赖 `//libs/lvgl:lvgl_web` 由 Emscripten toolchain 编译。需要 target OSAL、不同 source set、compile feature 或 toolchain dependency 时才增加 `_desktop`、`_embed`、`_mobile`、`_web` variant；不能为每个 App 创建 App-named LVGL library，也不能仅为名称整齐复制相同 target。

`//libs/lvgl:display` 只借用 Display PAL 与 Memory PAL：它打开 display、创建 LVGL display 和 RGB565 partial draw buffer，并把 flush 转换为 `draw_bitmap()` 与 `present()`。它不依赖 SDL3、不创建原生窗口，也不拥有 Desktop policy。Desktop 的 SDL3 Touch PAL 继续通过 `h2_lvgl_touch_create()` 接到 LVGL pointer；键盘与滚轮没有公共 PAL contract，因此其 bridge 留在 `//libs/pal/providers/desktop/app_support:app_support` 私有实现中。销毁时调用方先删除 LVGL consumer，再销毁 SDL3 provider，最后执行 `lv_deinit()` 与 `h2_lvgl_platform_deinit()`。

## 构建与测试

```sh
bazel test //libs/lvgl:all
```

Bazel package 编译 LVGL portable source，并排除 target-specific driver 和未启用 backend。测试验证 PAL allocator bridge、filesystem seek，以及 Display PAL 的尺寸、partial flush、stride、present 与重复 lifecycle。
