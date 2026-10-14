# Application Host

Application Host 是 PAL application 的宿主合同和公共组装层。它让 Web、Desktop、ESP 与 BK 注入平台差异，并共享 prepare、Runtime 初始化、required-component 校验、App task、协作停止、退出等待、检查销毁与平台释放。App 仍只消费 Runtime；产品 mapping、H2Loader、catalog 和 board selection 由调用方注入。公开参数与 ownership 以 [Public API](/references/application-host) 的生产头文件为准。

## 依赖与 ownership

下面箭头表示依赖。基础 PAL target 只包含合同头文件；Application Host 不进入 Runtime 的依赖闭包。

```mermaid
flowchart TD
    Runtime --> PAL["//libs/pal:pal · header-only"]
    Host["//libs/pal/application/host"] --> Runtime
    Host --> PAL
    Host --> Atomic["//libs/atomic"]
    Web["web/application_host"] --> Host
    Desktop["desktop/application_host"] --> Host
    SDK["native_component_src · ESP / BK AP"] --> PAL
    Lua["application/host/lua · optional"] --> Host
    Lua --> VM["//libs/lua"]
    Bridge["web/application_host:gizclaw_apps_web · optional"] --> Catalog["//libs/gizclaw/lua_apps"]
```

`h2/pal/application/h2_pal_application_host.h` 只依赖 PAL 公共错误码，通过 opaque Runtime struct pointer 表达组装参数，不 include Runtime、SDL、Emscripten、SDK 或 board header。分类目录仍属于基础 PAL package；共享实现是单独的 Bazel package。Runtime 不新增 Host field，也不持有 Host。

`libs/app_host/**` 的源文件、头文件、构建宏、shell、测试及公开 API 保持原样，继续独立可用。原 `libs/lua/web` 也保留旧 Web 生成路径。新 Web provider 使用独立的 C board 类型与 symbol，仅复用旧 board 的公开 Starlark descriptor identity，使既有 board target 可以被两个 helper 消费；新路径的 Runtime/App 生命周期不依赖旧 Host library。

## 共享生命周期

1. 调用方用独立 allocator 创建 single-use Host。API descriptor 被复制，opaque context、字符串和 provider 依赖仍借用。
2. 平台在调用线程 prepare，并提供完整 Runtime config。prepare 失败也进入平台 release 回滚。
3. 公共 control task 调用产品 configure、初始化 Runtime、validate required components，再 activate 外部输入。configure 失败仍调用配对的 release_runtime；部分 activate 失败仍执行 deactivate。
4. 公共 App task 执行 entry。调用线程持续 poll 平台事件；窗口关闭、页面 Stop 或 run_ms 只设置协作停止标记。App 在返回前自行停止并 join 自建 worker。Lua runner 用 cancel job 与 checked Host destroy 完成这一责任。
5. 公共流程成功 join App，deactivate 并 drain 外部 callback，然后调用 `h2_runtime_destroy(&runtime)`。Runtime 指针仍非 NULL 时保留依赖图；指针为 NULL 的 latched worker fault 仍报告失败，但允许继续安全释放。
6. 产品 release_runtime 后，调用线程成功 join control task，最后释放平台资源。Web 在 UI 上撤下 singleton 并 drain 同步 UI callback 后，才释放公共 Host descriptor。

Web 先启动 Runtime input consumer，再绑定 DOM producer。shell 的 physical sources 与待投递 edge 队列分离：TIMEOUT/BUSY/WOULD_BLOCK/FULL 表示该 edge 未接纳，最多 256 个待投递 edge 按全局 FIFO 重试，release 和后续 press 不互相越过。UI 每批最多处理 32 个 edge，以 timer 让出浏览器事件循环；Runtime 仍在接纳时赋予 timestamp。blur/隐藏/解绑将最后的 release 入队，解绑立即关闭 DOM admission，并返回 Promise 等待队列排空和 retry timer 取消，公共 control task 成功等待后才能销毁 Runtime。拒收会触发 pump Worker 上的 input health 检查，永久故障的 consumer 用 CLOSED 关闭 UI 重试并请求停止；健康检查不能在 UI 上取得阻塞 PAL mutex，其 Runtime borrow 也必须在销毁前排空。终止错误或 outbox 溢出关闭 admission、取消重试并请求协作停止，Host 最终报告 FAIL；retained Host 的 UI 注入返回 CLOSED，不能作为 BUSY 永久重试。旧 `libs/app_host` 保持原实现。

