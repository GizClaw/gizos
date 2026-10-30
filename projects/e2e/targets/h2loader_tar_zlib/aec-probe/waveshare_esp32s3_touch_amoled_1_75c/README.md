# 1.75C 单机回声与增益探针

此独立 H2Loader App 复用原 H200 单机探针的三音信号和静音基线扫描方法，
通过真实 Audio PAL 同时播放、采集并观察 raw ADC、实际 AEC reference、
processed PCM 和已完成的 DAC 写入。它不启动 GizClaw 或注入麦克风 PCM。

```sh
. /path/to/firmwares-devenv/export.sh
bazel build --config=esp32s3 \
  //projects/e2e/targets/h2loader_tar_zlib/aec-probe/waveshare_esp32s3_touch_amoled_1_75c:package
```

使用 H2Loader 管理安装，先核对 board 和设备 UID。设备将发出约三分钟的间歇
校准音；测试期间保持扬声器、麦克风位置和环境稳定。原始串口日志绑定 App
版本、UID 和随机 run。测试完成后停止并 join 音频、关闭 track、撤销 observer，
再确认 App。失败保留错误，成功完成测量不等于所有候选都有效。

四个实际 direct-AEC profile 分别测单麦／双麦与 0／12 dB reference PGA。
每个 profile 在 75／100% speaker 和约 18／24／30 dB mic 的若干组合中记录
10 个有序静音／far-only 窗口，四组共 40 个。PAL mic 百分比按 codec 的实际量化值回读；
reference PGA 不随 mic 调节改变。原板默认配置不因运行探针而持久化。
每个 far 窗口预热 96 帧、计分 96 帧，静音预热 32 帧、计分 64 帧；
16 kHz、512 samples。检查完整实际 DAC 帧、原始输入、AEC 输出和实时 cadence。
日志只保留功率、peak 和 clipping 统计，不保存麦克风 PCM。

```sh
python3 projects/e2e/targets/h2loader_tar_zlib/aec-probe/waveshare_esp32s3_touch_amoled_1_75c/analyze.py \
  --log /absolute/path/serial.log --uid <device-uid> --version <image-version>
```

分析器拒绝身份、时序、声道、窗口或 DAC 证据不完整的日志，并独立检查静音
背景稳定性、signal/noise、reference 活性、30000 peak headroom 和至少 10 dB
的背景扣除后 far-only 回声功率降低。`far_echo_probe_passed` 仅针对这次单机
信号和电平；它始终保留 `acoustic_qualified=false`。数字 headroom 不证明模拟
功放／扬声器没有失真。真实近端保留和双讲需要另行实测；完整自动验收使用
`projects/e2e/apps/aec-calibration` 及其独立真实近端声源，不复用此探针的局部通过。

基于实际日志选择参数后修改 BSP，再重新构建和实机验证相关 Audio／GizClaw
镜像；不能把探针临时 profile 或旧镜像的成功当作新默认配置已验证。
