# 北京入口 Default DevKit GizClaw E2E 错误报告

## 测试基线

- 日期：2026-08-13 CST
- Pull Request：Firmwares `#825`
- 设备：DevKit ESP32-S3，通过 H2Loader 安装并在测试后回滚到 Loader
- 入口：北京 E2E（`edge-bj-01.e2e.gizclaw.com:9821`）
- Runtime Profile：`default`
- RegistrationToken：使用该 profile 自己的 `deploy-default` test token
- WebRTC backend：H2Peer
- GizClaw C SDK：正式 tag `v0.3.1`，commit `ddf02c1743eb3a12524523156b7f1fbf45132427`
- 最终结果：`4 PASS / 1 FAIL`，`5/5` case 均产生 terminal result，cleanup PASS，残留资源为 0

## 错误

### Issue SERVER-1：Firmware metadata 指向不可下载的 artifact

- 归属：第 5 层 GizClaw Server / E2E artifact publication 或 storage 配置。
- 现象：`ServerFirmwareGet` 成功返回 metadata，并声明 artifact 大小为 1 byte；随后 HTTP 下载返回 `H2_PAL_ERR_NOT_FOUND`，实际得到 0 byte。
- 影响：Firmware 下载测试失败，无法验证 metadata、声明 size 和实际 artifact 内容一致。
- 已排除：Connectivity、RPC、Voice、并发、H2Peer channel cleanup 和 ESP HTTP transport 的其他覆盖均通过；客户端没有改写 Server 返回的 metadata，也没有添加 placeholder fallback。
- 结论：不是第 1 至第 3 层 Firmwares 问题，也没有证据指向第 4 层 GizClaw C SDK。
- 下一步：部署侧发布真实可下载 artifact，并确保返回 URL、声明 size 和实际内容一致后重跑 Firmware case。