App 普通错误与清理失败是不同结果。结果的 application/cleanup 保存实际错误，retained 表示无法证明释放安全。任何 uncertain join、输入解绑、Runtime destroy 或 provider release 失败都禁止报告成功；retained Host 不提供自动恢复或重新运行。可以在 App 内重试 contract 明确允许的 bridge close；最终仍失败时调用 retain，保留 persistent owner 和全部依赖。重复 run 也保留原 retention 证据。

协作停止没有强制取消或退出 deadline；不响应停止的 App 可以继续占用资源，同时平台事件循环仍推进。无法 join 的 task、失败的 Lua display transport、打开的 filesystem handle 都不能靠取消外层 entry 证明安全。allocator 必须独立于 provider release，并覆盖 Host destroy；上下文不能使用在 retained 返回后失效的栈对象。

## Web 最小入口

```c
#include "h2_web_application_host.h"

static h2_pal_result_t entry(h2_application_host_t *host,
    h2_runtime_t *runtime, void *user) {
    (void)user;
    h2_pal_result_t rc = h2_application_host_ready(host);
    while (rc == H2_PAL_OK && !h2_application_host_should_stop(host))
        rc = h2_pal_time_sleep_ms(runtime->time, 10u);
    return rc;
}

int main(void) {
    const h2_web_application_host_config_t config = {.name = "demo"};
    return h2_web_application_host_run(&config, entry, NULL);
}
```

```starlark
load("@gizos//libs/pal/providers/web/application_host:web_app.bzl", "h2_web_app")

h2_web_app(name = "demo", srcs = ["main.c"], app_name = "demo", deps = [])
```

该宏保留 wasm tar、serve、browser_test、COOP/COEP headers、preload、board skin、按键与 canvas test 接口。C main 和 PAL task 在 pthread Worker 运行，DOM、持久化、Canvas 与 Promise 调用通过 Web main-thread proxy。persistent_root 使用实际 IndexedDB，readonly_roots 保留资产隔离，LVGL 是可选 provider 资源。hardware 提供 prepare/release_prepare、configure/release_runtime、validate_runtime 和 Button producer；required-component 检查应在 validate_runtime 中完成。

需要产品 configure/validate/release_runtime 或 before_prepare 时，使用 `h2_web_application_host_run_app(&platform, &app, &result)` 传入普通 Host config。Web Host 复制该 descriptor、转发原始 user，并在硬件和产品 configure 完成后按最终 periph 配置 LED。`json` 按需创建和销毁 YYJSON；Core 的 task allocator 与 FirmwareInfo 观测也由平台 config 注入。调用方不再创建 Web platform、Runtime 或独立 runner task，也不再执行 pump/join/FS flush/platform destroy。UI singleton 的解绑在 UI 线程 drain 已进入的 callback；独立 admission latch 保证解绑与 descriptor 销毁之间不能启动另一个 Host。

## Desktop 入口与资源所有权

`h2_desktop_application_host_run(&platform, &app, &result)` 在进程主线程完成 provider 和公共 Host 的创建、运行及检查销毁。零尺寸 window 选择无窗口运行。未注入 Runtime descriptor 时采用完整 Desktop PAL；services 按需持有 TLS/crypto、HTTP、MQTT、WebRTC、JSON、PortAudio、SQLite、LVGL 与 BLEIKCP 全局引用，mounts 由 Host 打开文件系统。Runtime 销毁后才释放这些资源，部分 prepare 失败也执行已创建资源的清理。`service_allocator` 独立于 Host/Runtime allocator，保留 MQTT 等测试的独立分配计数。

生成的 Desktop layout 可通过 `h2::desktop::application_host_config(layout, services)` 转成平台配置。peripherals、mappings 和 input 表达按键接线，公共 Host 安装 mapper、启动 input consumer、转发 SDL key/focus 事件并在 Runtime 清理前关闭输入。status_name 可创建公共状态画面；status_on_error 只在 App 失败时显示，保持成功 App 的内容。

