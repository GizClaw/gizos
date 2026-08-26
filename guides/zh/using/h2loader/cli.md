# H2Loader CLI

H2Loader CLI 是 repository-only 的短生命周期 native C11 设备和 package App。支持的入口是 `bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- <args>`，命令应从仓库根目录执行。CLI 不启动 persistent Web server、tray、actor 或 Desktop process。

## 环境

CLI 由 Bazel 直接编译为当前 host 的 native executable，不使用 Python、uv、venv、pyserial 或 Bleak。`<host>` 使用 `macos_arm64`、`linux_x86_64`、`linux_arm64` 或 `windows_x86_64`。Windows 使用 Win32 Host Serial PAL 枚举和打开 `COM<n>` endpoint，并把当前可访问的本地 fixed/removable DOS drive root 挂载到对应的小写 portable filesystem path，例如 `C:\` 挂载为 `/c`。查看完整命令：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- -h
```

也可以让 Make 先编译再返回当前 host 的二进制路径，直接在自己的 shell 里调用（cwd 保持不变，相对路径按当前目录解析）：

```sh
$(make h2loader-bin) -h
$(make h2loader-bin) --port <serial-port> send --file bazel-bin/<...>/x.update.tar.zlib
```

`make h2loader-bin` 只把路径写到 stdout，编译日志走 stderr；host config 按 `uname` 选择 `macos`/`linux`，可用 `BAZEL_CONFIG` 覆盖。

仓库启动前可以只检查环境：

```sh
make cfg-doctor
```

该命令通过 `scripts/config/h2loader-operation-env.sh` 加载并校验 operation environment，再运行 native CLI 的 `check` 检查 Runtime/PAL capability。CLI 不读取 process environment，也不会显示 Wi-Fi password 或其他 secret。

颜色会根据 stdout 是否连接交互式终端、`CI` 和 `TERM=dumb` 自动关闭。`NO_COLOR=1` 可以显式关闭，`FORCE_COLOR=1` 可以在日志采集等非交互环境中强制开启。

裸 `make` 只显示 help，不解析 Bazel dependency、不加载 environment、不安装依赖，也不启动进程或 listener。GizOS 的公开 upstream source 由 Bazel 按 `MODULE.bazel` 中的 immutable archive pin 和 SHA-256 digest 下载并校验，不需要初始化仓库 submodule。

根目录 `make cfg-doctor` 按以下顺序加载 shell environment；其他 Make target 不隐式加载：

1. 仓库 `.env/devenv`
2. `~/.config/h2loader/env`

每个文件独立求值并报告 `[OK] env loaded`、`[WARNING] env missing` 或 `[ERROR] env invalid`，失败文件造成的 environment 变化会被丢弃。已提交的 `.env/devenv` 无效时 Make-owned command 立即失败，不能回退到 inherited SDK；复制出的测试仓库缺少 adapter 时仍只报告 warning。私有文件无效时丢弃其变化并继续使用共享环境。environment loader 不回显文件输出或变量值，随后执行的 Doctor 只报告上述非 secret 信息。Shell 文件已经触发的外部副作用无法回滚，不属于只读检查保证。`.env/devenv` 根据仓库根目录解析同级 `../firmwares-devenv/export.sh`，为所有开发者提供统一的 SDK 和 toolchain environment；它不依赖调用命令的当前目录，也不再加载 `.env/<os-user>`。`.env/users/<os-user>.md` 只供人和 Agent 阅读，永远不会 source。直接调用 `bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader --` 时只使用调用方继承的 environment。

仓库不提供默认 Wi-Fi SSID 或 password。只有仍需要构建期 Wi-Fi 配置的 target 才从显式 environment 读取凭据；不要把 credential 写入仓库 env，只在本机创建 `~/.config/h2loader/env`：

```sh
export H2LOADER_WIFI_SSID="private-network"
export H2LOADER_WIFI_PASSWORD="private-password"
```

ESP-IDF 和 BK SDK 由同级 `firmwares-devenv` checkout 提供，不是 GizOS 仓库的 submodule。首次使用先初始化该 checkout：

