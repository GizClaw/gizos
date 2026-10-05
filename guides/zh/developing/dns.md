# DNS

`libs/dns` 提供跨平台 DNS message codec 和 UDP DNS client，支持 A 与 AAAA record。

## API Reference

[API Reference](/references/dns)

`libs/dns/include` 中实际参与项目构建的头文件是 DNS 的生产 Public API contract。Client config 注入 PAL network、crypto 和 time API，同时提供 DNS server、可选 bind、timeout 与 retry 次数。

## 依赖和边界

DNS 不直接调用 socket、随机数或系统时钟实现。网络传输、transaction ID 所需的随机能力和时间能力都通过 PAL 注入；DNS server 的选择由调用方决定。

## 构建与测试

```sh
bazel test //libs/dns:all
```

IPv6 DNS client 使用 server 的实际地址族和 scope 创建 UDP socket，并验证回复的地址、端口和 scope。`pal-ipv6` 的私有 `.test` 权威夹具提供真实 IPv6 UDP AAAA 往返；系统 resolver 的保留 localhost 名字测试与 DNS wire 测试分开记录。
