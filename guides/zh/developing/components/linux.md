# Linux Components

`libs/pal/providers/linux/` 保存可被多个 Linux userspace target 复用的 PAL backend。它只依赖 Linux/POSIX userspace contract、portable Libraries 与 PAL，不拥有某个 SoC vendor SDK、物理 board wiring、App lifecycle 或 service unit。

## 目录结构

```text
libs/pal/providers/linux/
├── pal_core/             # libc, pthread, button, touch, netif, event 与 framebuffer PAL
├── serial_host/          # Linux Host Serial discovery/provider
├── alsa_audio/           # ALSA PCM playback Audio PAL
└── fdk_aac_decoder/      # FDK-AAC Audio Decoder PAL
```

`pal_core` 当前提供 libc Memory、stderr Log、POSIX Time、pthread Task/Queue/Sync、evdev/GPIO single-button、evdev single-contact Touch、Linux Netif、rtnetlink System Event 和 framebuffer Display。BSP 在 Runtime assembly 前注入 framebuffer path、viewport、evdev stable device name/key code、Touch axis transform 或 GPIO chip label/line offset/active level；component 不固定 `/dev/fb0`、`/dev/input/eventN`、`/dev/gpiochipN`、panel geometry、物理输入或某块板的 service policy。

不支持的 PAL capability 由 BSP 使用 `libs/pal` 的 canonical unsupported API object 显式组装。Linux 内核或 pthread 的存在不代表 BLE、Filesystem、HTTP 等全部 PAL 自动可用；Touch 和 Audio 也只有 BSP/launcher 明确配置对应 provider 时才可用。

## Task lifecycle

Task provider 为每次成功 start 分配 opaque handle，并用 pthread 运行 entry。`min_stack_size` 非零时不得低于 `PTHREAD_STACK_MIN`；创建失败时释放 partial state。成功 join 等待 entry 返回并释放 handle，join 失败时保留 handle，允许调用方按 PAL contract 重试。

## Sync 与 button

Sync provider 使用 pthread mutex、condition variable 和进程内 semaphore；timeout 基于 monotonic clock。普通与 recursive mutex 均可用，但 PAL condition wait 按 contract 只接受由调用方持有的 non-recursive mutex。

evdev button provider 通过 `EVIOCGNAME` 发现 BSP 指定的设备，再用 `EVIOCGKEY` 读取指定 key 的当前 pressed/released 状态。它缓存发现到的 path，但设备消失后会重新发现，因此 BSP 必须配置稳定 device name 和 Linux key code，不能绑定易变的 `eventN`。provider 当前只实现 single-button；Linux input/BSP 必须提供稳定的当前按键状态，Runtime 负责 down/up 边沿与 action 识别，launcher 负责 App component mapping。

GPIO button provider 枚举全部数字后缀的 `/dev/gpiochip*` 节点，通过 `GPIO_GET_CHIPINFO_IOCTL` 按 BSP 指定的 stable chip label 发现 character device，再以 input 方式请求 line handle 并读取当前逻辑状态；枚举不假定 `gpiochipN` 的编号上限。BSP 通过 process-wide `h2_linux_configure_gpio_buttons()` 传入 `periph_id`、chip label、zero-based line offset 和 active level，provider 同步复制 mapping 与 label，并在第一次 read 时完成发现和 request。没有匹配 chip/line 返回 `H2_PAL_ERR_NOT_FOUND`，多个有效 chip 使用同一 label 返回 `H2_PAL_ERR_INVALID_STATE`，访问权限不足返回 `H2_PAL_ERR_UNAVAILABLE`，line 已被内核或其它 userspace consumer 占用返回 `H2_PAL_ERR_BUSY`。成功 handle 保留到 read failure、reconfigure 或进程退出；设备移除导致的 read failure 会关闭旧 handle，下一次 read 重新发现。Reconfigure 在提交新 copy 前关闭全部旧 handle，close 失败则返回 `H2_PAL_ERR_IO` 并保留旧 mapping 供后续 lazy reacquire。配置、read 与 reconfigure 不提供并发安全，BSP 必须在 Runtime 初始化并启动 input task 前完成配置。目标 Linux image 必须先保证 ownership 唯一，provider 不擅自解绑其它 driver。

两个 button provider 当前都只实现 single-button 当前状态读取。evdev 路径使用 Linux input 已确认的状态；GPIO provider 不额外实现物理 debounce，需要 debounce 的 board 必须在电气、内核或独立 target component 层提供稳定状态。Runtime 只负责从 pressed/released 产生 down/up 边沿和带时间戳、click count 的 action，launcher 负责 App component mapping。

## Touch

evdev Touch provider 枚举全部名称符合 `/dev/input/event[0-9]+` 的 node，通过 `EVIOCGNAME` 按 BSP 提供的 stable device name 发现设备，不对 `eventN` 编号设置上限。Open 必须通过 `EVIOCGABS` 取得有效的 `ABS_MT_POSITION_X/Y` minimum 与 maximum；缺少 axis 或 `maximum <= minimum` 返回 `H2_PAL_ERR_UNSUPPORTED`。每个 raw axis 先 clamp 到 `[minimum, maximum]`，再以整数向下取整公式 `(clamped - minimum) * (logical_size - 1) / (maximum - minimum)` 映射到 logical range；之后依次应用 `swap_xy`、`invert_x` 与 `invert_y`。当前 contract 支持 legacy single-contact Type A stream：首次 `DOWN` 要求 `BTN_TOUCH=1` 与 X/Y 坐标都出现在同一个 `SYN_REPORT` logical report 中；每个 `SYN_REPORT` 都清除本 report 的坐标和 contact 有效标记，不能用更早 report 的坐标补齐按下事件。成功建立 contact 后保留最后已知 raw X/Y；后续 report 只更新出现的 axis，未出现的 axis 沿用该 contact 的最后值，因此单轴变化可以形成 `MOVE`，带单轴更新的 `BTN_TOUCH=0` 也以最终坐标形成 `UP`。设备消失后下一次 open 重新发现，BSP 不能保存易变的 `eventN`。

