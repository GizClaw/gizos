# ESP-IDF 6.x Components

`native_component_src/esp-idf6.x/` 是 ESP-IDF 6.x compatibility line 的统一 SDK-coupled component source root。ESP32-S3、ESP32-P4 和 ESP32-C5 共享这里的 PAL backend、器件组件和 SDK-owned port，不再为每个 chip 复制一套顶层 component tree。Bazel archive 只有在拥有独立 SDK dependency 或 registration identity 时才建立薄导入 adapter。

## 目录结构

```text
native_component_src/esp-idf6.x/
├── h2_pal_core/                    # ESP-IDF PAL backend
├── h2_adc_oneshot/                 # Shared ADC OneShot service and stabilization policies
├── h2_esp_audio_decoder/           # ESP Audio Codec AAC-LC PAL provider
├── h2_gizclaw/
├── h2_es8311_audio_system/         # Hardware capability component
├── h2_es8311_es7210_audio_system/
├── h2_nfc_fm175xx/
├── opus_port/
└── zlib/
```

只有包含 ESP-IDF implementation source，或拥有独立 SDK dependency 或 registration identity 的 component 才放在这个 root。没有该 native contract 的 Bazel archive 由真实 source component 或 firmware composition owner 直接导入，不建立只转发 include 和 `.a` 的目录。具体 board 的 `sdkconfig.defaults`、partition、GPIO 和 wiring 仍然属于 `boards/<board>/<esp-chip>/` 或具体 image entry。SDK 配置的所有权分两层并按序合并：board 拥有完整的 canonical `sdkconfig.defaults`；layout 只拥有 partition/rollback 或注册变体（如诊断串口、E2E 内存保留）等 layout 专属项。ESP 没有具名 config profile，也没有 project-local SDK config；配置项不得替代 component 依赖声明。

## Chip 差异

ESP chip 差异通过以下方式表达：

- `IDF_TARGET` 条件选择。
- Target-specific source list。
- Kconfig 或 compile definition。
- Component config 中由 BSP 提供的 chip/board 参数。
- 仅在某个 chip 可用的 ESP-IDF dependency。

不能因为 ESP32-S3、ESP32-P4 或 ESP32-C5 的 capability 不同，就复制整个 component root。差异应该限制在拥有该差异的 source、config 或 dependency 上。

例如 ESP32-P4 没有片上 Wi-Fi 时，`h2_pal_core` 可以选择 unsupported Wi-Fi backend；它不需要复制一套独立 PAL component。

## PAL Backend

### Task policy ownership

`h2_pal_core` 只拥有一次性 policy 配置、policy 校验、FreeRTOS task 创建、诊断、join 和清理，不得比较具体 App、library、Loader、modem、board、packaging workflow 或 product task name。最终 portable name 到 absolute priority、core affinity、minimum stack 和 Internal/PSRAM placement 的映射属于具体 firmware target，并由 target package 的 `task_policy/h2_esp_target_task_policy.c` 自包含地提供。每个 target 直接声明它自己的 trie routes，不通过共享 route table 或通用字符串比较层转发。

Runtime-capable launcher 必须在 board Runtime configuration、`h2_runtime_init()` 或直接 PAL task creation 前调用 `h2_esp_target_task_policy_install()`，安装失败必须停止 startup。配置只能成功一次；未配置启动、重复配置和任意 task start attempt 后配置都返回 `H2_PAL_ERR_INVALID_STATE`。具体 target 可注册 `fallback_resolver(user, name, out_policy)`；trie 未命中时 PAL 把原始 task name 传给该函数，由 target 按 name 动态返回 priority、core、minimum stack 与 stack placement。Public H2Loader targets 的默认函数返回 priority 4、no affinity、4096-byte minimum、PSRAM stack；private target 可不注册该函数，或让函数返回 `H2_PAL_ERR_NOT_FOUND` 来拒绝，并通过 firmware macro 的 `task_policy` 参数注入私有 component，不能把私有 task name 发布到 GizOS。

