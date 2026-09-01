# 固件结构分区与类型

H2Loader 管理的固件分为两类：Loader 固件和 App 固件。更新包、Host UI/CLI 和 flash slot 都不是固件。

## 分区布局

H2Loader 使用以下存储。它们不一定都是 Flash 上的 partition：

| 名称 | 内容 | 用途 |
| --- | --- | --- |
| `h2loader` | Canonical Loader firmware | 固定的管理、恢复和 Loader 更新最终运行位置 |
| `app` | App firmware；Loader 更新期间临时保存 trial Loader | App 的正常运行位置，也是 Loader self-upgrade 的中继位置 |
| `dl` | `update.tar.zlib` candidate 和 staging 临时文件 | 接收、校验、替换和发布更新包 |
| `data` | App 数据和 `/data/.checksum` | 保存随 App package 安装的数据与内容版本 |
| `preference` | `boot_intent` 与 Stage、Partition 1、Partition 2 metadata | 跨重启保存实际镜像 identity；不保存安装流程 phase |
| `coredump` | 固件崩溃数据 | 供 Loader/App command 查询、导出和擦除 |

ESP target 的 `preference` 是独立的 256 KiB internal LittleFS partition；24 KiB `nvs` 仍保留给 ESP-IDF system state，H2 PAL 不再把新 Preference 值写入其中。BK7258、BK3633 和 Desktop 继续使用各自已声明的 backend。

### 新 Board 接入要求

新增支持 H2Loader 的 Board 必须满足以下要求：

| 要求 | 合同 |
| --- | --- |
| 两个启动位置 | Loader 分区和 App 分区都必须可以启动，并通过 PAL 查询当前分区、选择下次启动分区。 |
| 非运行分区写入 | Loader 必须能写入并校验 App 分区；trial Loader 必须能读取自身并写回、校验 Loader 分区。任何时候都不能擦写当前正在运行的分区。 |
| ESP 容量 | App 分区必须严格大于 Loader 分区。 |
| BK7258 容量 | `h2loader` 与 `app` 两个 boot window 必须严格等大。 |
| 其它平台容量 | 必须先定义能够完成 Loader -> trial -> canonical 中继的 target-specific 布局；不能默认套用 ESP 或 BK 规则。 |
| Firmware 边界 | 构建必须拒绝超出目标分区的 raw firmware image；设备端必须在第一次 erase/write 前再次检查 image size 和目标容量。 |
| `/dl` | 可以位于内部 Flash、外部 Flash 或可移除存储，但必须提供 filesystem 语义，并能容纳一个最大受支持 package 以及临时写入和 filesystem metadata 开销。开始新的 stage 表示替换旧 candidate；设备在接收新 bytes 前删除旧 candidate，只在新 package 完整校验并提交 identity 后发布它。 |
| `/data` | 可以与 `/dl` 共用物理介质，但必须是独立的逻辑目录，支持安装时替换 App 数据并维护 `/data/.checksum`。 |
| 存储不可用 | 必需的 Flash、SD 卡或 filesystem 无法挂载时必须明确启动失败，不得静默换用未声明的 fallback。 |
| Board 文档 | 必须记录 H2Loader 使用的全部存储、物理介质、容量和关键 SDK 配置。 |

分区容量比较使用可写入 raw firmware image 的实际容量，不使用压缩 package 大小。

各 Board 当前采用的存储布局、关键 SDK 配置和源文件入口统一记录在 [Boards 总览](./boards/)。Loader 更新的分区切换过程见 [Loader 更新](./update/loader)。

## Loader 固件

Loader 固件是设备的固件管理与恢复入口。Portable Loader App 位于 `projects/h2loader/apps/loader/app/`；每个 Board 的构建入口位于：

```text
projects/h2loader/targets/h2loader_tar_zlib/loader/<board>/
```

Loader 固件负责接收和校验更新包、安装 App 固件与数据、选择下一次启动的固件、更新 Loader 自身，并在 App 无法正常启动时提供恢复入口。

Loader 固件报告 `active_role=loader`。支持 BLE 的 Board 通过固定 Service UUID 和 Service Data 广播 H2Loader GATT service，不携带 local name；Host 根据 Service Data 中的 Board 合成 `h2l.<board>` 显示名。连接后的 `stats` 是设备身份和当前角色的最终依据。

