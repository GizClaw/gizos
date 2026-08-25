# Library

`libs/<library>/` 保存没有明确 project owner 的仓库级可复用代码，`projects/<group>/libs/<library>/` 保存由具体 project 拥有的可复用代码。两者都与具体 board、chip 和 SDK 无关，可以定义 public contract 并提供 portable implementation；需要平台能力时，通过 PAL API、config、callback 或调用方提供的 portable object 注入。

`libs/pal/providers/<platform-or-sdk>/` 是明确例外：它保存由 Bazel 直接编译的平台实现，或把 Bazel archive 接入 ESP-IDF、BK7258 等原生 build system 的薄 adapter。它仍属于 Bazel library graph，不是原生 SDK source component；需要 SDK config、SDK lifecycle 或 SDK-owned source selection 的源码继续属于 `native_component_src/<sdk-family>/`。

## 标准结构

```text
[libs | projects/<group>/libs]/<library>/
├── include/                              # Public API
├── src/                                  # Portable implementation
├── tests/                                # Bazel 驱动的跨平台测试
└── BUILD.bazel                           # Library 的构建、测试和显式依赖
```

Public header 只暴露稳定类型和函数，不暴露 board header、SDK object、内部 task、private state 或 target-specific implementation。

每个 library 都必须提供自己的 `BUILD.bazel`。其中的主要 `cc_library` target 名与目录名一致；测试目录统一使用 `tests/`，不再使用单数形式的 `test/`。Library 是否进入某个平台 graph 只由 toolchain 和 compatibility 决定，不声明自定义 CI tag。

PAL 是其中的 contract-only 特例：只需要类型和 provider vtable 的 library 依赖
header-only `//libs/pal:pal`；直接调用 canonical unsupported accessor 的 target 才依赖
`//libs/pal:unsupported`。Unsupported source 按 capability 拆成独立 object，最终静态
链接只抽取真实引用的 capability，不能用 `alwayslink` 或 whole-archive 保留全集。

Library family 可以增加一层只用于分类的 container。例如 `projects/pixa_games/libs` 本身不是 library，不能直接放 `include/`、`src/` 或 shared implementation；其中每个 `projects/pixa_games/libs/<game>/` 才是遵守上述标准结构的独立 reusable library。Family 可以在具名的 `libs/cmake/` package 中提供跨 target build system 复用的 CMake API；该 build library 只拥有 family source inventory、include 和 portable compile contract，不能包含 board wiring、image policy、target SDK glue 或 platform implementation。PIXA Games 的 Host 只注入 shared Player PIXA，game-owned legacy UI 贴图转换成 `src/` 中的 ARGB4444 代码数据；具体边界见 [PIXA Games](/apps/pixa_games)。

## 依赖边界

Library 可以依赖：

- C 标准库。
- `libs/pal` public contract。
- 其他跨平台 `libs`。
- `third_party` 中的平台无关上游代码。
- 调用方传入的 PAL mem API、其他 PAL API、config 和 callback。

Library 不能直接依赖：

- ESP-IDF、Armino、FreeRTOS 或其他 target SDK。
- `boards/<board>` BSP。
- `libs/pal/providers/<platform>` 或 `native_component_src/<sdk-family>` private header。
- Project launcher 或具体 firmware image。
- Board partition、GPIO wiring、mount policy、boot policy 或 app policy。

需要 Bazel 平台实现的 library，由 `libs/pal/providers/<platform>` 封装；需要原生 SDK source glue 的 library，由 `native_component_src/<sdk-family>` 封装；需要 board 差异配置时，由 BSP 提供。

明确命名的 library compiled variant（例如 `//libs/lvgl:lvgl_desktop`）可以直接依赖 `third_party` overlay 暴露的稳定 upstream target，以取得对应工具链产出的 header 和 link input。该 target 必须是无需 first-party source、config 或 platform adapter 的纯 upstream contract；平台选择留在 overlay 内部，consumer 不依赖带 `_macos`、`_linux` 等后缀的 label。Library 即使是 platform variant 也不能反向依赖 `components/`，更不能取得 PAL backend、launcher policy 或 board 类型。

## Third-party 兼容层

部分 third-party library 会要求调用方实现固定名称和固定签名的 global function，例如 LVGL OSAL 的 thread、mutex、同步和 delay 接口。这类函数无法携带 PAL 的 `user` 参数，可以由对应 library 提供兼容层，并在初始化时绑定所需的 PAL API object：

