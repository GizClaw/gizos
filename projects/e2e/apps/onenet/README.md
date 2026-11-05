# OneNET 设备 E2E App

Portable App 通过调用方注入的 MQTT、JSON、Memory 和 Time PAL 运行公共 OneNET 库。它拥有一次连接与订阅确认、一次属性上报与平台应答、客户端关闭和清理；完整通过要求这些阶段都成功。身份、Token、属性模型与 provider 生命周期由 launcher 提供，App 不读取环境变量、文件或具体 SDK。接口与终态结果见 [Public Header](app/include/h2_onenet_e2e.h)。

A failed connection stops reporting without automatic reconnect. Control requests return an unimplemented result and do not drive physical actuators; platform rejection, timeout and cleanup failures remain failures. The DevKit carrier entry consumes this portable App.

Live calls require a platform-registered product, device and matching Key or Token, with TLS and server-certificate verification. Lingxi credentials do not automatically authorize OneNET access; any compatibility trial needs explicit caller authorization and separate configuration. Property reporting also requires the product's registered property model. Local tests do not establish live registration or device enrollment.

DevKit 的运行时配置、托管安装与恢复流程见 [carrier entry](../../targets/carrier/espressif/esp32s3_devkitc_1/README.md)。设备与 host 的结果分别记录，不能从 host 结果推断实板通过。