Provider 不创建 input task，也不调用 LVGL。LVGL/App thread 通过 Touch PAL 的 nonblocking `poll_event` 消费 edge；gesture 和 widget hit-test 分别属于 Runtime 或 LVGL，`libs/lvgl` adapter 只把 widget edge 写入 launcher 映射的 `PUSH_EDGE` Button periph。

## Netif 与 System Event

`libs/pal/providers/linux/pal_core` 是 Linux Netif 与 SystemEvent 的唯一 owner。Netif 使用 `getifaddrs`、interface ioctl 和 route table 生成真实快照。System Event 使用有界 subscription table 与可 join 的 rtnetlink monitor；deinit 先唤醒并 join monitor，再 drain in-flight callback。resolver 无法把 DNS 安全归属到非默认 interface 时返回成功的空列表，不伪造 per-interface DNS。Desktop composition 直接选择 `h2_linux_netif_api()` 与 `h2_linux_system_event_api()`，不复制实现，也不经过 Desktop forwarding accessor。

## Host Serial

`libs/pal/providers/linux/serial_host` 通过 `h2_linux_serial_host_api()` 导出 Linux-owned provider。Discovery 优先枚举 `/dev/serial/by-id/*`，再补充 `ttyACM*` 与 `ttyUSB*`，按 canonical device 去重，并从 `/sys/class/tty` parent 读取可选 product、VID、PID 与 serial metadata。它依赖 `libs/pal/providers/posix/serial_host:serial_host_internal` 的 private termios/session lifecycle；该依赖不形成公共 POSIX provider。

普通 open 不切换 DTR/RTS、不执行 board reset/download mode，也不清空输入。调用方看到的 identifier 不被 trim、alias 或重写。PTY test 覆盖 bounded I/O、timeout、duplicate open、error 与 idempotent cleanup；没有兼容 USB 设备时，只能跳过真实 enumeration smoke，不能把 PTY 当成实体设备证明。

## Framebuffer

Display PAL 的输入与 `native_format` 保持 RGB565。Provider 支持 native RGB565 framebuffer，以及将 RGB565 扩展为 opaque ARGB8888 的 framebuffer；其它 bpp、channel layout、nonstd、geometry、stride 或 storage 不满足合同时返回 `H2_PAL_ERR_FORMAT`。当 virtual geometry 与 framebuffer memory 容纳两个完整 page 时，provider 向 back page 绘制，再用 VBlank pan 切换；每轮首次 partial dirty-rectangle draw 前从当前 scanout page 初始化 back page，使未修改像素保持不变。覆盖完整 scanout 的 viewport draw 会写满目标 page，不执行这次复制；没有 draw 的 present 会先复制当前 page，避免翻到 stale 内容。不能容纳两个完整 page 时保持 single page。具体 framebuffer node 与 mode 是 BSP 配置。

## Audio

`alsa_audio` 运行时只解析 `libasound.so.2` 的 playback ABI，支持一个 S16LE mono/stereo track、nonblocking write、timeout、xrun recovery和软件音量。ALSA device name、sample rate、channels与 block size由 launcher配置；component不固定 K4B mixer route。

`fdk_aac_decoder` 把固定版本的 FDK-AAC source适配到 Audio Decoder PAL，当前只接受 raw AAC-LC与 MPEG-4 AudioSpecificConfig，并输出 allocator-owned S16LE frame。它不依赖 CedarX、board path或 ALSA，codec packet与 PCM track仍由 portable MP4 App编排。

## Build 与验证

Host contract tests 不打开真实 framebuffer：

```sh
bazel test //libs/pal/providers/linux/pal_core:pal_core_test
bazel test //libs/pal/providers/linux/pal_core:evdev_button_test
bazel test //libs/pal/providers/linux/pal_core:evdev_touch_test
bazel test //libs/pal/providers/linux/pal_core:gpio_button_test
bazel test //libs/pal/providers/linux/pal_core:sync_test
bazel test //libs/pal/providers/linux/serial_host:serial_host_test
bazel test //libs/pal/providers/linux/alsa_audio:alsa_audio_test
make -C libs/pal/providers/linux/pal_core test
```

Host test 覆盖 RGB conversion、double-buffer selection、Time、Task、Queue、Sync、evdev/GPIO button mapping、evdev Touch report mapping、Netif snapshot、default-route event、ALSA write/recovery 与 lifecycle；fake input 不证明某个 board 的实体按键或触屏能产生正确 edge。目标交付还必须构建对应 board executable，并在实机验证 ABI、device permission、line ownership、button/touch edge、坐标方向、framebuffer、route notification、mixer、speaker、signal shutdown和资源释放。

## Ownership 边界

- Allwinner CedarX 等 vendor SDK integration 属于 `libs/pal/providers/allwinner-linux/`。
- `/dev/fb0`、1024×600、T113-S3 identity 等物理配置属于 K4B BSP。
- executable、signal handler、media path 和 systemd unit 属于 App owner 的 `cc_binary/<app>/<board>/`。
- SDL window、desktop input 和跨 OS Desktop package integration 属于 `libs/pal/providers/desktop/`；Linux Netif/SystemEvent/Host Serial 不属于 Desktop。