```text
third-party global function
    -> library compat implementation
    -> library 内部保存的 PAL API object
    -> PAL user + vtable
```

兼容层必须遵守以下规则：

- Global function 只能用于满足 third-party ABI，属于对应 library 的 integration boundary，不能扩展为 `h2_pal_*()` 形式的全局 PAL proxy。
- Library 通过明确的 `platform_init(config)` 接收 PAL mem API 和其他 PAL API，通过对应的 `platform_deinit()` 解除绑定并清理内部状态。
- Global function 的实现放在 library source 中；compat header 只提供 third-party 编译所需的声明、类型或宏。
- 兼容实现只能调用 PAL 或其他 portable library，不能根据 target macro 直接 include ESP-IDF、Armino、FreeRTOS 或 board header。
- 如果 third-party ABI 使用全局函数，library 必须明确它是 singleton integration，并定义重复初始化、未初始化调用和 deinit 后调用的行为。
- 能通过显式 context、callback 或调用参数注入依赖时，优先使用显式注入；只有 third-party ABI 无法修改时才使用全局兼容入口。

`libs/lvgl` 的 OSAL 是这种模式的基准：`h2_lvgl_platform_init()` 绑定 task、sync、queue、time 和 mem API，LVGL 要求的全局 OSAL 函数再通过已绑定的 PAL API 工作。PixelRoot32 等需要 Arduino-compatible global function 的集成也应采用同一边界，而不是在 `libs` 中直接调用具体平台 SDK。

`libs/pal/providers/wolfssl` 也是这种受控 singleton integration。裁剪的 `wolfcrypt`
target 通过 `h2_wolfcrypt_crypto_init()` 注入 entropy，只提供 Crypto PAL；
完整的 `wolfssl` target 通过 `h2_wolfssl_init()` 同时复制 Memory PAL 与
entropy，安装全局 allocator/RNG bridge，并提供 Crypto PAL 与 DTLS PAL。
WolfSSL header、source inventory、private configuration 和 global symbol
全部由这个 package 独占，两个 variant 不能链接进同一 binary。

Target 不重新实现或转发 wolfCrypt provider。BK3633 的 `h2_pal_core` 只提供 hardware TRNG callback，TapDoki BSP 对 TRNG 做启动健康检查后把 callback 注入 `h2_wolfcrypt_crypto_init()`，并直接把 `h2_wolfcrypt_crypto_api()` 组装进 Runtime。Singleton lifecycle、重复初始化、deinit 和 ready/unsupported 状态仍由 `libs/pal/providers/wolfssl` 独占；TRNG timeout、continuous repetition health failure 或 provider 初始化失败会阻止 capability 进入 Runtime。

### App 初始化

`boards/main` 负责初始化 Runtime，并把 Runtime instance 传给 App entry。App 收到 Runtime 后，根据自身实际使用的 library，从 Runtime 取得所需的 mem API 和其他 PAL API，组装对应 library 的 platform config，再调用该 library 的 OSAL 或 platform init。未被 App 使用的 library 不应由 `boards/main` 或 Runtime 统一初始化。

```text
boards/main
    -> init Runtime
    -> app entry(runtime, app-level parameters...)
        -> init required library integrations with Runtime APIs
        -> run app
        -> deinit library integrations in reverse order
```

App 必须在调用 third-party API 之前完成对应 integration 初始化，并在 Runtime 仍然有效时完成 deinit。Library integration 只借用 Runtime 提供的 PAL API，不取得底层 PAL backend 的所有权，也不能在 deinit 时销毁由 Runtime 或 BSP 管理的 backend。

### OSAL 共存

不同 third-party library 的 OSAL 可以同时使用同一套 Runtime PAL API。每个 OSAL 应拥有独立的 config、内部状态和资源 handle，且只导出对应 third-party ABI 要求的符号。

需要避免以下冲突：

- 两个 integration 同时实现 `millis()`、`delay()`、`Serial` 等无命名空间的通用 global symbol。
- 两个版本或两个 adapter 同时实现同一套 third-party ABI symbol。
- 一个 library 的 deinit 清理共享 PAL backend，导致其他 library 保存的 API object 或资源失效。
- 多个 library 并发调用同一个 PAL API，但该 PAL contract 或 backend 没有提供相应的并发保证。
- Compat header 泄漏通用宏、类型或 target header，影响同一 translation unit 中的其他 library。
- 多个 singleton integration 没有定义初始化顺序、重复初始化和退出顺序。