SDL 创建、实际 renderer、事件及窗口重建始终由进程主线程处理。直接使用 SDL/LVGL 的 App 设置 entry_on_main，在帧边界使用 `h2_desktop_application_host_should_stop`，静态画面使用 wait。Lua 等可能等待子 worker 的 App 在公共 App task 运行，以免主线程等待 Lua mutex 时阻塞 worker 的开窗请求。已打开 Display 的重复 open 是线程安全的幂等操作；真正的 reopen 才代理回主线程。cached pointer 可从任意 App task 读取，自定义触摸 adapter 仍由产品持有。

正常 GUI 运行持续到窗口关闭；app.run_ms 或 `H2_DESKTOP_RUN_MS` 可设置协作运行时限。录制、像素观察等可借用 Host 的原生 SDL/PortAudio handle，必须在 App 返回前注销观察回调。App 自有 worker/track/codec 的停止仍属于 App；无法证明安全时调用 retain。调用方的 opaque context、mapping、mount 与字符串在 retained 时必须继续有效，不能随栈或 RAII 容器销毁。

## 四端注入

| 平台 | Provider 与调用上下文 | 调用方注入 |
| --- | --- | --- |
| Web | `//libs/pal/providers/web/application_host`；C main Worker，UI proxy | hardware、persistent/readonly roots、board/skin、App entry |
| Desktop | `//libs/pal/providers/desktop/application_host`；进程主线程 run | window、services、mounts、peripherals/mappings、App hooks；特殊测试可覆盖 Runtime config |
| ESP | `//native_component_src/esp-idf6.x/h2_application_host`；已有 SDK/BSP task | 同一个 PAL `user + const vtable`，提供 prepare/activate/poll/deactivate/release/notify；由目标配置 PAL task policy |
| BK | `//native_component_src/bk7258/ap/h2_application_host`；AP BSP entry task | 同上；CP 不链接 Host/Runtime，Armino yield 由 AP provider 执行 |

Desktop 的 Runtime 在公共 control task 初始化和销毁；App 根据 entry_on_main 在 App task 或主线程执行。provider create、run、destroy 都检查进程主线程；不把第三方 SDK type 泄漏到基础 PAL。

ESP/BK 直接接收 `h2_pal_application_host_api_t`，使用与 Web/Desktop 相同的 PAL vtable，不另设 SDK config 或另一套回调名称。所有 slot 必须提供；没有外部输入时 activate/deactivate 显式返回 OK，没有额外停止来源时 poll 写入 0 并返回 OK。provider 只包装 SDK task identity 检查与 scheduler yield，header 和 native component 仅依赖基础 PAL 合同，不依赖共享 Host 或 Runtime。prepare/poll/release 必须回到 init 时的已有 task，并拒绝 ISR；prepare 返回实际 BSP Runtime config，release 必须容忍部分初始化，activate/deactivate 负责外部输入接入与 drain。API descriptor 被复制，vtable 与 user context 保持借用直到成功清理，retained 时继续保持有效。provider 不选择私有 board。`$application/control` 与 `$application/entry` 的 placement、priority、stack region 由最终 target task policy 提供，config 中可分别设置 control/App minimum stack。默认板级接线由 `boards/{devkit/esp32s3,amoled/esp32s3,bk7258_v3_202405/bk7258}:application_host` 声明，公共实现位于对应 SDK 的 `native_component_src/.../h2_board_application_host`，链接共用的完整 vtable；调用方不必重复实现空回调和 BSP prepare/release。native PAL E2E 通过 `projects/e2e/libs/application-host` 的公共 launcher 组装同一个 Host config，保留产品命令服务与测试 fixture 的 ownership。BSP wrapper 会在 entry 返回后释放 provider 时，retained 路径必须保留 owner task/context，不能返回触发自动清理。

通用接线是创建平台 provider，将其 API 填入 `h2_application_host_config_t.provider`，调用 create/run，再按 result.retained 决定能否 destroy descriptor 和调用方持有的 provider。config 表达 App entry、allocator、运行时限和 task stack 等实例参数；其中 configure/validate/release_runtime 只负责产品资源，平台生命周期只使用 PAL vtable。平台 release 只释放平台拥有的资源。H2Loader confirm/return、网络恢复和产品目录由调用方配置。