Maintained Runtime scope 是使用 ESP32-S3 与 ESP32-P4 五种 public board layouts 的 H2Loader firmware targets。特定 App 的 route 只进入实际构建它的 target：`bleikcp-speed/*` 只属于 BLEIKCP speed client/server，modem routes 只属于 modem smoke。`standard` 与 ESP32-C5 `compile_only` images 不初始化 Runtime，因此不安装 task policy，也不属于此 contract 的 firmware validation scope。

Netif provider 枚举现有 `esp_netif`，以 implementation index 优先、if-key 兜底
建立稳定 identity，并映射 IPv4、MAC、DNS 与当前 default。没有 active netif
时返回空 list/`NOT_FOUND`，不是 `UNSUPPORTED`。

System Event init 建立默认 IPv4 接口 baseline。`esp_netif` inventory 和 default
read 统一转交 TCPIP context；STA 获取/失去 IPv4、接口销毁，以及仓库内所有 PPP
`esp_netif_set_default_netif()` promotion/restore 路径，都在变更提交后调用
reconcile。一个 provider-owned mutex 把 default read、baseline compare/update 和
event post 串行化，避免 ESP event-loop 与 modem task 的旧快照反向覆盖；Netif
不接管 IDF route priority。Board/modem 在 interface 存活期通过 registration hook
补充 PPP 等不能仅凭 if-key 可靠判断的 kind。

`h2_pal_core` 实现 ESP-IDF 6.x 的 PAL backend。它负责把 ESP event loop、
FreeRTOS、ESP network、NVS、filesystem、Bluetooth、crypto 和 power API 转换为
`libs/pal/include` 中定义的 contract。HTTP 由同一 component 编译 portable
`libs/pal/providers/corehttp` 作为 SDK build adapter，但 component 不拥有平台 HTTP client 或
HTTP accessor。

Net backend 使用 bounded asynchronous resolver task 实现 Net PAL 的
`resolve_start`/`resolve_poll`/`resolve_close`，使 portable consumer 的总 deadline
同时约束 DNS、connect、TLS 和 HTTP I/O；resolver capacity 用尽时返回
`H2_PAL_ERR_NO_SPACE`，close 不等待 worker，并由 backend 回收迟到结果。TLS 使用
ESP-IDF Mbed TLS；IDF 6.x 初始化 PSA RNG，兼容旧 Mbed TLS 时显式注入 Crypto PAL
random callback。显式 root CA 或平台 certificate bundle 都保持 `REQUIRED` hostname
verification、SNI 和 ALPN；只有显式 `INSECURE_TEST_ONLY` 才能关闭验证。Wrapped
socket 的 send、bounded send、recv 和 close 继续使用相同 PAL socket id。TLS slot
registry 有固定并发上限，但完整 Mbed TLS context 必须随 provider 在 heap 上分配，
不能按 slot 常驻静态 DRAM。

可能 freeze 或关闭 external-memory cache 的 ESP-IDF operation 统一通过 `h2_pal_core` 的 safe-call 执行。Safe-call 使用一个固定在 CPU0、优先级 `9` 的 Internal worker，持有 4 KiB Internal stack 和 1 KiB bounded Internal context buffer；PSRAM caller 同步复用该 worker，不在调用期间申请或释放 task、stack、TCB 或 context。Worker 内发生的 nested safe-call 直接复用当前 Internal stack，避免自锁。只有能够同时证明 caller stack 与 context 位于 Internal RAM 时，启用 PSRAM XIP 的 target 才可直接执行 callback；未启用 XIP 时还必须证明 callback 位于 IRAM。

仓库内所有启用 PSRAM 的 ESP target 必须同时启用 `CONFIG_SPIRAM_XIP_FROM_PSRAM`；`h2_pal_core` 在编译期拒绝 PSRAM 已启用但 XIP 被关闭的配置。没有 PSRAM 的 ESP companion target 不适用该约束。XIP 把 flash text 与 read-only data 复制并映射到 PSRAM，但不代表所有 ESP-IDF flash API 都允许 PSRAM task stack。例如 `esp_partition_mmap()` 会 freeze external-memory cache，并明确要求当前 stack 位于 Internal RAM；因此不能仅凭 XIP 配置绕过 safe-call worker。

