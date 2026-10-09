# Audio AEC Calibration PAL contract

Audio PAL 的 `set_aec_observer` 是可选声学诊断 operation；所有 provider 仍提供明确的真实实现或 unsupported stub，不以缺失 slot 表示支持状态。真实实现只在 AEC 实际处理时同步观察同帧 raw ADC、实际缩放后 reference input 和 processed PCM，同时标明 sequence、mic lane/mask 与 reference lane；单独的 playback callback 观察混音／数字增益之后、完整交给 DAC transport 的 PCM。ADC input 与 packed lane 的映射留给 provider，App 不接触 codec SDK。PCM 只在 callback 期间借用，不写入测量日志；mic mask 所有实际输入都参与 clipping/headroom 检查。

注册 descriptor 被复制，user 借用到成功撤销。注册、替换与撤销需要 mic 与 speaker 已停止且 worker join；失败保留旧 registration，调用方不能提前释放 user。两个 callback 可以互相并发、各自串行，禁止等待、分配或重入 Audio。对应成功 stop 之后不再有该类 callback。Runtime 与 ESP lazy proxy 保留 forwarding；替换 mic PCM 的 Testing Audio decorator 明确返回 `UNSUPPORTED`，真实校准直接借用底层 provider。ES8311+ES7210 实现该 contract；当前无完整 pre/post 诊断的其它 provider 返回 `UNSUPPORTED`，不因此否定其普通 Audio 功能。

公共 AEC Calibration App 以此合同通过 PAL 同时播放和采集，并在独立近端声源参与下验证回声抑制、近端保留、raw/reference/output/DAC headroom、处理 cadence 与候选取舍。单看低 output energy、启用 AEC 的 flag 或 fixture EOF 静音不能授予声学资格。它的 consumer、探针限制和报告合同见 [E2E 测试 App](/apps/e2e#aec-校准)。

