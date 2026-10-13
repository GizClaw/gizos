# Application Host E2E

该 headless App 使用同一个 Application Host entry contract 报告 ready 并响应协作停止。它不初始化 Runtime、不选择 BSP，也不拥有 H2Loader 或设备操作。retained 时 launcher 保持 owner task/context 存活，避免 BSP wrapper 在返回后释放仍被借用的 provider。调用方提供执行预算；两个 SDK launcher 使用 100 ms 的 run_ms，并检查 application、cleanup 与 retained 结果。接口见 [生产头文件](app/include/h2_application_host_e2e.h)。

## 构建与验收

```sh
bazel build --config=esp32s3 //projects/e2e/targets/h2loader_tar_zlib/application-host/devkit:package
bazel build --config=bk7258 //projects/e2e/targets/h2loader_tar_zlib/application-host/bk7258_v3_202405:package
```

这两个入口验证真实 SDK 编译、链接和打包；没有 device runner，也不自动确认、flash、reset 或控制物理设备。`APPLICATION_HOST_SDK rc=0 retained=0` 是将来实际执行时的成功条件，build 成功本身不证明该日志或硬件清理已经发生。通用故障、Desktop 与真实浏览器验收由 [Application Host 指南](../../../../guides/zh/developing/application_host.md) 中的独立测试覆盖。
