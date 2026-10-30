# BK7258 V3 202405 H2Loader

SD 的 FatFs 实例和 sector window 使用 64-byte 对齐的固定 `.psram.bss`，生命周期覆盖本次启动。SDK 的 SDIO FIFO read 向调用方 buffer 写入，FatFs mount/read/write 继续在 task context 执行；UART ISR buffer 和所有通信 deadline 保持原有存储和行为。Native consumer 的 post-link contract 应验证该 symbol 的大小、对齐和 PSRAM 地址，当前源码布局不代表新版 package 的 SD 真机资格已通过。

## 构建

```sh
bazel build --config=bk7258 \
  --//tools/bazel:firmware_version=<version> \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/bk7258_v3_202405:package \
  //projects/example/targets/h2loader_tar_zlib/ble-broadcaster/bk7258_v3_202405:package
```

该命令先通过内部 `:firmware` 构建 AP、CP、combined image 和 `app_ab_crc.rbl`，再通过 `:package` 生成 `update.tar.zlib`。`firmware_version` 同时注入当前固件 metadata 和 package manifest，二者不得使用不同版本。

## Partition layout

内部 8 MiB Flash 的原生启动入口固定为 Loader：

| 区域 | 物理地址 | 物理大小 |
|---|---|---|
| 系统 Bootloader | `0x000000` | 68 KiB |
| Loader CP | `0x011000` | 1156 KiB |
| Loader AP | `0x132000` | 1224 KiB |
| App CP | `0x264000` | 1156 KiB |
| App AP | `0x385000` | 3944 KiB |
| 原生启动控制 | `0x75f000` | 4 KiB |
| 用户配置 | `0x760000` | 124 KiB |
| App 启动记录 | `0x77f000` | 4 KiB |

Loader 合计 **2380 KiB**，App 合计 **5100 KiB**。可执行分区按 68 KiB 对齐；扣除每 32 字节附加 2 字节 CRC，App 的 CPU 可见空间为 **4800 KiB**，还需容纳镜像尾部元数据。尾部 FlashDB、coredump、EasyFlash 与 RF/网络配置保持原地址。`/dl`、`/data` 仍在 SD/FATFS，不占内部 Flash。

Loader 与 App 使用各自的分区表，分别链接到最终地址。上表是本 board 的取值；尺寸只由 board 的两张分区表决定，代码不写死。每个固件从自己的分区表推导两个窗口：Loader 表的 `primary_cp_app + primary_ap_app` 是 Loader window、`s_app` 是 App window，App 表正好相反，两张表的 `h2_boot_request` 与其余共享分区位置相同。窗口起点与大小必须是 68 KiB 的整数倍（4 KiB 擦除扇区与 34 字节 CRC 块对齐），App window 不小于 Loader window。`fixed_flash:board_layouts_test` 检查每个 board 的两张表是否一致。App CP 的 XIP 向量位于 `0x02240000`，App AP 的向量位于 `0x02350000`。不通过 BK 原生 B 槽 remap 来执行 App；H2Loader 协议里的 P1/P2 是 Loader/App 逻辑角色，不能直接等同于 SDK 的执行标记。

从旧等大布局迁移必须使用系统烧录路径，安装新 Loader 及其原生 Bootloader 分区表，并初始化启动控制记录。不能把新 Loader 包当作旧布局的普通 self-update。App 的 `all-app.bin` 包含 SDK 打包器生成的引导内容，**不能作为整机恢复镜像烧录**；App 使用 managed `update.tar.zlib` 安装到独立 App 区域。Loader 通过 managed Loader 包自升级，流程见下文“Loader 自升级”。

## 平台配置

公共 BK Preference provider 为每个 namespace handle 分配独立状态，在关闭后释放；并发数量由实际 allocator 余量决定。不要继续使用关闭后的 handle 或 cursor。并发读写仍使用公共 FlashDB 的数据库与事务锁，格式和持久化语义不变。

Loader 和 H2Loader APP layout 使用 AP 直驱的 UART1 承载 managed UART 与 AP 日志，固定为 460800 8N1。板级 AP defaults 选择 UART1，GPIO 表同时声明 P0=`UART1_TXD`、P1=`UART1_RXD`；SDK 会拒绝映射未出现在该表中的引脚，因此只修改串口号不足以启用 UART1。USB 转串口的 RXD 接 P0/TX，TXD 接 P1/RX，并与开发板共地。UART0 保留 ROM 下载与 CP 日志，UART0 转串口的 RTS 可以接 CEN 控制复位；两路串口可同时连接，均不启用硬件流控。

