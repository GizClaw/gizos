# Waveshare ESP32-S3-A7670E-4G Modem Smoke

## 构建

```sh
bazel build --config=esp32s3 \
  //projects/example/targets/h2loader_tar_zlib/modem-smoke/waveshare_esp32s3_a7670e_4g:package
```

## 诊断流程

App 依次记录 `open`、`identity`、`sim`、`registration`、`ppp`、`ppp_status`、`icmp` 和 cleanup 阶段。`identity` 包含 A7670E manufacturer、model、revision 和 15 位 IMEI；`ppp_status` 在 data session 建立后记录 PAL state 和 IPv4 address，只有地址有效时才继续 ICMP。

没有 SIM 时，identity 和 IMEI 仍应被读取；SIM 状态记录为 absent 或 unavailable，registration、PPP 和 ICMP 记录 `attempted=0 reason=sim_unavailable`。插入可用 SIM 后，App 等待有限时间完成网络注册和 PPP，随后通过 portable Net PAL 向 `1.1.1.1` 发出一次有超时上限的 ICMP echo，并记录 transmitted、received 和 elapsed time。无论中间阶段结果如何，App 都会尝试关闭 PPP、恢复原 default netif 并关闭 Modem。

该 App 不主动拨号。真实语音呼叫只有在提供非紧急测试号码并明确授权后，才可以通过 Modem PAL 单独操作。