无法避免通用 global symbol 的 third-party integration，必须由构建配置保证同一 firmware 只选择一个 provider，或者修改上游集成以使用带 library 前缀的私有 bridge。PAL API object 可以共享，third-party 的全局兼容符号和内部状态不能相互覆盖。

## Libraries

`libs/` public surface 包括：

- [`app_test`](./app_test.md)：以同一份 C scenario 在 Memory 或未来 device driver
  上验证 Runtime input 到 App state 与 production LVGL subject 的映射。
- [`audio_mixer`](./audio_mixer.md)：跨平台音频混合 API 和实现。
- [`bleikcp`](./bleikcp.md)：在已建立的 BLE connection 上提供可靠有序 byte stream。
- [`command`](./command.md)：同步、可注册、由调用方注入 byte stream I/O 的命令执行器。
- [`iostreamikcp`](./iostreamikcp.md)：在可能混入日志的 UART 或 USB Serial-JTAG byte stream 上提供可靠有序传输。
- `libco`：封装固定版本 higan-emu/libco 的单线程 stackful cooperative task executor，并实现既有 PAL Task、Time、Queue 和 Sync contract。调用方直接注入 allocator、monotonic clock，以及可选 external-poll/idle hook，并为每个 task 选择独立的固定 stack size；task 只共享 heap、global 与地址空间，不共享 local variable、call frame 或 stack。CPU context backend 在 build time 由目标 toolchain 选择，不是 PAL capability 或 vtable，也不进入 Runtime。Upstream 已支持的 host、AArch64 和 ARMv5 backend 来自 `@h2_vendor_libco` 的 verified archive；GizOS 补充的 Cortex-M、ESP32-S3 Xtensa、ESP32-P4 RV32 backend，以及对 upstream ARM warning 的最小修正位于 `third_party/libco_patch/`，由 vendor repository 通过显式完整文件 overlay 装配。`//libs/pal/providers/libco:libco` 始终把 portable wrapper 与所选 backend 编译成一个完整 `.a`，consumer 不得再并行编译第二份 libco source。每个 instance 只允许在创建它的 executor root 上调度，Native `schedule()` 使用 start-of-turn snapshot 和显式 budget，`wait()`/`wake()` 是 library/target private control；Runtime 和 portable App 只消费借出的 PAL API object。Queue/Sync/Time 的 predicate、timeout、close/reset generation、cancellation 和 live-object teardown 都由 library 拥有，不向 PAL 增加 scheduler contract。Guard 只能在检查点发现部分边界破坏，不提供 memory protection、动态增长或 stack-overflow immunity；`wake()` 是不保存历史的 edge-triggered runnable transition，不会直接切换 context。
- [`json`](./json.md)：PAL JSON contract，以及 yyjson portable provider 的严格解析、ownership 和 lifecycle。
- [`bundle`](./bundle.md)：bundle manifest、archive、path、tar 和 installer。
- [`coremqtt`](./coremqtt.md)：CoreMQTT 的 GizOS integration。
- [`corehttp`](./corehttp.md)：通过 Net PAL 提供 HTTP/1.1 的 portable coreHTTP provider。
- [`dns`](./dns.md)：跨平台 DNS client。
- [`drivers`](./drivers.md)：modem、motion、NFC 等 portable driver family。
  NFC 子目录中的 `type2` protocol engine 与 `fm17660k` controller driver 分别拥有协议和设备状态机；board wiring 仍由 component/BSP 注入。
