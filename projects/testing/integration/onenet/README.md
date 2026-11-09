# OneNET device E2E App

The portable App runs the public OneNET library through caller-injected MQTT, JSON, Memory and Time PAL. It owns one connection/subscription acknowledgement, one property report/platform response, client shutdown and cleanup. Full success requires every phase to succeed. The launcher supplies identity, token, property model and provider lifetime; the App does not read environment variables, files or concrete SDKs. See the [public header](app/include/h2_onenet_e2e.h) for interfaces and terminal results.

A failed connection stops reporting without automatic reconnect. Control requests return an unimplemented result and do not drive physical actuators; platform rejection, timeout and cleanup failures remain failures. The DevKit carrier entry consumes this portable App.

Live calls require a platform-registered product, device and matching Key or Token, with TLS and server-certificate verification. Lingxi credentials do not automatically authorize OneNET access; any compatibility trial needs explicit caller authorization and separate configuration. Property reporting also requires the product's registered property model. Local tests do not establish live registration or device enrollment.

See the [carrier entry](../../targets/h2_loader_tar_zlib/integration/carrier/espressif/esp32s3_devkitc_1/README.md) for DevKit runtime configuration, managed installation and recovery. Device and host results are recorded separately; host results cannot establish physical-board success.