### 统一设备指令

APP 和 Loader 注册同一命令集合；memory、Wi-Fi 和 Coredump 是否可用由 PAL provider 与动态 command availability 决定：

| 指令 | 作用 |
| --- | --- |
| `h2loader help` | 输出 Loader 支持的指令。 |
| `h2loader status` | 输出 active identity、运行/下次分区、`boot_intent`、三份 metadata、`last_result`、capabilities 和 command availability。 |
| `h2loader stats` | 输出运行统计。 |
| `h2loader memory` | 输出 internal RAM、IRAM 和 PSRAM 的容量、空闲量与最小空闲量；没有 memory stats provider 时返回 unsupported。 |
| `h2loader wifi scan [--limit <1-16>] [--timeout-ms <1-30000>]` | 有界扫描 Wi-Fi AP；每个 callback 立即输出一条安全编码的结果，最后输出独立 terminal summary。serial IO Stream iKCP 与 BLE-iKCP 使用同一 typed command。 |
| `h2loader wifi connect <ssid> <password>` | 连接 Wi-Fi，等待取得 IP，并在连接成功后保存同一份 STA 配置供 App 重启后使用。 |
| `h2loader wifi disconnect` | 断开当前 Wi-Fi。 |
| `h2loader stage <bytes> <sha256>` | 替换已有 staged candidate，再从当前 command transport 接收指定长度的更新包；完整校验 archive SHA-256 后才发布到 `/dl`。 |
| `h2loader stage url <url> <bytes> <sha256>` | 替换已有 staged candidate，再通过 HTTP 下载更新包；完整校验长度与 SHA-256 后才发布到 `/dl`。 |
| `h2loader stage abort` | 放弃当前或已经发布的 staged package。 |
| `h2loader reboot app` | 设置 AUTO、选择 Partition 2 并重启；不消费 Stage。 |
| `h2loader reboot loader` | 设置 LOADER、选择 Partition 1 并重启；不消费 Stage。 |
| `h2loader reboot upgrade` | 设置 AUTO、选择 Partition 1 并重启，由 Loader 执行完整升级检查。 |
| `h2loader coredump status` | 查询 Coredump partition 和有效数据大小；省略子命令时默认为 `status`。 |
| `h2loader coredump dump` | 按顺序输出当前 Coredump 数据。 |
| `h2loader coredump erase` | 擦除 Coredump partition。 |

Wi-Fi、memory stats 或 Coredump 的底层能力不可用时，对应命令不出现在 availability 中；直接调用返回统一的 unsupported/unavailable 响应。旧的 `restart`、`rollback`、无参数 `reboot`、`reboot ota`、`reboot-loader`、独立 `upgrade` 和 `hold on/off` 已删除。

三个 reboot 命令在执行重启前输出对应 accepted marker。它只表示 durable 请求已经提交；Host 仍须断开旧 session、重新发现同一物理设备并用新的 live status 验证预期 role、partition、identity 和 Stage 终态。

## App 固件

App 固件是由 H2Loader 安装和启动的产品或诊断应用。每个 App 固件拥有独立的 Board 构建入口：

```text
projects/<owner>/targets/h2loader_tar_zlib/<app>/<board>/
```

`<owner>` 是 Portable App 的真实 project owner。Artifact entry 只组合 App、BSP 和 H2Loader App client；`h2loader_tar_zlib` 只表达安装产物类型，不改变源码或 artifact ownership。

App 固件在业务逻辑之前启动 H2Loader App command service，并在达到 command-ready/healthy point 后确认当前 image。它报告 `active_role=app`。同一 Board 的 Loader 与 App 广播相同的 H2Loader Service UUID 和 Board identity；Host 合成相同的 `h2l.<board>` 显示名，并通过 `active_role` 区分当前运行的固件。

ESP App command service 通过 Power PAL 读取当前启动分区并要求该分区具有 `APP` 标志，再把实际分区 ID 交给 H2Loader Core；产品 App 不配置或复制 App 分区 ID。

APP 复用上面的完整设备命令实现，包括 Stage payload/url/abort 和三个 reboot 命令。APP 与 Loader 共享 Pref、DL 路径、package validator、digest、HTTP/Wi-Fi provider 和 operation mutex，不复制另一套协议或发布逻辑。