```sh
git clone --recurse-submodules git@github.com:h2vivi/firmware-devenv.git ../firmwares-devenv
make -C ../firmwares-devenv
make cfg-doctor
```

`firmwares-devenv/export.sh` 提供下列可执行 contract；本表是 provider variable、
真实 consumer、路径含义和 host state 的 source of truth。某个 optional target
缺失不会清除其它已经可用的 target：

| Target | Provider variables and exact layout | GizOS consumers | Ready evidence and host state |
| --- | --- | --- | --- |
| ESP-IDF 6.0 | `H2LOADER_IDF_PATH`/`IDF_PATH` 是同一个 checkout root；`IDF_PYTHON_ENV_PATH` 是其已安装 tools Python root；`IDF_TOOLS_PATH` 是 provider tools root | H2Loader CLI build/flash operation 和 ESP launcher | checkout version 为 6.0，PATH 中 `idf.py` 正是该 checkout 的 `tools/idf.py`，tools Python executable 可用；未安装为 unprovisioned，配置错误为 invalid |
| BK7258 | `BK7258_PATH` 是 pinned AVDK checkout root；`BK_LOADER_BIN` 是单个 host executable | `bk7258_firmware` Bazel rule、BK launcher、H2Loader flash/erase | SDK commit 固定；Arm GNU 10.3.1 由 Bazel repository 下载并校验，不是 operator environment capability；loader 未配置且不是 macOS arm64 时为 `SKIP` |
| BK3633 | `BK3633_PATH` 是锁定的 BK3633 Git checkout 根目录；SDK 位于其 `SDK/` 子目录 | 三个 canonical `bk3633_firmware` label | checkout 为固定 commit，包含 required SDK source、allroles BIM project 和 Stack BIN/ELF；Arm GNU 10.3.1 由 Bazel repository 管理；BinConvert 由 Bazel 作为 exec tool 注入 |
| K4B | `K4B_CEDARX_INCLUDE_DIR` 是直接含 `vdecoder.h`/`memoryAdapter.h` 的 flat include root；`K4B_CEDARX_LIB_DIR` 是直接含 `libvdecoder.so`/`libMemAdapter.so` 的 ARM hard-float library root | K4B Bazel CedarX repository 与 MP4 Player launcher | include/library layout 完整；compiler/sysroot 由 Bazel 固定下载，不消费 provider toolchain path |

BK7258、BK3633 和 K4B consumer 不从 `$HOME`、ambient `PATH` 或 GizOS
仓库内旧 vendor path 猜测替代输入。Doctor 的 `[WARNING]` 表示未 provision
或当前 host `SKIP`，配置了但结构/版本不可用则报告 `[ERROR]`。

ESP build 和 flash 需要 ESP-IDF 环境；BK 操作需要 BK SDK、toolchain 和 loader。CLI 不主动 source SDK；直接运行设备命令时使用调用方已经继承的 environment。

## 状态

先扫描当前串口，并以设备返回的结构化 identity 选择 board、target、role 和 transport：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- scan
```

扫描使用 `iostreamikcp`，输出 JSON，不以串口文件名推断 board。Reliable 握手失败会直接报告，不会自动切换其它串口命令协议。所有后续设备命令通过 scan 返回的 `--port` 与 `--transport` 选择设备：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> status
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> memory
```

