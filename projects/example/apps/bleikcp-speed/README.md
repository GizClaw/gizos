# BLE iKCP Baseline

`bleikcp-speed` 是共享的 portable 设备间 BLE iKCP 测速 Smoke App。业务代码只定义 Server/Client 角色、测速协议、统计和屏幕投影；具体 Board、Runtime、Display 与产品 lifecycle 由 consumer launcher 接入。现有 H2Loader launchers 继续拥有 App command service、image identity 与 confirmation。

当前 launcher 覆盖 AMOLED、SZP 和 BK7258，每块 Board 都提供 Server 与 Client image。产品合同、屏幕字段和实机验收方法见 [`guides/apps/h2loader/apps/bleikcp_speed/`](../../../../guides/apps/h2loader/apps/bleikcp_speed/)。

协议单元测试：

```sh
bazel test //projects/example/apps/bleikcp-speed:all
```
