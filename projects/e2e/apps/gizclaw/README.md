# GizClaw E2E

`gizclaw` 是 `projects/e2e` 持有的 target-independent、headless、阻塞式测试 App。它直接验证 `libs/gizclaw` 的连接、RPC、Firmware 下载、语音流和有界并发能力，不包含 H106 Main、MFG、UI、产品 RuntimeProfile 断言或目标平台的网络连接策略。

Launcher 提供已经初始化的 `h2_runtime_t`、真实 RegistrationToken、服务 endpoint、suite mask 和可选的 16 kHz mono S16LE 测试音频。App 不读取环境变量或文件，不选择 AP/BJ，不创建 Wi-Fi task，也不拥有 Pion/H2Peer provider。RegistrationToken 原样用于注册；注册响应返回的 RuntimeProfile identity 只作为结果 evidence 记录，不与产品常量比较。

一次 `h2_gizclaw_e2e_run()` 调用按固定顺序运行所有选中的独立 case。某个 case 失败不会阻止后续独立 case；取消会把尚未运行的 case 标为 `CANCELLED`。App-owned runner task 执行 case，调用 `h2_gizclaw_e2e_run()` 的 task 至少每 10 秒通过 Runtime Log 和同步 progress observer 报告进度。结果包含 terminal counts、首个失败、cleanup result、retained resource count 和注册返回的 RuntimeProfile。未清理资源只按资源类型写入 redacted recovery ledger，不记录资源名称。正常返回前会完成反向清理、为每个选中 case 生成唯一 terminal record，并在 runner 确认退出后 join。Cleanup deadline 限制已退出 runner 的 PAL handle 释放重试；如果 PAL 仍拒绝释放 handle，App 返回 harness error、保留 handle/context 并保持全局 run guard。此时 runner 已退出，不会在返回后访问 config、Runtime object 或 caller observer。

`service` case 将 fixture 的一个独立 Peer identity 从 direct client 转交给真实 `h2_gizclaw_service_t`。Service network task 创建、连接并注册 client；App runner 是唯一 dispatch consumer，并断言 completion callback 的顺序与唯一性。显式 stop 后必须完成 drain、handle release 和 deinit，fixture 再使用同一 identity 执行普通 Peer 反向清理。这个 case 不依赖 H106 Main、产品 RuntimeProfile 或 UI observation。

Desktop live test 位于 `projects/e2e/targets/cc_test/gizclaw/`。H2Peer 与 Pion 分别由独立的 `manual` `cc_test` 装配 Desktop Runtime/PAL，从 `H2_GIZCLAW_E2E_REGISTRATION_TOKEN` 读取真实 token，加载确定性 PCM，然后调用同一个 portable entry。Token、private key、authorization metadata、Firmware URL、原始音频和响应正文不得进入日志或 artifact。

未来 firmware launcher 负责 Wi-Fi credential、重连 task、Runtime event main loop、image/package 和结果传输。`H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_GOT_IP` 第一次出现后，launcher 在独立 runner task 中启动 App；`LOST_IP` 或 `DISCONNECTED` 只更新网络状态，同一次 boot 不启动第二个 runner。