CLI 按需初始化 BLE Host：只有 `scan` 和参数校验通过的 `bleikcp-speed` 会启动 BLE；`--help`、`check`、package 命令和所有串口设备命令不会触碰 BLE。macOS 的 BLE Host 由 CoreBluetooth 提供，responsible process 没有 Bluetooth 权限（TCC）时启动 BLE 会被系统直接终止；serial-only 场景使用全局 `--no-ble`，让 `scan` 只做串口发现并完全跳过 BLE 初始化：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --no-ble scan
```

`check` 报告 BLE capability 时只检查平台是否提供 BLE Host，不启动它；`--no-ble` 下 BLE capability 报告为不可用。

串口 command transport 只保留 `iostreamikcp`。每次打开端口都会使用新的非零 session ID 完成握手；握手失败会直接报告错误。`--transport raw` 会在解析参数时被拒绝。BootROM recovery 是对应 board 文档定义的独立流程，不属于 H2Loader command transport。

Reliable UART transport 固定使用 `230400` baud，Loader 与 app image 使用相同 contract，CLI 不提供 baud 参数。打开 ESP 原生 USB Serial/JTAG endpoint 时不更新 DTR/RTS，避免只读命令重启 target；外置 USB-UART endpoint 在打开前明确将两条 control line 置低，CH340 连接的 BK target 依赖这一既有 transport 行为退出 BootROM/reset 状态并确认 reliable session。Host、ESP UART backend 和 BK AP/CP tunnel 都保留一次 encoded frame 的写边界，不增加固定 write gap；设备 console 与 protocol frame 通过 target TX serializer 串行化。进度与最终成功仍以 peer ACK 和 package checksum 为准。

IO Stream iKCP 的 `send` 由 receiver window、CWND、ACK 和重传提供 backpressure，不使用固定 256-byte/10 ms 发送节流。Host 按 KCP MSS 组织 single-segment message，并以不超过 6 KiB（BK 20-segment receive window 内）的 delivery batch 输出 `acked bytes / total / percent / rate` 进度；进度表示 peer 已确认的数据，不是仅写入本机串口 buffer。

Loader 与 app image 都返回结构化 `H2_LOADER_STATUS`。状态包括 device-reported board、target、chip 和 active role；app image 还会报告 active app name。

`memory` 返回 Internal RAM、IRAM 8-bit heap 和 PSRAM heap 的总量、当前空闲量、启动以来最低空闲量与最大连续空闲块。不同 capability 可能覆盖同一物理内存，因此不能把各组数值相加。

`bleikcp-speed` 是独立的 native C baseline client，只访问 `FEE0/FEE1/FEE2` Baseline service；它不提供 H2Loader management BLE discovery、Firmware 或 Console。macOS 使用 Darwin CoreBluetooth PAL；Linux 在真实 BLE Host PAL 可用前明确返回 unsupported。

## 连接 Wi-Fi

设备运行 Loader image 时，可以通过 reliable command session 连接 Wi-Fi：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- \
  --port <serial-port> \
  wifi connect <ssid> <password>
```

`ssid` 和 `password` 都是必填的单个参数，当前 contract 不接受空值或 ASCII 空白字符。命令只有在设备取得 IP 并返回 `H2_LOADER_WIFI result=connected` 后才成功；如果该 Loader 提供 Wi-Fi settings storage，设备会在连接成功后保存这份 STA 配置，后续 App 可以通过 Runtime `wifi_settings` 读取并重新连接。认证失败、关联超时、没有取得 IP、配置保存失败、PAL 不支持或 transport 失败都会让 CLI 返回非零状态，不能记为已连接。

该命令只允许用于 `status` 报告 `active_role=h2loader` 的设备。App image 上调用会被 Host Core 拒绝；CLI 不提供任意 raw command 旁路。主动断开当前连接：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- \
  --port <serial-port> \
  wifi disconnect
```

`wifi disconnect` 不删除已经保存的 STA 配置。仓库和 operation environment wrapper 都不补默认 SSID 或 password；依赖 Runtime `wifi_settings` 的 App 必须先在 Loader 中成功执行 `wifi connect`。SSID 和 password 会作为当前进程的命令行参数，可能被本机进程查看工具或 shell history 记录；CLI 的 help、usage 和错误输出不会主动回显 password。不要把真实 credential 写入仓库文档、脚本或提交记录。

## 构建 Package

文件参数使用 PAL filesystem 的绝对路径。macOS 暴露 `/tmp` 与 `/Users`，Linux
暴露 `/tmp` 与 `/home`，Windows 将可访问的 DOS drive 按 drive letter 暴露，例如 Windows host path
`C:\work\app.bin` 对应 PAL path `/c/work/app.bin`。
CLI 不读取 current working directory，也不把相对 host path 隐式转换成 PAL path。
同一个输出路径不能由多个 CLI process 并发写入；现有 PAL FS 没有
create-exclusive contract。CLI 直接 truncate/write/sync 目标文件，写入失败时目标文件可能不完整，
调用方必须将非零退出状态视为输出无效并重新生成。

使用已经构建的 app image 创建 package：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- package \
  --out /tmp/update.tar.zlib \
  --app-bin /absolute/path/to/app.bin \
  --role app \
  --board amoled \
  --target esp32s3 \
  --version dev
```

