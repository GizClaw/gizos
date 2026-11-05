# BLE iKCP Baseline

`bleikcp-speed` 是共享的 portable 设备间 BLE iKCP 测速 Smoke App。业务代码只定义 Server/Client 角色、测速协议、统计和屏幕投影；具体 Board、Runtime、Display 与产品 lifecycle 由 consumer launcher 接入。现有 H2Loader launchers 继续拥有 App command service、image identity 与 confirmation。

AMOLED, SZP, and BK7258 launchers each provide Server and Client images. See the [App sources](app/) for protocol, statistics, and rendering, and the [launcher packages](../../targets/h2loader_tar_zlib/) for board wiring.

协议单元测试：

```sh
bazel test //projects/example/apps/bleikcp-speed:all
```
