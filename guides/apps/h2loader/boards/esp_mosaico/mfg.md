# ESP-Mosaico MFG

入口 `projects/mfg/apps/esp_mosaico/` 只组合板级 Runtime/USB、声明式 profile、 共享 MFG 和现有 Loader。包为 `board=esp_mosaico`、`target=esp32s31`、 `image=mfg`、`role=h2loader`；与[普通 Loader](./h2loader)互相替换。

```sh
bazel build --config=esp32s31 \
  //projects/mfg/apps/esp_mosaico:package \
  //projects/h2loader/targets/h2loader_tar_zlib/loader/esp_mosaico:package
bazel build //projects/h2loader/targets/cc_binary/cli:h2loader
```

MFG 单独启用 `H2_MOSAICO_AUDIO_AEC=ON`，并以 image-local `CONFIG_VFS_MAX_COUNT=16` 容纳 Loader 文件系统、CDC 和 Camera。 使用现有 16 MiB 布局；两个准确管理镜像都须验证不超过 3 MiB 容量。

## 安装与门控

使用同一 checkout 的 CLI，经板载 Type-C CDC 执行：

```sh
h2loader --no-ble --port <serial-port> status
h2loader --no-ble --port <serial-port> send --file <mfg-package.update.tar.zlib>
h2loader --no-ble --port <serial-port> reboot upgrade --monitor
h2loader --no-ble --port <serial-port> status
h2loader --no-ble --port <serial-port> mfg status
```

管理镜像替换/恢复优先于交互验收与 profile revision 重置。 App 安装和启动始终要求十一项全部为 PASSED；FAILED、SKIPPED、UNTESTED、 不可读结果或保存失败都不能放行。接收 Stage、安装 App、启动 App 和替换 Loader 是不同操作，接收成功不代表允许安装。普通 Loader 的入口与策略保持原样。

Loader 安装、失败、启动显示回调均为 NULL，没有品牌或安装进度界面。 按[普通 Loader 的终态检查](./h2loader#usb-管理与更新)验证运行分区、 image/package identity、metadata 与 Stage；MFG 测试界面不报告安装成功。

## 十一项操作者流程

Revision 4 保留十一项流程：显示颜色与亮度、触摸、按键、AEC、电池、IMU、磁力计、振动、Wi-Fi、BLE、相机。九个触摸点横纵等距 112 像素；按键沿用每键三次完整按下/松开及原确认逻辑。电池仅显示存在、电压和百分比。失败保留当前项，用原 ticketed retry/reset 命令补测。全部通过后显示“重新测试”：在按钮内按下并松开后，调用原 `reset all`，持久化清空所有结果、关闭 App 安装/启动门槛，并从第一项开始。保存失败时不撤销已有结果。

Wi-Fi 连接由 `apps/esp_mosaico/main/main.c` 在 Loader 启动前准备公开工厂网络，共享检查只观察已有 STA 状态；页面显示实际 SSID 和获取到的 IPv4 地址，断开时隐藏旧 IP。磁力计实时基线、补偿哨兵处理和方框反馈也由 app 拥有，公共 UI 仅消费观察回调；共享磁力计检查保持原 min/max 逻辑。app adapter 转发相对诊断磁场值以防无效轴/恢复产生伪运动，不将这些数值宣称为绝对磁场测量。

IMU 响应检查按 capability 区分加速度、陀螺仪或两者；Mosaico profile 明确要求两者。按住振动保留，松开/离页停止；页面保留振动按钮及按钮内文字，移除重叠提示和计数。相机保持异步任务、有效借帧/归还和按实际尺寸预览。

## BLE 对端

原共享 BLE 检查需要真实连接和命令往返。已有 AMOLED、DevKit、Waveshare 1.75C 的 peer package 可按 DUT UID 构建；对联板连接 DUT Loader，读当前 MFG ticket，再从真实 BLE 连接发送 `mfg probe ble <ticket>` 并回读持久结果。串口 probe、仅扫描或仅连接均不足。


## 原 AEC 诊断

保留原分阶段流程、正式测量窗口和全部门限。板上采样为 16 kHz、512 samples，处理后单声道；MFG 启用真实 AEC。app 不再增加额外准备阶段，直接执行原 QUIET/ECHO TEST/QUIET AGAIN/语音/双讲/回放检查及各阶段自带的预热。排查时加入的 `aec_prepare`、状态、日志和专用测试已删除。静音及回声检查期间保持安静，出现 SPEAK NOW 后再说话。

app 为 `mfg/aec` 轨道设置 250/1000 的数字幅度，包含语音回放；共享阶段仍请求 100% codec 音量，所有能量、reference、headroom 均观察实际 PCM。板级音频初始化、codec 时钟系数及公共 provider 均保持 main 原状；排查时加入的私有 DAC OSR 覆盖及其开关、测试替身扩展已删除。麦克风维持 18 dB 和 ADC `0xC8`。这些是本镜像的板级验收条件，不能宣称为其他音量下的声学校准。AEC 失败页面显示实际原因；双讲未满足条件仍按原逻辑重测，不提高音量到喊叫以强行通过。

共享 MFG 通过 Audio PAL 使用板级音频，不直接访问 codec 寄存器。当前共享 AEC 要求 16 kHz，现有 ES8311 provider 的采样率配置路径只支持 16 kHz / 384 倍 MCLK。锁定的官方 Mosaico BSP 中，`audio_loopback` 使用 48 kHz / 256 倍 MCLK，语音唤醒示例以 16 kHz 打开麦克风；不能只修改 MFG profile 就套用不同的时钟组合。

## 验收边界

`2a3279c9` 的十二项通过属于旧 revision 2，不能移作此 revision 4 的通过记录。`8bedf532` 的无计量 revision 4 已完成十一项，但仍包含后来删除的私有 OSR 覆盖。删除 OSR 的本地对照以及继续删除额外准备后的对照，均通过静音稳定性与回声检查，最终在双讲阶段因削波失败，不能沿用之前的通过记录。收敛后的准确镜像仍需重新验收十一项、真实 BLE 对端、原 AEC 阶段、恢复与持久化。管理镜像更新优先复用 Loader；ROM 烧录仅用于已有下载模式的初始配置/恢复，不代表管理更新通过。记录准确 commit、镜像校验值、DUT UID、端口、日志、人工结果及未测原因。