- [`game_runtime`](./game_runtime.md)：跨平台 game runtime。
- [`pixa_games`](/apps/pixa_games)：可以被多个 project 引用的 PIXA game library family。
- [`gizclaw`](./gizclaw.md)：GizClaw client、config 和公共类型。
- [`h2peer`](./h2peer.md)：由 GizOS 维护、通过 PAL 注入平台能力的 portable WebRTC core。
- [`h2sctp`](./h2sctp.md)：由 GizOS 维护、在调用方 DTLS packet transport 上运行的 portable SCTP PAL provider。
- [`lvgl`](./lvgl.md)：LVGL platform 与 OSAL contract。
- [`mp4_decoder`](./mp4_decoder.md)：从 random-access MP4 source 产生同步、可写的音视频 presentation frame。
- [`ntp`](./ntp.md)：跨平台 NTP client。
- [`pal`](./platform_abstract_layer.md)：Platform Abstraction Layer contract。
- [`pixa`](./pixa.md)：PIXA image、pack、decode、reader 和 blit。
- [`tinyh264`](./tinyh264.md)：TinyH264 的 portable Video Decoder PAL provider。
- [`runtime`](./runtime.md)：提供给 app 使用的跨平台 Runtime。
- [`utils`](./utils.md)：APN 等小型 portable helper。
- [`wolfssl`](./wolfssl.md)：同一 upstream 下的裁剪 Crypto PAL variant 与
  完整 Crypto/DTLS provider；不公开 WolfSSL private type。

## 构建与测试

Library 的跨平台代码统一通过 Bazel C/C++ toolchain 编译，并由 `tests/` 中的测试验证。每个 stable library 都支持从 repository root 运行：

```sh
bazel test //<libs/library-or-project-library-path>:all
```

该入口会构建整个 package，并只运行 package 实际声明的测试。macOS 与 Linux CI 使用同一组 semantic label。ESP-IDF 与 BK7258 firmware graph 中的 `utils`、`audio_mixer`、`bleikcp`、`command`、`corehttp`、`coremqtt`、`iostreamikcp`、`lvgl`、`mp4_decoder`、`pixa`、`tinyh264` 和 `yyjson`，以及 BK3633 production graph 的 `wolfcrypt_bk3633`，都必须由 semantic target 使用所选 embedded toolchain 生成 `.a`；最终 firmware entry 的唯一 `firmware_lib_component` 直接选择这些 library，并将完整 archive closure 交给单个 `h2_firmware_lib` 原生组件。Firmware archive 不能通过 `stdout`、`stderr` 或 `FILE *` 隐式取得 diagnostics backend；需要日志的 portable library 必须显式借用 Log PAL。`firmware_lib_component` 会拒绝 archive 对 `_impure_ptr` 或 `__getreent` 的 undefined dependency。最终 linker 必须按真实未解析符号从 rescan group 中的静态 archive 抽取 object，不能用 whole-archive 或人为 retained symbol 代替真实依赖。任何依赖最终 SDK-generated config、SDK lifecycle 或 SDK-owned source selection 的源码都不是 portable library archive，必须由 `firmware_native_component` 描述并由最终原生 SDK action 编译。两种方式都不能在 library 中引入 board、project launcher 或 SDK 实现。SpeexDSP 只作为 Desktop PortAudio 的完整 vendor dependency，不属于 GizOS library 或 embedded archive graph。

Library 的 BUILD target 只引用自身显式列出的源码、明确依赖的其他 library 和受控 third-party vendor overlay，不能从 board、component、project launcher 或 SDK tree 拉取实现文件，也不能使用 recursive source glob。

## API 文档

Library Public Header 中的 Doxygen 注释是 API 说明的 source of truth。VitePress 不再复制一份手写 API，也不直接把完整 header 当作普通代码块展示；文档构建会先由 Doxygen 解析 Public Header，再将 XML 转换为 VitePress 中的结构化 API 页面。

```sh
cd guides
make guides-build
make guides-watch
make guides-build
```

`dev` 和 `build` 会自动执行 `api`。生成器优先使用本机的 `doxygen` 命令；没有安装 Doxygen 时会使用 Docker 中的 Doxygen。macOS 可以通过 `brew install doxygen` 安装本机命令，以避免每次通过容器生成。

API 页面会按 header 展示 macro、type、enum、data structure、function、参数、返回值和字段说明。新增或修改 Public API 时，应同时更新同一 header 中的 Doxygen 注释，不能在 Markdown 中维护第二份函数声明。

API Reference 只从参与生产构建的 Public Header 生成，不能从手写副本或临时 target shape 生成。

## Lua library

`libs/lua` 的 Core 与 Runtime adapter ownership、依赖方向和公开模块见
[Lua Runtime](./lua)。`//libs/lua:lua_core` 不得依赖 Runtime/PAL；
`//libs/lua:lua_runtime` 是唯一可以借用 `h2_runtime_t` 的 adapter，
`//libs/lua:lua` 是符合 library ownership 规则的 semantic target。