NVS、internal partition、OTA、Wi-Fi persisted config、LittleFS 和 SPIFFS adapter 分别拥有自己的串行边界。LittleFS、NVS 和 internal partition I/O 还串行复用一块固定 16 KiB Internal safe I/O scratch：路径、key、config 和 bounded byte chunk 在进入 SDK/VFS operation 前复制到 Internal RAM；读取结果只在 cache 恢复并回到原 caller 后复制到 PAL output。Safe-call 是 ESP-IDF component 私有机制，不能改变或进入 `libs/pal/include`，也不能把 FreeRTOS、ESP memory capability 或 SDK handle 暴露给 portable consumer。

ESP Preference provider 挂载独立 label `pref` 到私有 `/h2pref`，要求 partition subtype 为 LittleFS、容量严格等于 256 KiB，并把所有 VFS operation 放在上述 SafeCall/scratch 边界。只有 partition 全擦除且首次 mount 失败时允许 format；非空 mount failure、record path/CRC mismatch 必须保留原始 bytes 并返回错误。旧 `h2pref` NVS namespace 只由一次性只读迁移 adapter 打开；trial App 确认前保留旧值，确认后只擦除该 namespace，系统 NVS 继续由 ESP-IDF 内部能力使用。

负责运行完整 Runtime 的 ESP target 必须提供完整 PAL surface。不支持的能力使用 canonical unsupported backend，不能通过删除 header、symbol 或 config field 表达。

PAL backend 只处理 platform/SDK 能力。具体 display、audio codec、sensor、modem 和 GPIO wiring 由硬件 capability component 与 BSP 继续组装。

NimBLE GATT server indication 在 component 内串行化 outstanding indication，并使用 `BLE_GAP_EVENT_NOTIFY_TX` 完成同步调用。Status `0` 只表示 command 已发出；只有 `BLE_HS_EDONE` 表示 peer confirmation。其他终态错误、disconnect、Host stop 和 timeout 作为 `h2_pal_ble_indicate()` 的直接结果返回；private generation 防止迟到 completion 错配。提交使用 caller 提供的 payload 创建 mbuf，不能回退到 characteristic 的旧缓存值。

ESP-IDF BLE Host vtable 显式接入可选的独立 legacy scan-response operation，但当前返回 `H2_PAL_ERR_UNSUPPORTED`。既有 `adv_set_set_data` 对 local name 的内部拆分不提升为独立 scan-response contract；在完成单独的 NimBLE lifecycle、clear、failure 和固件验证前，component 不能报告该 capability。

Crypto provider 通过 public PSA API 实现 random、X25519、HKDF-SHA256、
AES-GCM/ChaCha20-Poly1305、AES-CTR、MD5、HMAC-SHA1、P-256 和 ECDSA。
实现不能 include `mbedtls/private/**`，也不能为缺失 operation 保留 private
SHA fallback；portable libSRTP 等 consumer 因此可以注入同一完整 Crypto PAL。

## Hardware Capability

ESP hardware component 把 portable driver、ESP-IDF driver API 和 component state 组合起来。例如：