当前 PAL FS contract 没有目录枚举，因此 native CLI 暂不支持 `--data-dir`；带该参数会明确返回 unsupported，不会绕过 PAL。`golden` 只生成 parser 和 desktop test 使用的确定性 fixture，不用于真实固件发布：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- golden --out /tmp/golden.tar.zlib
```

Loader image 使用 `--role h2loader`，且不能包含 `--data-dir`。将 package stage 到已支持自升级的设备后，显式执行 `bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> upgrade`；仅 stage 不会自动升级 Loader。

## Stage 和启动

设备当前运行 App image 时，必须先返回 H2Loader，再发送新的 App package。不要直接在运行中的 App 上执行 `send` 或 `send-url`；更新流程只通过 App command transport 执行 `rollback`，package download、stage 和安装由 H2Loader image 执行。

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> rollback
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> status
```

继续更新前，`status` 必须报告 `active_role=h2loader`。发送完成后必须看到 `H2_LOADER_DOWNLOAD state=done` 和 `H2_LOADER_STAGE result=OK`；随后使用 `reboot` 触发安装和启动，再次查询状态，直到设备报告 `active_role=app`、`state=confirmed`，且 `installed_checksum` 等于本次 stage 返回的 checksum。

新的 `send` 或 `send-url` 会在接收 bytes 前自动删除已有 staged candidate，不需要先执行 `stage abort`。替换开始后，旧 candidate 不再作为失败回退；如果新传输、校验或发布失败，设备清理临时内容并报告 `staged_valid=0`。这类失败只表示当前没有 staged candidate，不会创建 App rollback、改变 boot target，或修改已安装 App、确认、hold 和已有 recovery 状态。

通过同一串口直接发送 package：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- \
  --port <serial-port> \
  send \
  --file /tmp/update.tar.zlib
```

`--file`、`--out`、`--app-bin`、`--data-dir` 接受主机路径：相对路径按调用 shell 的目录解析（`bazel run` 会把 cwd 切到 runfiles，CLI 会改用它导出的 `BUILD_WORKING_DIRECTORY`），符号链接会被解析，因此可以直接传 `bazel-bin/<...>/x.update.tar.zlib`。解析后的真实路径必须落在 PAL filesystem 的挂载内（macOS 为 `/tmp`、`/Users`；Linux 为 `/tmp`、`/home`），否则 `send` 会在 `stat` 阶段失败并报 `h2loader: send failed step=stat file=<path> code=<rc>`，退出码 3——此时把 package 复制到 `/tmp` 再发送。

通过已经部署的 HTTP server 让设备下载 package：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- \
  --port <serial-port> \
  send-url \
  --ssid <ssid> \
  --password <password> \
  --url https://example.test/update.tar.zlib \
  --bytes <package-bytes> \
  --sha256 <package-sha256>
```

当前 PAL Net contract 没有 TCP listener/accept，因此 native CLI 暂不支持
`send-url --file`，也不会在 target 中维护 POSIX 或 Win32 listener。

`send-url` 是 package transport，不是唯一的 Loader stage 路径。ESP target 上如果 `send-url` 返回 `state=error`、`checksum_fail`、`step=size`、重复停在同一字节数，或者网络路径不能稳定传完整包，保持设备在 `active_role=h2loader`，改用默认 `iostreamikcp` transport 的 UART `send` 发送同一个 package：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- \
  --port <serial-port> \
  send \
  --file /tmp/update.tar.zlib
