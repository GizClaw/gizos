# H202 MFG

入口 `projects/h2loader/targets/h2_loader_tar_zlib/mfg/haivivi/h202_bk_v3/` 替换 [H202 H2Loader](./h2loader.md)，是 Loader 角色，包为 `board=h202_bk_v3`、`target=bk7258`、`image=h202-mfg`、`role=h2loader`。语音引导的状态机位于 `projects/h2loader/apps/mfg/h202_bk_v3/`，入口 `ap/` 组合板级 Runtime、公共 H2Loader Core、UART 管理和 profile。

```sh
bazel build --config=bk7258 //projects/h2loader/targets/h2_loader_tar_zlib/mfg/haivivi/h202_bk_v3:package
bazel test //projects/h2loader/targets/h2_loader_tar_zlib/mfg/haivivi/h202_bk_v3/... //boards/haivivi/h202_bk_v3/bk7258/...
```

## Loader 角色与镜像大小

与其他 MFG 一样，H202 MFG 替换管理镜像，链接在固定 Loader 窗口（`0x11000`，CP 1156 KiB + AP 1292 KiB）。按每 34 字节物理块含 32 字节数据计算，AP 镜像最多约 1216 KiB，CP 约 1088 KiB。为放进这个窗口：

- 与 H106 MFG 相同，只用 UART 管理：`layouts/mfg` 关闭 AP 蓝牙主机和 CP 蓝牙控制器，入口在蓝牙配置打开时编译报错；同时关闭 iperf。
- 语音构建时编码为 4-bit IMA ADPCM，并把重复的重试提示单独存一份，见[语音](#语音)。

此前的实验室构建（MFG `0.3.10-14`）AP 为 1,065,260 字节（约 1040 KiB）、CP 为 770,796 字节（约 753 KiB），以构建产物为准。

H202 不使用共享 `h2_mfg_*`：它没有屏幕，需要充电、NFC、Wi-Fi 和左右麦等步骤。因此：

- 入口按 H202 Loader 的方式组装 Loader Core：只对当前镜像计算一次身份 SHA-256，由 Core confirm，能力为 UART 与 Wi-Fi。Core 在命令模式就绪后才启动厂测；管理端发起重启时直接结束 MFG。
- Core 按验收结果放行 App：入口在开放 UART 管理前、持有初始化操作锁时校验验收版本，并同步 Loader 的内存结果；每次启动重试都先校验，不依赖文件系统挂载或恢复成功。失败时保持命令模式并关闭 `reboot app`，恢复命令仍可用。
- 第 11 步保存通过结果、开机消费一次性进度或重置进度时，在同一把操作锁下同步 Loader 的内存门禁；成功验收后无需再重启一次，`reboot app` 即可使用。
- 管理端的 Wi-Fi 扫描、连接、断开与 MFG 的 Wi-Fi 检测共用同一把操作锁；检测在等待 DHCP 时释放锁，重新拿锁后会再次确认仍连在测试热点上。老化重连的每次 DHCP 等待也受 Wi-Fi 预算限制，断开关联时提前退出并重新尝试。
- 设置 `mfg_required_total=11`：十一项全部为 PASSED（SKIPPED 不算）时，开机才会进入分区 2 的 App，`reboot app` 也才可用；否则始终停在 MFG。暂存的 App 包仍可通过 `reboot upgrade` 写入分区 2，等厂测通过后再启动。
- 不提供 `h2loader mfg ...` 命令；没有定向清除进度的管理命令，不要用清空整个数据卷代替。

## 十一步流程

| 步骤 | 眼灯 | 操作 | 判定 |
| --- | --- | --- | --- |
| 01 启动 | 白色 | 开机被接受后立即亮白灯并播“已开机，正在启动，请稍候。”；SD 卡通过后后台连接 Wi-Fi（扫描、连接、IP、校时） | 结果在第 05 步汇报 |
| 02 SD 卡 | 黄灯等待，绿灯通过，红灯失败 | 插入 SD 卡。失败后重新插卡 | 挂载成功即通过，见 [SD card test](#sd-card-test) |
| 03 充电器与电池 | 橙色 | 进入时读一次电池电压，同时按提示插拔充电线 | 电池 ADC（GPIO12）无读数时立即失败并播“未检测到电池。单击肚子上的按键重试。”；电池在位且 USB 插入脚（GPIO3）两次变化即通过；60 秒超时 |
| 04 NFC | 紫色 | 测试卡靠近感应区 | 读到任意 ISO14443A 卡 UID 即通过；60 秒超时 |
| 05 Wi-Fi | 青色 | 结果未出时先播“正在检测网络，请稍候。”，再播报 Wi-Fi 结果 | 通过后自动继续 |
| 06 双麦与 Record | 测试侧蓝色 | 左右麦各采集 10 秒并实时回放该侧 AEC 输出，无底音；结束后停止采集回放，提示按 Record | 不设电平阈值，完全人工判断：每侧跑满 10 秒即进入下一侧，只记录电平；人工确认两侧 loopback 正常后按下并松开 Record，不按则持续等待 |
| 07 电源键 | 蓝色 | 短按底部开关键 | 新的按下并松开即通过 |
| 08 音量+ | 蓝色 | 按音量+ | 同上 |
| 09 音量- | 蓝色 | 按音量- | 同上；前面全部通过才进入老化 |
| 10 老化 | 六颗彩虹 | 只放音，循环播放与 H106 Zero BK 老化相同的旋律（523、659、784、659、587、698、880、698 Hz，每音 400 ms，无间隔，1/3 满幅），不开麦克风、不运行 AEC；Wi-Fi 保持第 05 步的连接，断开才重连；短按音量＋/－，每次增减音量 20 个百分点（0–100%，初始 35%） | 按 Record 结束，正常停止才算通过 |
| 11 等待刷固件 | 全部通过且保存成功为绿，否则黄 | 等待 | 全部通过时先保存结果，成功后亮绿并播“请等待刷入固件”；保存失败亮红，单击 Record 重试保存。未全部通过播 `incomplete`。单击音量−返回第 10 步重新老化（老化前各项须全部通过） |

记录 11 项结果：SD 卡、充电、NFC、Wi-Fi、左麦、右麦、听音与 Record 确认、电源键、音量+、音量-、老化。检测失败后亮红灯并播报失败语音。SD 卡检测通过拔插重试，其他项目按 Record 重试。

## 操作手势与进度

- **跳过**：按住 Record 再单击音量+，当前项记为 SKIPPED。这个手势不算第 08 步通过，也不算 Record 确认；有跳过项时不进入老化，第 11 步不亮绿灯。
- 只有第 11 步可以往前翻：单击音量−回到第 10 步老化，老化结果重置为未测，结束后重新进入第 11 步；老化前各项未全部通过时不响应。其他步骤没有返回和复查；第 06 步没有单独的“判失败”手势，麦克风异常时用跳过。
- **关机**：电源键长按满 2 秒即开始保存进度并进入 AP 深睡，不等待松手。电源键使用下降沿唤醒，松开本次关机按键不唤醒。
- **唤醒**：再次按住电源键满 2 秒后继续启动，不等待松手；不足 2 秒松开则立即请求重新深睡。长按判定在 MFG Runtime、外设和厂测任务启动前完成，不清除保存的进度；普通上电、软件重启不受此门槛影响。
- **清除进度并重启**：按住 Record 再短按电源键，清除已保存的进度后看门狗重启，从第 03 步开始。按住 Record 时长按电源键不关机、不清除；保存进度期间不响应。清除或重启失败时亮红灯，单击 Record 重试。

进度写入现有 MFG preference（`h2_loader_mfg_write`）：长按关机时写入，另外第 11 步十一项全部通过时先写入一次，写入成功才亮绿灯、播报等待刷固件，这样工人无需关机即可送入下一个固件并 `reboot upgrade`，重启后由 Loader Core 进入 App。开机读取后立即清除，再从第一个未通过的项继续，所以之后的复位、断电或清除手势都会从第 03 步重新开始（全部通过的设备会回到第 11 步并再次写入）；清除失败时丢弃恢复的结果并从第 03 步开始。复测跳过或失败的项，需要正常关机再开机。

## 开机反馈

MFG Runtime 初始化后、启动 Loader Core 之前，入口立即点亮全部眼灯为白色并播放开机语音。随后 Core 读取当前 Loader 身份（对镜像计算一次 SHA-256）、启动 UART 管理并完成启动检查，先完成第 02 步 SD 卡检测，再进入第 03 步；这段时间由白灯提示正在启动。长按唤醒时，满 2 秒判定通过后约 0.2 秒即有灯光和语音，无需松手。

## 安装

Send the ordinary `.update.tar.zlib` package via the management UART. Verify Stage and run `reboot upgrade` as described in [installation and recovery](./h2loader.md#installation-and-recovery). Loader and MFG retain the `h2loader` role. The operation uses the App window for the candidate, so reinstall the App after replacing the Loader if needed.

## SD card test

MFG checks the socket switch on GPIO45 after UART management starts. With no card present it plays the insert prompt and waits. After detecting a stable insertion, it mounts the card and prepares the `/dl` and `/data` directories under the shared Loader operation lock. A successful mount records the SD result for the current run.

If FatFs returns `FR_NO_FILESYSTEM`, MFG formats the inserted card once and retries the mount. Formatting erases the SD card. Format, remount or transport errors trigger a red indication and a voice prompt to reinsert the card. The UART recovery interface remains available while MFG waits. Standard Loader only mounts the card. Old manufacturing records are invalidated by the new acceptance revision.

## 4G

H202 V3.0 drops the 4G pin assignments and uses GPIO45 for SD card detect.

## 喇叭功放

功放为 AW8733A，工作档位由使能脚（GPIO53）的上升沿个数决定。SDK 的功放控制只给一个上升沿（第一档），所以板级音频自己驱动 GPIO53：先拉低 2 ms 以上让功放复位，再在关中断的情况下发 4 个 2 µs 脉冲并保持高电平，选第四档（最大音量）。停止喇叭时先拉低使能脚再停 DAC。

喇叭输入队列与 DAC 传输分开确认：板级 SDK Audio Element 工作任务使用公开 DAC/DMA 接口，保留 16 kHz、双帧 DMA 缓冲及优先级 5 / core 1。输入入队成功不触发 `on_playback`；完整 PCM 写入 DAC 传输缓冲后才触发，短写或 DMA 停滞由工作任务报告错误。`drain` 等待已提交帧全部被 DAC 传输接收，再等待最后的 DMA 帧结束；失败的停止操作保留资源供重试。

## 第 06 步测量

板级音频实现 PAL `set_aec_observer`，每帧分别给出左右两路的原始信号、参考信号和 AEC 输出。参考信号来自原理图 “Hardware AEC”：DAC 输出 `BK_AUDLP/N` 经 20 kΩ、1 µF 接回 `BK_MIC2P/N`，取自功放之前，不包含功放和喇叭的非线性。

每侧固定采集 10 秒（前 3 秒为 AEC 收敛），以 100 ms 为一段在日志中报告原始、参考和 AEC 输出的宽带电平，但不据此判定：麦克风是否正常完全由工人听回放判断。采集中途出错时自动重启音频继续本侧，连续 3 次重启仍失败才判失败。第 06 步不播放底音，也不声称自动认证回声抑制能力。

左右侧分别进行实时 loopback，没有另存录音，也没有第三段回放。两侧结束后只播放 Record 确认提示，停止麦克风和喇叭，持续等待新的 Record 按下并松开；人工听感异常时不按键即可停留，也可按跳过手势标记为 SKIPPED。Record 确认同时验证按键，并由工人确认之前两侧 loopback 的听感。老化阶段与 H106 一样只放音：早先同时采集双麦并运行两路 AEC 时，处理速度跟不上采集，几秒内采集队列就会溢出导致老化失败。

## 充电指示

SLOW_CHARGE（GPIO6）在 Loader 和 MFG 初始化时拉低，休眠保持低电平，使用快充。

白灯（GPIO37）与充电芯片没有硬件连接，MFG 读取 USB 插入、CHRG（GPIO30）和 DONE（GPIO31）后点灯：未插线灭，充电中按 2 秒完整周期呼吸（1 秒渐亮、1 秒渐灭），充满常亮，无电池但有充电器时保持 2 Hz 闪烁；欠压、温度异常也保持闪烁。2 秒内 CHRG 翻转至少 2 次视为电池未接，窗口为暂定值。呼吸通过板级 PAL PWM provider 驱动 PWM11，载波为 1 kHz；全亮、灭灯和休眠释放 PWM 后恢复 GPIO 电平。充电指示不算测试项，只在 MFG 运行时有效。

电池检测在第 03 步与充电检测同时进行：VBAT 经 R67/R66（10k/10k）分压、Q1 由 WAKE_UP 打开后接到 GPIO12（SARADC 通道 14），读数换算回 VBAT 后低于 2500 mV 视为没有电池。读取流程与 H106 Zero BK 相同（每次 acquire、配置、读取、release）。2500 mV 为暂定值，须在不装电池、只插 USB 的板子上实测后确认。

## 语音

仓库保存 30 段 8 kHz 单声道 PCM 原稿。构建时 `prompts/ima_adpcm.py` 把首尾静音裁到 80 ms，再编码为 4-bit IMA ADPCM C 数组；播放时解码并升到 16 kHz；安装包不带语音数据。“单击肚子上的按键重试。”和“单击肚子上的按键重试关机。”各只存一份，播放器在 6 条检测失败语音（含电池）和 2 条关机失败语音之后自动接上，工人听到的完整措辞不变。文案和生成方法见 `projects/h2loader/apps/mfg/h202_bk_v3/prompts/README.md`。ADPCM 会带来可闻的量化噪声，每条语音须在真机上确认听得清。

## 验收边界

真机验收需记录准确 commit、包校验值、设备 UID、SD 卡型号与 GPIO45 实测电平、NFC 地址、每侧采集电平和人工 loopback 听感确认；当前无底音模式不提供自动回声抑制量验收。
