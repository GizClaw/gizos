# 灵犀设备 E2E App

Portable App 通过注入的 HTTP、JSON、Memory 和 Time PAL 运行公共灵犀库。它拥有有序 case、独立结果、每 case 的有限执行预算、依赖失败处理、进度回调和临时设备凭据清理；不读取环境变量、文件、具体操作系统或 board。Host 和未来设备 launcher 使用同一个 [App contract](app/include/h2_lingxi_e2e.h) 与 [case registry](app/include/h2_lingxi_cases.inc)，具体 provider 与网络生命周期由 launcher 拥有。

App 先获取已登记设备的凭据，再发送一次调用方提供的心跳。查询失败时心跳记为 BLOCKED，继续执行清理；任何 FAIL、BLOCKED、超时或 launcher teardown 失败都不能算通过。结果只包含 case、PAL 返回值、HTTP／业务状态与耗时，不包含产品 Key、设备 Secret 或原始响应。固定账本是一次设备 HTTP 接入验证，不代表模型接入、OneNET 验证或物理设备入库。

## Host 入口

`projects/e2e/targets/cc_binary/lingxi` 组装真实 CoreHTTP、yyjson 和 Darwin／Linux Net PAL。所有接入数据由进程环境注入，入口解析后传给 App；没有位置参数或必需的参数文件。必需与可选环境变量从入口的 usage 读取：

```sh
bazel run --config=macos_arm64 //projects/e2e/targets/cc_binary/lingxi:lingxi -- --help
```

产品、设备号与编码类型使用平台已登记的测试身份。心跳 JSON 是调用方按协议组装的 params 对象；主机模拟测试使用明确的测试版本标记、测试机观察值和协议允许的缺省值，生产验收使用真实设备数据。凭据保存在调用方本机的受控配置或进程环境，不进入 Git、BUILD、CMake 或编译参数。

设置 usage 要求的环境后，手动运行实网测试：

```sh
bazel test --config=macos_arm64 \
  --test_env=LINGXI_BASE_URL \
  --test_env=LINGXI_PRODUCT_ID \
  --test_env=LINGXI_PRODUCT_KEY \
  --test_env=LINGXI_DEVICE_NO_TYPE \
  --test_env=LINGXI_DEVICE_NO \
  --test_env=LINGXI_SOURCE_IP \
  --test_env=LINGXI_HEARTBEAT_JSON \
  //projects/e2e/targets/cc_binary/lingxi:live_test
```

可选配置也通过同名 `--test_env` 继承。`live_test` 声明 `manual`／`external`，不进入默认自动测试，不复用上次实网测试结果，构建缓存保持启用。实网入口始终验证 TLS，拒绝明文与重定向。一般 host executable 仅为隔离的 loopback fixture 提供显式明文开关。

## 自动验证与边界

```sh
bazel test --config=macos_arm64 //projects/e2e/apps/lingxi/app:all
bazel test --config=macos_arm64 //projects/e2e/targets/cc_binary/lingxi:wire_test
```

App-local tests 验证依赖失败、时钟错误、deadline、账本与资源清理。Wire test 经真实 CoreHTTP 和 host Net 验证环境注入、完整查询后心跳、业务拒绝、身份不匹配、输入与响应预算、重定向拒绝以及实网入口的 TLS 门禁；只连接隔离的本机 fixture，不读取外部凭据。macOS／Linux host 与 [DevKit carrier entry](../../targets/h2loader_tar_zlib/carrier/devkit/README.md) 复用同一 App；设备入口从 Runtime 组装心跳并消费一次性运行时配置。每个平台的实测结果绑定实际镜像和运行记录，不能互相替代。
