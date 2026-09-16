# BLE Wi-Fi Config API

默认凭据配网通过 `h2_runtime_wifi_connect_and_save(api.runtime, config, timeout_ms)` 执行，调用方须提供有效且覆盖 service 生命周期的 Runtime。PAL 连接与单条凭据保存成功后，手机配置的网络进入 Runtime 的最多 8 条保存集合，按 SSID 去重并置顶，满额淘汰末尾；PAL 失败不添加记录。Runtime 集合写入失败也报告配网失败，此时 PAL 可能已经更新单条凭据。

集合存储在 Runtime 的独立 pref namespace，固定 blob 为 920 字节。排序依据是存储位置，未校准时的时间戳为 0。BLE 不维护另一份网络列表，也不改变 PAL 的单条凭据契约。关联/IP 验证沿用 PAL `connect_and_save`，Runtime 不重复等待 IP；自定义配网回调仍由应用负责其持久化策略。

<!--@include: ../.generated/api/ble_wifi_config.md-->