AP 的 UART PAL 持有 IO Stream iKCP session 和 Loader/App command owner。Loader 只有在 firmware identity 与共享 Loader state 初始化成功后才确认 UART session；随后在 storage mount、publish recovery 和 startup retry 之前启动 UART command task。startup 与 UART/BLE lifecycle/package operation 继续由共享 mutex 串行化，因此 mount 或启动恢复失败时仍保留串口诊断与管理入口。 存储挂载及 publish recovery 成功后，startup 返回命令模式时也启动 BLE，即使安装被拒绝或恢复返回错误；保留原错误与 Stage 供管理命令处理，不在 BLE 启动前反复重试同一个失败包。 进入命令模式不代表 App 已确认，入口只清理本地启动诊断，不覆盖 Core 保留的 `last_result`。

Host 打开 managed serial 后、借出 stream 前明确 deassert DTR/RTS；不支持控制线的 endpoint 只有返回 canonical `UNSUPPORTED` 才可继续，其它错误立即终止连接。APP 与 Loader status 都从设备端同一个 BLE public/identity MAC 返回 12 位小写十六进制 `device_uid`，BLE 重启后以该 UID 验证设备，而不是信任 endpoint/address。

普通 Loader 在读取 firmware version 后，使用公共 `h2_loader_read_current_loader_identity()` 构造完整 running identity，并持有已有 operation mutex；公版与私有 consumer 使用相同 metadata、容量回退与摘要校验。UID 由公共 `h2_bk_h2loader_get_device_uid()` 提供，与 App UART/BLE 使用同一 controller MAC 来源。

APP 的 Loader client 必须显式传入 `app_entry_path=app/bk/app_ab_crc.rbl`，
与 Loader 使用同一包入口。共享 client 的默认值仍为 ESP 包入口，遗漏此配置会在
完整接收后以 `H2_LOADER_STAGE_ERROR step=validate code=-12` 拒绝 BK 包。
启动身份校验使用独立 SHA256 context，避免与已经开放的 UART/BLE 收包共用摘要状态。
APP 确认与 Stage/reboot 命令使用同一 operation mutex；先提交运行镜像 metadata，
再确认平台执行标记，成功后才清理匹配的已安装 Stage。
需要耗时自检的 App 应在开放命令服务前初始化并持有 operation mutex，
在同一启动任务完成确认后释放。`ble-broadcaster` 使用此顺序，防止启动期间
新收到的同版本包被延后的确认误认为已安装 Stage 并清理。

原生启动标记始终保持 A，复位总是先运行 Loader CP；Loader CP 在 `driver_init` 之后、无线与 AP 启动之前读取 `0x77f000` 的启动记录，决定是否转入 App CP 的独立 Flash 向量。记录只在发布新请求时擦除一次，其余转换都只清位：

| 记录状态 | 复位后 | 含义 |
|---|---|---|
| 空白 / 其他 | 留在 Loader | 显式选择 Loader，或尚未选择 App |
| 请求（magic 有效、confirmed 为擦除态） | 试运行 App 一次 | Loader 安装完成或 `reboot app` 后发布 |
| 已消耗（magic=0）、未确认 | 留在 Loader，撤销 P2 的 `BOOTABLE` | 试运行在确认前复位或崩溃 |
| 已消耗、已确认（confirmed=0） | 每次直接进入 App | App 启动确认后写入，行为同原生已确认的 B |

Loader CP 在检查 App 向量之前先把 magic 写成 0，所以试运行时向量无效也会记为一次失败尝试，不会反复重启。对已确认记录，这次写入不改变任何位，但必须执行：实板上跳转前没有 Flash 编程操作时，App CP 在启动早期 HardFault（`pc=0`，来自 `bk_pm_module_vote_power_ctrl`）。写入 App 分区前先清除启动记录，CP 不会进入写了一半的镜像。App 每次启动都调用确认，只有记录处于“已消耗、未确认”时才写入。未确认的 App 显式返回 Loader 时保留已消耗的试运行记录，避免 AUTO 再次安装相同 Stage；由 Loader 选择自身或已确认 App 返回时仍清除记录。原生 ROM/系统固件烧录路径保留，CP 不承载 H2Loader UART 转发。