Native PAL E2E 的普通固件入口直接调用 `projects/e2e/libs/application-host` 的 `h2_pal_e2e_application_host_start`，公共实现安装 target task policy 并启动 BSP owner task，无需在每个 main 重复创建 entry wrapper；BK 同时初始化 SDK。ESP/BK 均复制应用名和普通 Host config，main/app_main 返回不使启动配置失效；user 与回调资源仍保持借用。App entry 错误由公共 E2E Host 记录并驻留，串口恢复在等待 Stop 时保持可用；Stop 后保留可能仍被失败 fixture 借用的 Runtime 图。不可继续执行的 setup 错误和专属 deadline worker 使用公共 fail/park，不重复驻留循环。网络等待、用例执行、资格确认和报告属于测试，仍留在 App entry 中。

普通 Host config 的可选 `before_prepare` 在启动 owner 上先于平台 prepare 执行；失败时不调用平台 prepare/release 或启动 Runtime，该 hook 必须保证自身资源在错误返回时安全。BK Core 的真实双核原子前提与 Backup 的原始 Preferences 分区备份使用这一阶段，保持它们在 BSP 初始化前的顺序。ESP Core 的任务栈 allocator 观测由最终目标的 task-configure 链接 wrapper 注入；公共启动仍安装同一目标 policy。

## Lua 与 catalog

`//libs/pal/application/host/lua` 提供可选的 portable Lua runner，四端都可以在 entry 中调用 `h2_application_host_lua_run(host, runtime, &config)`。它接收 ordinary Lua Host budgets、persistent resources、job args、module registration 与 event routing；填入同一个 Runtime，运行一个 job，首次等待时 ready，Host stop 时 cancel，最后 checked destroy Lua。module/resource context 在 retention 时必须保持有效。

`//libs/pal/providers/web/application_host/lua:lua_web_app.bzl` 的 `h2_lua_web_app()` 生成 board Button args、脚本资源与事件映射，执行交给共享 Lua runner。它保留 script args、memory/source budgets、exit Button、extension、serve/browser_test 和 layout 行为。

