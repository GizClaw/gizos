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

管理镜像替换/恢复优先于交互验收与 profile revision 重置。 App 安装和启动始终要求十二项全部为 PASSED；FAILED、SKIPPED、UNTESTED、 不可读结果或保存失败都不能放行。接收 Stage、安装 App、启动 App 和替换 Loader 是不同操作，接收成功不代表允许安装。普通 Loader 的入口与策略保持原样。

Loader 安装、失败、启动显示回调均为 NULL，没有品牌或安装进度界面。 按[普通 Loader 的终态检查](./h2loader#usb-管理与更新)验证运行分区、 image/package identity、metadata 与 Stage；MFG 测试界面不报告安装成功。

## 十二项操作者流程

profile revision 1 固定以下顺序；界面为 480×480 黑底、橙色控件与中文文字。 进度点始终保留原顺序，绿色为通过、红色为失败，跳过或未测仍显示未完成。 具体数值由 `main/profile.c` 维护。

1. **显示**：观察四个测试图案，确认渲染、颜色与方向正常。
2. **亮度**：触摸调整亮度，观察真实变化后确认。
3. **触摸**：按顺序逐个按下并松开九个目标；拖出目标、陈旧按住和跨页触摸不能计数。
4. **AI / BOOT**：先完整按下并松开 AI，再完成 BOOT。两键是被测对象，不用作全局确认。
   AI 按住时的新 BOOT 点击是显式跳过，记录 SKIPPED，不能打开 App 门控。
5. **AEC 实时监听**：进入即开始实际处理后麦克风 PCM 的采集与扬声器播放。
   就绪后说话并听声音，以人工判断正常/异常；有效静音可作为 PCM，不能靠幅度自动通过。
   不调整麦克风增益，不代表定量回声抑制或受控声学标定。
6. **电池**：观察真实 presence、电压、有符号电流和电量百分比；接外电检查充电/满电指示，
   再移除外电确认仍由电池工作。满电不要求正充电电流；插拔与充电指示由人工判断，
   不从电流符号、USB 或 DTR 推断插拔，也不写电池 profile。
7. **IMU**：移动/转动板子观察 BMI270 加速度和陀螺仪响应，就绪后人工确认。
   使用原生响应，没有未经真机验证的屏幕轴映射。
8. **双磁力计**：分别将磁铁靠近和移开右侧 202、左侧 203；对应橙色方块填充并恢复轮廓。
   读取错误单独显示；补偿溢出哨兵不表示吸附，恢复后的首次有效轴只重建基线。
9. **振动**：触摸按住驱动电机，松开即停；确认已实际振动并停止，再给出人工结果。
10. **Wi-Fi**：通过现有 Loader `wifi connect <ssid> <password>` 配置工厂 AP，
    等待显示真实可用 IPv4，再人工确认。固件不含凭据，不要求回环服务。
11. **BLE 扫描**：进入即扫描周边，观察实际设备标识/名称和 RSSI 后确认。
    没有观察、扫描或停止错误不能通过；无需配对、GATT 或第二块板，未启用 BLE 命令服务。
12. **摄像头**：断电时预先接好左 H2 的 SC101IOT；进入即启动真实 1280×720 UYVY
    采集并显示等比例 384×216 预览。移动物体，确认动态画面、颜色、方向和比例后通过。
    缺摄像头或生命周期错误是失败；安装/拆卸必须断电。

初始显示/亮度/触摸之后用触摸导航。触摸和按键的完整新输入序列可推进对应项， 所有主观验收都须明确确认。启动失败、超时或 unsupported 保留实际错误，不能静默跳过。 外设操作有边界，等待操作者没有自动超时或自动结果；保持监听直到动作完成或明确取消。

## 结果、恢复与重新测试

进度经现有 Loader MFG preferences 持久化，重启恢复。 `mfg status` 的 `steps` 数字为 0=UNTESTED、1=PASSED、2=SKIPPED、3=FAILED； `complete=1` 才表示全部 required checks 已通过，`result=OK` 仅表示命令成功。 使用当前 step 和每次转换/启动更新的八位十六进制 ticket：

```sh
h2loader --no-ble --port <serial-port> mfg status
h2loader --no-ble --port <serial-port> mfg confirm <step> <ticket>
h2loader --no-ble --port <serial-port> mfg fail <step> <ticket>
h2loader --no-ble --port <serial-port> mfg retry <step> <ticket> --monitor
h2loader --no-ble --port <serial-port> mfg skip <step> <ticket>
h2loader --no-ble --port <serial-port> mfg reset all <ticket>
```

未就绪或旧 ticket 的确认被拒绝。失败保留当前项，明确 RETRY 才开始新观测； SKIPPED 仍阻止 App，可通过 retry 重测。保存失败不能发布通过或推进。 语义/顺序变化须提升 revision；不导入旧 Firmwares 的私有结果格式。

遍历结束显示“测试完成”“等待固件刷入”和“重新测试”，清除旧摄像头画面。 仅全部 PASSED 才显示“全部通过”；等待标签不能证明门控已打开。 暖态及冷启动完成页均可重新测试，按共享 `reset all` 先持久撤销验收再回到显示。 AUTO 中全部通过且确有 App 工作时，释放资源返回 Loader，不为等待页阻塞 App。 安装/重启前共享 cleanup 停止监听/扫描/振动、归还 Camera 帧、释放触摸和显示； 清理失败保留资源并阻止交接，重测与安装使用现有共享操作锁串行化。

## 待记录的验收证据

此页没有新 GizOS 包的构建、烧录或硬件通过记录。原 Firmwares checkpoint `abae140f50234c85428fc109c8b6c07a4a0a1bad` 仅用于行为参考。 验证时记录准确 GizOS commit、package SHA-256、板 identity、USB 端口、串口日志和人工结果。

运行共享回归（含 `//projects/mfg/libs/mfg:network_checks_test`）、Loader/host/CLI 与 Mosaico USB/磁力计回归；完整命令见[共享 MFG README](https://github.com/GizClaw/gizos/blob/main/projects/mfg/README.md)。 构建两个准确 S31 包并核对容量后，在实体板执行全部十二项、失败/retry/skip、 重启恢复、保存失败、暖态/冷态重测、磁铁移入移出与哨兵恢复、真实音频、拔外电、BLE 扫描和相机动态画面。

再用既有 CLI 验证 MFG ↔ 普通 Loader 替换、未完成时 App 拒绝和 Loader 修复、 cleanup 失败、重测与安装竞争、完成后 App stage/install/launch、返回 Loader 及恢复。 每次均核对实际运行分区与 image identity、metadata、Stage 终态和 `last_result`。 原生烧录仅用于初始配置/恢复，不能替代管理链路验收；未执行项记录 SKIP/UNSUPPORTED 及原因。
