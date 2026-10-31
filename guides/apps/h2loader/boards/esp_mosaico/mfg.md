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

Revision 3 复用原共享 model/session/AEC；显示颜色与亮度合并为一项，之后依次为触摸、按键、AEC、电池、IMU、磁力计、振动、Wi-Fi、BLE、相机。触摸点由 app profile 提供；按键沿用每键三次完整按下/松开及原确认逻辑。电池仅显示存在、电压和百分比。失败保留当前项，用原 ticketed retry/reset 命令补测；完成页仅显示原结果摘要。

Wi-Fi 连接由 `apps/esp_mosaico/main/main.c` 在 Loader 启动前准备公开工厂网络，共享检查只观察已有 STA 状态。磁力计实时基线、补偿哨兵处理和方框反馈也由 app 拥有，公共 UI 仅消费观察回调；共享磁力计检查保持原 min/max 逻辑。app adapter 转发相对诊断磁场值以防无效轴/恢复产生伪运动，不将这些数值宣称为绝对磁场测量。

IMU 响应检查按 capability 区分加速度、陀螺仪或两者；Mosaico profile 明确要求两者。按住振动保留，松开/离页停止。相机保持异步任务、有效借帧/归还和按实际尺寸预览。

## BLE 对端

原共享 BLE 检查需要真实连接和命令往返。已有 AMOLED、DevKit、Waveshare 1.75C 的 peer package 可按 DUT UID 构建；对联板连接 DUT Loader，读当前 MFG ticket，再从真实 BLE 连接发送 `mfg probe ble <ticket>` 并回读持久结果。串口 probe、仅扫描或仅连接均不足。


## 原 AEC 诊断

保留原分阶段流程和全部门限。板上采样为 16 kHz、512 samples，处理后单声道；MFG 启用真实 AEC。原阶段声源/回放用 100% 扬声器，而旧实时监听用了 50%，仅此差异不能证明失败原因。先采集 QUIET/ECHO TEST/QUIET AGAIN 的 `H2_MFG_AEC_WINDOW`、能量、reference、peak/clipped、时间和最终原因，再判断参数是否匹配，不降低门限或用监听成功替代定量结果。

## 验收边界

`2a3279c9` 的十二项通过属于旧 revision 2，不能移作此 revision 3 的通过记录。新镜像需重新验收十一项、真实 BLE 对端、原 AEC 阶段、恢复与持久化。管理镜像更新优先复用 Loader；ROM 烧录仅用于已有下载模式的初始配置/恢复，不代表管理更新通过。记录准确 commit、镜像校验值、DUT UID、端口、日志、人工结果及未测原因。
