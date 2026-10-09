# AEC 校准

`//projects/e2e/apps/aec-calibration/app:aec_calibration` 是可由产品独立 firmware entry 复用的 headless App。它只通过 Audio、Memory 和 Time PAL 使用真实音频设备，在 AEC 有效、输入／输出没有削波或超过指定 headroom、近端信号保留且处理 cadence 满足预算的条件下，搜索调用方指定的扬声器音量与麦克风增益组合。App 不依赖 board、codec、ESP-SR、WakeNet、GizClaw、网络或生产 Main，也不实现 AEC 算法。

## 组合与证据

调用方提供独占、停止状态的真实 Audio PAL、候选组合、阈值与独立近端声源 callback。Audio PAL 的可选 diagnostics contract 在同一帧提供 raw ADC、实际 AEC reference input 与 processed PCM，并独立观察混音后真正交给 DAC transport 的 PCM；SDK、codec input 到 packed lane 的 mapping、reference gain 与模拟上限仍由 provider 和 board 持有。没有真实 AEC、pre/post diagnostics 或独立近端源时返回 `UNSUPPORTED`，不从模拟数据、mic 静音、fixture EOF 或低 output energy 推断通过。

近端 callback 必须用 DUT 之外的真实声学源循环播放 App 给出的 mono S16LE probe，并确认启动／停止。不能通过覆盖 DUT `mic_read` 注入近端 PCM，也不能用 DUT 自己的扬声器同时扮演近端声源。独立源控制有显式有限 timeout，默认每次 5 秒，callback 必须遵守；runner 记录次数、总时长与最大时长，并拒绝完成过晚的调用，但不能抢占违反合同而永不返回的 callback。调用方固定并记录距离、角度、外部声源电平、设备姿态、环境、夹具版本和 DUT image／board／UID；App 借用的 probe buffer 直到独立声源停止成功后才可释放。未接夹具的产品可以保留明确的 `UNSUPPORTED`，不能把 host fake 当作物理校准。

扬声器和 mic 都使用 provider 的 0–100 控制范围。0% mic 是合法的最低增益，不假定为静音；speaker 0% 不作为优化候选。每个请求立即回读，报告 requested 与 actual 值，Pareto 与推荐只比较实测 actual 值。Codec 最大模拟增益、供电与参考输入不能由 App 越过 board 合同；ES7210 的 mic gain 调整保留独立 reference input 的增益。

## 测量

每个候选在两个幅度等级分别执行 noise、far-only、near-only、double-talk 和持续 double-talk 五个阶段，每阶段独立停止并 join DUT Audio，重新预热后计分。默认每个纯音幅度为 2048 与 4096，三个纯音相加的理论峰值不超过三个单音幅度；这不代表满幅语音压力。默认期望 far 频率约为 437、1031、2156 Hz，near 约为 719、1438、2938 Hz，实际频率取 `bin × sample_rate / frame_samples` 并随格式写入报告。不同格式可能改变实际频率，无法得到六个独立有效 bin 时明确拒绝。

默认预热 64 帧、常规计分 96 帧、持续计分 640 帧，在 16 kHz／512 samples 下分别约 2.05、3.07、20.48 秒。每次 PAL I/O 有独立有限 timeout；处理窗口的总时间还必须在真实 PCM 时长的 125% 加 100 ms 内，不能用逐帧长 timeout 把低吞吐量伪装成持续实时处理。报告保留 frames、samples 与实际 elapsed_ms。Provider 的软件 sequence 用于检查诊断交付顺序，不等于硬件 ADC 无丢帧证明；没有硬件 drop counter 的平台只能授予这里可验证的处理 cadence 资格。

所有计分阶段检查 raw mic、raw reference、缩放后 AEC reference、AEC output 和实际 DAC PCM 的 peak／headroom。默认 peak limit 为 30000，达到该阈值就拒绝，统计中的 clipped 同时包含这种保守 headroom 违规。Far-only 要求真实 reference 活性、足够高于 noise 的 raw mic 信号与默认至少 10 dB 的背景扣除后回声功率抑制。每个 near 频带在 near-only 必须活跃并保留默认至少 25% raw power；double-talk 与持续窗口还必须保留默认至少 50% near-only output power，并核对独立源输入稳定性。三个 near 频带分别检查，far 残留不能仅靠总能量代替近端证据。

数字 DAC PCM 与 raw ADC 的 headroom 不能代替模拟功放／扬声器的失真测量。

这些是受控的、频带分离的探针测量，难度低于频带重叠的人声。通过只表示此次夹具、输入电平、阈值与实测候选条件下可用；不证明绝对 dB SPL、全连续增益范围全局最大、真实语音清晰度、强风噪声或长期声场资格。实际内容峰值更高时需要重新选择压力幅度／headroom 并测量。真实近端 speech、重叠 double-talk、长期运行与产品不自对话另行验证；不能把三个频带尚有输出当作这些场景通过。

## 结果与清理

所有实际测过的候选与失败证据都保留。`PARETO_ONLY` 只返回互不支配的可用组合；`SPEAKER_FIRST` 先最大化扬声器，再在同音量中选择最高 mic；`MIC_FIRST` 顺序相反。相同 actual 值按输入顺序稳定选择。两个方向存在取舍时，不声称存在唯一的双最大值；结果从不自动应用到产品或持久化。

Runner 由 Memory PAL 分配，复制候选与 limits，借用 provider 和独立源 user。Callbacks 不等待、不分配、不调用 Audio；capture 与 playback 各自写独立测量计数，停止并 join 后才由 runner 读取。注册／撤销 observer 需要两个 worker 都停止；失败必须保留旧 registration user。任何阶段、stop、close、unregister 或恢复原控制值失败都不能授予资格。清理尽量停止独立资源；独立声源停止失败仍尝试停 DUT，track close 失败则保留其 handle 与 speaker，避免 provider 销毁 track 后重试悬空地址。失败保留 runner／probe／依赖供 `cleanup` 或 `destroy` 重试，直到成功才释放。

`h2_aec_calibration_report()` 通过调用方 writer 输出带统一 run identity 的 `AEC_CALIBRATION` JSON records，包含格式、probe、幅度、频点、阈值、请求／实际控制、每阶段完整测量、Pareto、selection、complete 与 cleanup。Writer 必须校验完整写入并自行加入换行。独立公共验证器重新核对阶段、能量／削波／near retention、cadence、Pareto 与选择策略，拒绝缺失、重复、混合 run 或矛盾的 PASS。

```sh
bazel run --config=macos_arm64 \
  //projects/e2e/apps/aec-calibration/tools:report -- \
  --log /absolute/path/aec-calibration.log --run <16-digit-hex-run>
```

退出码 0 表示完整报告中至少一个候选满足本次探针条件，1 表示报告有效但没有可用完整资格，2 表示数据无效／不完整。报告本身不证明物理设备和夹具身份；调用方必须同时保存原始日志、package/image SHA、实际 provider、独立源与摆位记录。

## 验证

`//projects/e2e/apps/aec-calibration/app:calibration_test` 用独立 fake 验证候选选择、两种优先策略、Pareto、最低 mic gain、量化 readback、静音／双讲过度抑制／无回声消除／缺 reference／reference 削波／格式错误／过快和过慢时钟，以及各资源失败后保留和重试。`//projects/e2e/apps/aec-calibration/tools:report_test` 运行同一个 C runner／formatter，再独立校验记录并拒绝被篡改的 near、reference、clipping、cadence、Pareto、selection 和 run identity。Host 结果证明执行与拒绝合同；每个实际 provider／产品仍需要原生 package 编译和真实夹具运行证据。