```

不要为了绕过 transport 故障改变 package 内容、降低验收条件或直接进入底层烧录。UART `send` 成功必须收到 `H2_LOADER_STAGE_RECEIVE result=OK` 和 `H2_LOADER_STAGE result=OK`；URL staging 成功仍必须收到 `H2_LOADER_DOWNLOAD state=done` 和 `H2_LOADER_STAGE result=OK`。

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> reboot
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> status
```

`reboot` 后如果 `status` 持续报告 `active_role=h2loader state=install-requested`，说明 install intent 和 staged package 已经持久化，但 Loader 当前 lifecycle 没有完成转换。只要 Loader command transport 仍可通信，就必须在 Loader 内恢复，不能要求使用者按 Reset，也不能调用 esptool、切换 DTR/RTS、断电或重新烧录：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> reboot-loader
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> status
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> reboot
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> status
```

`reboot-loader` 必须完成 Loader 自身的软件重启并重新出现 `H2_LOADER_READY ... status=ready`。随后再次执行 `reboot`，让新启动的 Loader 消费已经持久化的 install intent。Tiga 等带有 MFG 或 modem 清理任务的 board 可能需要较长时间完成转换；转换命令仍在运行时不要用新的 `status` session 替换它。最终验收条件不变：`active_role=app`、`state=confirmed`，且 `installed_checksum` 等于本次 staged package checksum。

只有 scan、status、`reboot-loader` 等 Loader recovery command 都无法通信，并且对应 board 使用文档明确进入 recovery 时，才允许使用底层 reset 或 flash。

新 BK Loader 使用默认 reliable `send` 接收 package，不依赖 Wi-Fi。新 BK App 不暴露 stage；先执行 `rollback`，等待重新枚举并确认 `active_role=h2loader`，再发送 package。

不支持 reliable command contract 的旧 BK7258 image 不能通过 legacy raw H2Loader command 迁移。只有默认 `scan`、`status`、`reboot-loader` 等 Loader recovery command 都无法通信，并且对应 board 文档明确进入 recovery 时，才允许使用独立的 BootROM recovery 流程。

可共享的 Serial port、USB/BLE identity、板型映射，以及带日期的最后一次 Loader、App 或 MFG 验证基线可以记录在 `.env/users/<os-user>.md` 并提交到 Git；历史验证基线不代表设备当前实时状态。本地 package URL、Wi-Fi credential、private endpoint 和其他 secret 不能写进仓库文档或提交到 Git。H2Loader 不从 Markdown operator context 或仓库默认值取得凭据。

主动取消当前 staged candidate：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> stage abort
```

`stage abort` 不是下一次 `send` 或 `send-url` 的前置步骤。只有调用方要放弃当前 candidate、且尚未开始替换时才需要显式执行。

需要放弃本次更新并启动原来已经确认的 App 时，先执行 `stage abort`，确认 `staged_valid=0` 且 `installed_valid=1`，再执行 `reboot`。如果仍停在 `install-requested`，同样先执行上面的 `reboot-loader` recovery，再由 Loader 启动 installed App；不要用硬件 reset 代替 Loader rollback。

控制 Loader hold：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> hold on
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> hold off
```

## App 操作

重新启动当前 app：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> restart
```

重新启动并继续读取串口日志：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> restart-monitor
```

返回 Loader：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> rollback
```

## Coredump

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> coredump status
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> coredump dump
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- --port <serial-port> coredump erase
```

先使用 `status` 确认 partition 和内容，再执行 `dump`。`erase` 会删除设备上的 coredump，需要在确认已经保存必要诊断数据后执行。

## Documentation

生产 Documentation 直接在 `guides/.vitepress/dist` 生成：

```sh
make guides-build
```

CLI 不拥有 Documentation server，也不启动临时 HTTP listener。

## 验证

```sh
bazel test --config=<host> //libs/iostreamikcp:all //libs/h2loader_host:all //projects/h2loader/apps/cli/app:all //projects/h2loader/targets/cc_binary/cli:all
```

这些测试覆盖 CLI App、native target、reliable transport、typed command、package writer/inspector 和 output。Bazel firmware-only Python helper 的独立覆盖位于 `//projects/h2loader/tools/bazel:all`。