浏览器包管理使用可选 `//libs/pal/providers/web/application_host:gizclaw_apps_web`；它借用产品已有的 GizClaw catalog，提供 Promise `h2WebLuaApps.install/list/uninstall`。它不持有另一份索引，也不把包管理业务放进 PAL core 或 VM。绑定、关闭、File/Blob/typed buffer、持久化与用户 KV 边界见 [Web 应用](/apps/web#浏览器-lua-应用目录-sdk)。产品退出顺序为关闭并成功 join bridge、销毁 catalog、退出 App，然后由共享 Host 清理 Runtime/provider。

## WASM 使用点

### 手写 Web entry

- `projects/example/targets/pkg_tar/audio-system`：迁移 main、Host API 与打包宏。
- `projects/example/targets/pkg_tar/display`：迁移 main、Host API 与打包宏。
- `projects/example/targets/pkg_tar/log`：迁移 main、Host API 与打包宏。
- `projects/example/targets/pkg_tar/lua-cosmic-drift`：迁移 main、Host API 与打包宏。
- `projects/example/targets/pkg_tar/lua-flappybird`：平台、Runtime、runner、Button mapper 与 DOM 生命周期交给公共 Host 和 shell，保留 Node headless 运行。
- `projects/example/targets/pkg_tar/lvgl-smoke`：迁移 main、Host API 与打包宏。
- `projects/example/targets/pkg_tar/qrcode`：迁移 main、Host API 与打包宏。
- `projects/example/targets/pkg_tar/raster2d`：迁移 main、Host API 与打包宏。
- `projects/example/targets/pkg_tar/starboy`：迁移 main、Host API 与打包宏。
- `projects/example/targets/pkg_tar/tap-reset`：迁移 main、Host API 与打包宏。
- `projects/example/targets/pkg_tar/touch`：迁移 main、Host API 与打包宏。
- `projects/example/targets/pkg_tar/web-hardware`：迁移 main、Host API 与打包宏。
- `projects/example/targets/pkg_tar/mp4-player`：平台、Runtime 与播放 runner/pump/join 收进 Host；入口保留媒体读入、播放参数和结果显示。

### Lua 生成 entry

- `projects/example/targets/pkg_tar/lua-script`：迁移生成 glue、共享 Lua runner 与打包宏。
- `projects/example/targets/pkg_tar/lua-script-config`：迁移生成 glue、共享 Lua runner 与打包宏。
- `projects/example/targets/pkg_tar/lua-script-extension`：迁移生成 glue、共享 Lua runner 与打包宏。
- `projects/example/targets/pkg_tar/lua-script-input`：迁移生成 glue、共享 Lua runner 与打包宏。
- `projects/example/targets/pkg_tar/lua-script-stop`：迁移生成 glue、共享 Lua runner 与打包宏。
- `projects/example/targets/pkg_tar/lua-script-submit`：迁移生成 glue、共享 Lua runner 与打包宏。
- `projects/example/targets/pkg_tar/lua-script-vector`：迁移生成 glue、共享 Lua runner 与打包宏。
- `projects/example/targets/pkg_tar/web-board-amoled`：迁移生成 glue、共享 Lua runner 与打包宏。
- `projects/example/targets/pkg_tar/web-board-bk7258`：迁移生成 glue、共享 Lua runner 与打包宏。
- `projects/example/targets/pkg_tar/web-board-k4b`：迁移生成 glue、共享 Lua runner 与打包宏。
- `projects/example/targets/pkg_tar/web-board-p4-lcd43`：迁移生成 glue、共享 Lua runner 与打包宏。
- `projects/example/targets/pkg_tar/web-board-szp`：迁移生成 glue、共享 Lua runner 与打包宏。

### E2E 与间接接线

- `projects/e2e/targets/pkg_tar/pal-net-tls`：手写 boundary entry 与独立 wasm binary dependency。
- `projects/example/libs/web/demo_board`：使用新 board helper，保留旧 board descriptor identity。
- 新 `web/application_host/lua/tests/lifecycle`：实际生成入口的慢 Display Stop、ordinary error、transport/display/App join failure、EXIT 和 filesystem BUSY。
- Lua worker 自己用 monotonic deadline 与 wake queue 管理 coroutine sleep；不为每次 sleep 创建 PAL Timer，事件/取消/Stop 仍能唤醒同一 worker。Native 回归验证 Timer 分配错误不影响 sleep、deadline 不提前、并发协程与长 sleep 取消。
- 新 `web/application_host/lua/tests/progress`：持续渲染与 timer warmup 后，20 次按下/释放均须由 Lua 确认；每次释放后须继续输出至少三次周期进度，覆盖 retained 相同帧与最终 Stop 清理。
- 新 `web/application_host/tests/input_backpressure`：consumer 暂停握手后填满真实 16 项 Runtime raw-edge queue，观察到拒收才恢复消费；72 次 30 ms 正反向按键与图像复原、TIMEOUT/BUSY 拒收、release 保序、pointer/key 合并与 blur、异步解绑排空、部分 bind 回滚、永久故障 consumer、溢出/终止错误和 retained CLOSED；清理完成后不再有 native 输入调用。
- 新 `web/application_host/tests/gizclaw_apps`：同一 origin 的安装、卸载、刷新、无效包/hash、并发 BUSY、Stop 中断安装、独立用户 KV 与失败 close 重试。
- `projects/e2e/targets/pkg_tar/pal-{core,storage,crypto,http,webrtc,json,audio,audio-decoder,display,wifi}`：公共 Host 拥有平台、Runtime、FS 与 runner，保留实际用例、故障注入、独立观察和报告。Lua Runtime 的 Desktop/Web 入口也采用同一生命周期。
- `projects/e2e/targets/cc_binary/pal-*`：12 个 Desktop PAL 入口使用公共 Host；Core/Storage 的独立 allocator、WebRTC 的后端比较 fixture、Display 的真实渲染观察和 Audio 的真实 capture witness 继续由测试持有。
- Desktop examples（含录制与 BLE speed）、Pixa 六个 game 的共享 runner、showcase 均使用公共 Host。产品仅提供所需服务、资产和业务 entry；不重复创建 Runtime、窗口、网络或音频 provider。
- 新 `web/application_host/tests/app_hooks`：真实 WASM C 断言验证普通 hooks 的 user 转发、before/configure/validate/entry 错误、失败清理后的再次运行和 retained admission。
- 旧 `libs/app_host` 和 `libs/lua/web` 的兼容路径继续独立可用；mobile、ESP/BK 产品 entry 和串口安装工具不在本轮 App 入口迁移范围。项目自有 native PAL E2E 的迁移见下表。

### Native PAL E2E

26 个现有入口均使用公共 start、板级 provider 与 E2E launcher，另两个 Application Host SDK 验证入口直接验证同一 launcher 的 run 路径。旧 `h2loader_zlib_tar` labels 继续指向相同实现，firmware/package label、设备 runner 和历史验收记录不变。每个 main 只提供测试 entry、必要的 before_prepare/configure/validate、原有 serial identity/capabilities；没有 Host vtable、SDK 初始化或独立 BSP/Runtime/App task 启动流程。

本次迁移的功能验收使用 C/C++ 直接编译执行生产代码，并构建真实 ESP/BK SDK package。历史 qualification 文件保留其原始版本记录；不使用 Python 源码抽取、字符串或源码哈希匹配作为当前源码的验收。

| Suite | ESP32-S3 | BK7258 AP |
| --- | --- | --- |
| Core / Storage / Crypto / HTTP / WebRTC / JSON / MQTT / Net-TLS | DevKit 各一项 | 各一项 |
| Audio | AMOLED | 一项 |
| Audio Decoder | DevKit | 一项 |
| Display | AMOLED | 一项 |
| Wi-Fi | DevKit、AMOLED fixture | 一项 |
| Storage Backup | — | 一项 |

Core 的 atomic prerequisite 和 backup 的只读原始分区 dump 保持在 BSP prepare 之前；Core 的 transport allocator 隔离、MQTT 的控制通道暂停/恢复与所有原有测试断言和结果协议继续由各 suite 拥有。Host control task 与 entry 使用明确的 target task policy，entry 保持旧 runner 的 placement/priority/stack budget。终端回放响应协作 Stop；失败 suite 或 Core 明确保留的 cleanup owner 不允许销毁其 Runtime 借用图。默认没有运行期限，保留原有驻留行为。

公共 E2E lifecycle 先 stop/join serial command worker 并释放 operation mutex，再允许 Runtime/BSP 销毁。SDK 命令服务附带 BLE owner 时返回 BUSY，保留借用图；不会强制关闭未知 BLE owner。新增真实 Host/Runtime/task/sync 集成测试验证正常清理、部分启动失败、独立 transport allocator 和 retained owner；SDK package build 只证明编译、链接和打包，不证明实板运行。

## 验证入口

```sh
bazel test --config=macos_arm64 //projects/e2e/libs/application-host/...
bazel test --config=macos_arm64 //libs/pal/application/host/... //libs/pal/providers/desktop/application_host/...
bazel build --config=macos_arm64 //libs/pal/providers/web/application_host/tests/app_hooks:app_hooks
bazel build --config=macos_arm64 //libs/pal/providers/web/application_host/lua/tests/lifecycle:lifecycle
bazel test --config=macos_arm64 //libs/gizclaw/lua_apps/... //libs/gizclaw:h2_gizclaw_service_test
bazel build --config=esp32s3 //projects/e2e/targets/h2loader_tar_zlib/application-host/devkit:package
bazel build --config=bk7258 //projects/e2e/targets/h2loader_tar_zlib/application-host/bk7258_v3_202405:package
bazel build --config=macos_arm64 //...
bazel query 'kind("cc_test rule", //...) except attr(tags, "manual|live", //...)' > /tmp/h2-c-tests.labels
bazel test --config=macos_arm64 --skip_incompatible_explicit_targets --target_pattern_file=/tmp/h2-c-tests.labels
bazel mod deps --lockfile_mode=error
git diff --check
git diff --exit-code origin/main -- libs/app_host
```

固件 package 验证包括真实 SDK 编译与最终链接；query/cquery 只证明 graph。没有运行物理设备时不声明 SDK task 调度、硬件行为或设备清理已通过。Bazel disk cache 保持启用；SDK checkout 必须匹配仓库 pin 且无 tracked modification，不能清理其它任务的 SDK 改动来满足检查。

Web 的功能验收执行真实 Chromium 中编译后的 C/WASM 程序，再驱动实际按键、触摸、网络切换、IndexedDB 跨页持久化和合成截图。app_hooks、lifecycle 与 input_backpressure 的资格条件由运行中的 C 断言决定；普通错误、retained 与重入拒绝分别验证。Python 源码字符串、源码哈希或包装脚本退出码不作为功能资格证据。