### Loader 自升级

Loader image 按 Loader window 链接，不能在 App window 的地址上运行。Loader 更新因此临时借用 BK ROM bootloader 的原生 B 槽重映射，把 App window 当作 B 槽：

1. 运行在 Loader window 的旧 Loader 把新 Loader image 写到 App window 开头（覆盖原 App），再把 image 末尾的 RBL 头区（`RBL_HEAD_POS` 对应的 `0x1100` 字节）复制到 App window 末尾。Bootloader 按 B 槽长度在末尾查找 RBL 头，这样 B 的校验对应的就是开头的 Loader image。
2. 选择 Partition 2 时，App window 的 CP 复位向量落在 Loader window 地址范围内，因此写原生标记 final=B、temp=B、confirm=1 并清除 App 启动记录，而不是写 CP handoff 请求。
3. 复位后 bootloader 开启 Flash 控制器的地址偏移，Loader window 的地址从 App window 取指。候选 Loader 通过重映射位（`native_slot=1`）报告运行在 Partition 2，并把原生标记确认为 B（confirm=4）。随后共享流程把 Partition 2 拷回 Partition 1。
4. 拷回完成后选择 Partition 1，原生标记恢复 A/A/confirm-A，新 Loader 从 Loader window 启动；Partition 1/2 image 相同，Stage 清理。App window 中留下的 Loader 在下次安装 App 时被覆盖。

拷回 Loader window 途中复位或掉电时，原生标记仍是已确认的 B，复位后回到 App window 中的候选 Loader 重新拷回。候选在确认 B 之前失败时，ROM bootloader 只试一次待确认的 B，随后回到 A 并留下 final=A、temp=B、confirm=1。旧 Loader 据此撤销 Partition 2 的 `BOOTABLE`，保留 Stage、不再自动重启进 B，发布不同的新 Loader 包时再重试。擦写原生标记途中掉电留下的空白标记由 bootloader 按 A 启动，三个写标记点（选择 B、确认 B、恢复 A）因此都落在可重试的状态。旧 Loader 在 Loader window 运行时只允许把 Loader image 写入 App window；只有经重映射运行的候选 Loader 能写 Loader window。

BK 条件变量等待使用栈上的静态信号量，但 SDK 仍为每个信号量分配动态自旋锁。
等待节点从链表移除后必须销毁信号量，再返回并释放栈空间；否则 BLE 高频等待会
耗尽 `CONFIG_SPINLOCK_DYNAMIC_CNT`，触发 `spinlock_mem_dynamic_alloc` 断言。
扩大锁池不能替代配对释放。`sync_cond_test` 同时验证唤醒、超时及创建/销毁计数。

## 预期表现

运行 `bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- scan` 后，只选择结构化 identity 为 `board=bk7258_v3_202405`、`target=bk7258`、`active_role=loader`、`transport=iostreamikcp` 的设备。APP 或 Loader 状态都可以通过 `send --file <build-dir>/update.tar.zlib` 直接发布 Stage；安装使用 `reboot upgrade`。

固定布局验收需要确认：Loader 镜像为 2437120 字节，App 为 5222400 字节；UART1 安装后 App 的实际 PC 落在独立 XIP 地址，SDK native slot 仍为 A；已确认 App 在复位（包括 App 启动过程中连续复位）后由 CP 直接进入；`reboot loader`/`reboot app` 往返；确认前崩溃的 App 回到 Loader 且不再自动启动，重新安装正常 App 后恢复；正常 App 安装不覆盖 Loader。

## Flash AES App upgrades

Use the host pre-encryption tool for an AES-enabled chip, with partition metadata from the App build. Ordinary App installation keeps native A as Loader and writes paired CP/AP directly into the final App window; ciphertext is not relocated through native B. The raw package checksum covers ciphertext and Flash CRC, while native RBL/vector checks use the decrypted mapped view. Recovery images and App upgrade packages are different artifacts.

The same H2Loader sources support both chip modes. A small read-only CP query supplies AES status for the Loader self-update guard; enabled or unknown status rejects that operation. For App qualification, use `--app-lifecycle` with an explicit UART/BLE endpoint and the matching package. Record the actual chip mode separately from the report filename. Transport interruption is not power-loss testing; reset and controlled power-cut recovery need separate device evidence.