- Audio system 组合 codec、I2S、audio mixer、task 和 queue。
- ADC OneShot component 统一管理一个显式 ADC unit service、channel config、serialized raw/calibrated read，并提供彼此独立的 radio-button debounce、binary transition stabilization 与连续 raw ADC stabilization policy。Binary stabilizer 立即发布首个有效样本，后续状态切换需要连续两个相同的相反样本；连续 numeric stabilizer 在前五个 raw sample 中发布当前样本集的 upper median，第五个样本建立完整窗口，第六个样本开始进入慢速 EMA。Caller 只通过 OneShot stabilizer init/deinit 给具体 channel 安装、重置或移除 optional value stabilizer；不配置 threshold、discard count 或 interval，也不直接取得 numeric filter state。`read_value` 在未安装时直接读取 raw count；安装后，当前 raw trigger 与上一次成功发布的 stable raw baseline 相差不超过 10 count 时继续五点中值和 EMA，相差超过 10 count 时按 component-owned 的 5 个 discard sample、5 个 reseed sample 和 5ms interval 立即重建稳定值。连续的小步变化在累计偏差超过 10 count 后同样触发 reseed，读取失败则完整回滚。完整读取由 service mutex 串行执行。`read_raw` 与 `read_mv` 始终保持单次 direct read，Board 在 `read_value` 返回后才把 stable/immediate raw count 转换为电压或产品语义。Stabilizer state、numeric init/update、sample/wait callback 和 seed window 都是 component implementation detail，Board 只配置 channel 并消费精简 raw reading。
- NFC component 组合 FM175xx portable driver 与 ESP-IDF I2C 或 SPI transport。既有 I2C config/init 保持 source compatible；SPI initializer 接收 BSP 提供的 host、CS/SCK/MOSI/MISO、NPD 和时钟，并负责 device、自己创建的 bus 与 NPD lifecycle。
- Motion 和 modem component 把 portable driver 加入 ESP-IDF build。

Component 定义 config shape，BSP 填写当前 board 的 bus handle、GPIO、address、channel 和其他 wiring。Component 不保存某块 board 的常量表。

`h2_es8311_es7210_audio_system` 的 `mic_gain_db` 继续作为统一默认值；BSP 可以通过可选的 selected-input mask 和每路 gain 覆盖 ES7210 input。没有覆盖时保留原有统一 gain，并在 AEC 开启时保留 reference input 强制 0 dB 的兼容行为。ESP32-S3 与 ESP32-P4 的双 codec AEC 都使用 ESP-SR direct-AEC init、process、reset 和 release 语义，不引入另一套算法 backend。P4 的双 microphone 输入按 ESP-SR 要求转换为 channel-planar buffer，并输出 Runtime mono microphone。

`h2_es8311_audio_system` 与 `h2_es8311_es7210_audio_system` 的 playback worker 使用每次 speaker session 独立的 task。Control mutex 只保护 playback state、task handle、I2S/Mixer 生命周期检查和把一帧 Mixer 数据复制到 worker scratch；PCM conversion 与 I2S write 在锁外执行。Control mutex 与 I2S operation 的单次等待上限都是 100 ms；worker 失败后延迟 20 ms 重试。`stop_speaker` 使用 200 ms 总 deadline，在短锁内发布 stop 后立即关闭 PA，再等待 worker 清空真实 task handle。只有 worker 已退出时才能释放 Mixer、I2S 或 codec；join timeout 保留这些资源并拒绝新的 speaker session，后续 stop 可以继续收敛。成功 stop 结束当前 worker 并释放 session 标记；双 codec system 继续执行既有 idle teardown，单 codec system 保留共享 Mixer/I2S 到静态 system 生命周期。返回后旧 session 不再执行 I2S write。

两套 audio system 的 direct-AEC handle 与工作 buffer 都在 component 初始化成功时建立，并归静态 Audio System 生命周期所有。`start_mic` 只 reset 既有 AEC state；`stop_mic` 与 start partial failure 不释放 AEC。Board composition root 必须在 `h2_runtime_deinit()` 之后调用自身的 Runtime deinit；该入口先调用对应 audio-system deinit，停止并 join microphone/speaker worker，再释放 queue、Mixer、I2S、codec 和 direct-AEC，最后才释放 Audio 依赖的其他 Board provider。Board Runtime deinit 的每个 cleanup step 相互独立：audio-system deinit 返回错误不跳过 HTTP provider destroy、cached Runtime config 清零和 filesystem unmount，入口把第一个错误返回给调用方。Audio-system deinit 是幂等操作；worker 未能在有界 deadline 内退出时返回错误并保留剩余资源，允许调用方安全重试。AEC disabled 时保持现有 fallback。

