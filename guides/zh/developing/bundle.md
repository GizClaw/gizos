# Bundle

`libs/bundle` 提供跨平台 bundle 解析、校验和安装能力，用于处理 manifest、tar archive、路径安全、版本记录和 OTA 数据布局。

## API Reference

[API Reference](/references/bundle)

`libs/bundle/include` 中实际参与项目构建的头文件是 Bundle 的生产 Public API contract。写入 app image 时由调用方提供 `h2_bundle_app_writer_t`；清理 data root 时由调用方提供 callback。Bundle 本身不决定 partition、启动目标或升级策略。

## 依赖和边界

Bundle 使用 PAL filesystem 和 mem API，依赖 `libs/pixa` 以及 `@h2_vendor_zlib`。文件系统挂载、partition layout、boot policy 和 image 切换由 H2Loader、launcher 或 BSP 负责。

普通 `data/` entry 由 Bundle 原样安装。以 `.pixa` 结尾的 entry 是明确的安装期转换输入：Bundle 先校验 archive 中的源 entry，再通过 PAL-backed PIXA extractor 写入同路径加 `.d` suffix 的 directory。Extractor 删除旧 completion marker，写入 `clips/*.argb4444`，最后发布 `index.bin`；App 只消费完成的 `.pixa.d`。

顶层 checksum 按 package 中全部 `data/` entry 的路径和源内容计算，包括展开前的 `.pixa`。安装成功后持久化该 checksum，用于判断相同 package data 是否已经安装；不能用展开后文件树重新定义 package identity。

## 构建与测试

Bazel semantic target 为 `//libs/bundle:bundle`：

```sh
bazel test //libs/bundle:all
```