## Library 与 Third-party Integration

不依赖最终 `sdkconfig.h`、IDF lifecycle 或 IDF-owned source selection 的 portable library，由最终 firmware entry 的唯一 `firmware_lib_component` 统一选择；runner 将其主 `.a` 与 `CcInfo` 传递静态依赖注册为单个 `h2_firmware_lib` component，并作为一个 rescan group 链接。依赖最终 IDF configuration 的 first-party source 由 `firmware_native_component` 声明，runner 为当前 action 生成 component directory/name/direct-source manifest，再由 `idf_component_register()` 在同一次 `idf.py build` 中编译。共享 archive import helper 位于 `native_component_src/esp-idf6.x/cmake/`。

Bazel 分别使用 Xtensa windowed ABI 或 RV32 `ilp32f` toolchain，将 `libs/pal/providers/libco` wrapper 和 `third_party/libco_patch` 中选择的 target backend 编译成 `//libs/pal/providers/libco:libco` 的唯一 `.a`。每个 ESP firmware entry 把 `//libs/pal/providers/libco`、`//libs/pal:unsupported` 与其他 portable libraries 一起列入唯一的 `firmware_lib_component`；仓库不建立没有 native source 或独立 SDK dependency 的 `h2_libco` 或 `h2_pal` 目录。`h2_pal_core` 只保留源码真正需要的普通 library 依赖，不取得最终 image 的 archive composition ownership。S3 backend 保存 stack/return、register window、SAR 和 hardware-loop state；P4 backend 保存 RISC-V ABI 的 integer 与 floating-point callee-saved registers。两者都不使用 POSIX signal、alternate stack、ucontext、TLS allocator 或额外 FreeRTOS task。ESP component 只拥有 IDF registration、generated config/include 与 SDK dependency，不能取得 CPU backend 的源码 ownership。

DevKit `libco-smoke` 从 `projects/e2e/apps/libco` 编译 portable App，并直接以 pinned `main_task` 为一个 executor root；启动和每次 context-switch boundary 都拒绝 task/core 变化。FreeRTOS 仍可在 coroutine stack 上抢占这个 outer task；因此 backend 必须在 1 kHz tick 与普通 interrupt 下保持完整 outer task state。Coroutine 自己不能调用 blocking RTOS/PAL operation；Runtime sleep、log、allocation 和 H2Loader lifecycle 只能在所有 coroutine 已返回 root 后执行。FreeRTOS high-water mark 和 stack watchpoint 只描述 outer task，不覆盖 libco 单独分配的 coroutine stack。

`h2_audio_mixer`、`h2_bleikcp`、`h2_iostreamikcp`、`h2_pixa`、`h2_mp4_decoder`、`h2_tinyh264`、`h2_utils`、`h2_yyjson`、libco、PAL 和 CoreHTTP 等 portable libraries，都由消费它们的 firmware entry 在同一个 `firmware_lib_component` 中列举；跨平台 library package 和 `h2_pal_core` 不定义 image composition target。PIXA Games 不创建逐游戏 native component wrapper；project-owned game library 仍由对应 firmware entry 完成 archive handoff。静态 archive 只按最终链接中的真实未解析符号抽取 object，不使用全库 retention。`lvgl_port`、`h2_esp_audio_decoder`、`opus_port` 和 `zlib` 是 SDK-dependent source component，由相同 firmware graph 声明 direct source、header 与 metadata，并继续由 IDF 使用最终 configuration 编译。ESP32 的 AEC 由 `esp-sr` managed component 提供，不创建 SpeexDSP archive adapter。GizOS 的跨平台 API 仍然属于对应 library owner。

## Build Validation

ESP-IDF component 变更需要对每个 maintained target 执行 compile validation；尚不可构建的 planned target 必须明确记录 `SKIP` 和 residual risk。Validation 需要确认：

- `IDF_TARGET` source selection 正确。
- 不支持能力使用预期的 unsupported backend。
- Component dependency 没有把某个 chip-only SDK module 带入其他 target。
- BSP 和 image component search path 只使用 `native_component_src/esp-idf6.x/` 中真实需要的 SDK component，以及所依赖 library 的 `native/esp-idf6.x/` adapter。

当前 maintained image matrix 只有 ESP32-S3 和 ESP32-P4。`boards/esp32p4_func_ev_board_v1_4/esp32c5/` 仍只有 config/partition skeleton，没有可构建的 BSP、launcher 或 CI toolchain target。在这些 ownership 补齐前，每个受影响的 component PR 都必须完成 S3/P4 validation，并把 C5 记录为 `SKIP` 及 residual risk；不能声称 C5 已通过。

Canonical local SDK 由同级 `firmware-devenv` 固定并经仓库 `.env/devenv` 激活；GizOS 不安装、搜索或回退到系统 ESP-IDF。`IDF_PATH` 与 `IDF_TOOLS_PATH` 只在 repository evaluation 阶段分别定位 ESP-IDF SDK 与 tools repository；tools repository 从固定的 `python_env/idf6.0_py*_env` layout 唯一解析并验证 Python environment。Native action 接收已经验证的 locator 与 committed commit/tool-version identity，不继承这些绝对路径或 caller `PATH`。全部 52 个 maintained ESP launcher 都有内部 `esp_idf_firmware` target，要求显式匹配的 ESP32-S3 或 ESP32-P4 platform，验证 SDK commit、Python、chip compiler 和 launcher `H2_ESP_TARGET`，再在隔离的临时 project/build tree 调用原生 `idf.py build`。

CI 与 Release 显式提供 native ccache 时，runner 使用 ESP-IDF 的 `IDF_CCACHE_ENABLE` integration，并分别把 ESP32-S3 与 ESP32-P4 放入独立 cache namespace。Runner 保留每个 action 的独立临时 build tree，但通过 `CCACHE_BASEDIR` 归一化其绝对路径，并关闭 CWD hashing，避免随机临时目录阻断跨 launcher 与跨 workflow 复用。不同 launcher 和 `sdkconfig` 共享 GCS `ccache/esp` family prefix，但是否复用单个 object 完全由 ccache 对 namespace、compiler content、compile flags、preprocessor input、included `sdkconfig` 与 generated header 的 key判定；不得按 project 名或人工判定跳过 native compilation。显式接收单个 `H2LOADER_WIFI_CREDENTIALS` JSON environment 的 image由runner校验并派生 `ssid` 与 `password`，同时把local和remote ccache设为read-only，使公共 object仍可命中，但任何可能包含credential的新object都不能写入共享cache。cache miss、remote暂时不可用或未配置时仍执行相同的完整 `idf.py build`。

成功的 native target 发布结构化 ELF、map、app、bootloader、partition-table、完整 flash file set、规范化 metadata，以及由同一次构建的 flash arguments 通过 `idf.py merge-bin` 生成的 `combined_factory.bin`。Combined image 写入 action-owned native build directory，从 `0x0` 供 factory/recovery tool 烧录；它不是 managed H2Loader payload。Loader role 的 recovery bundle 可以把它作为 native artifact 收入，但不改变 package identity 或 runtime update contract。缺失/空 output 或无效/逃逸 metadata 都失败。平台无关的 `h2loader_tar_zlib` target 再消费 native provider，生成唯一 H2Loader package 与 release metadata。ESP external action 不发布 CMake/Ninja intermediate，也不执行设备操作。GitHub host fake test 验证 runner contract，包括 combined image 必须写入声明的 native build directory；CI/Release matrix 构建 catalog 中全部 S3/P4 `:package` target，并由依赖触发 native build。

## 边界

ESP-IDF Component 不拥有：

- Physical board GPIO map 和 wiring。
- Board `sdkconfig.defaults`。
- Partition table 和 image layout。
- App component 到 board periph 的 mapping。
- H2Loader install policy 或 app 业务主循环。
