# SQLite Provider

`//libs/pal/providers/sqlite:sqlite` 把仓库固定版本的 SQLite 适配为 Preference PAL。它是 filesystem-backed reusable provider，不属于 Desktop `pal_core`。

## Instance 与路径

`h2_sqlite_create()` 校验并复制 database path，创建 opaque instance；`h2_sqlite_pref_api()` 返回绑定该 instance 的 Preference PAL；`h2_sqlite_destroy()` 在所有 consumer、transaction、statement 和 cursor 结束后关闭 database 并释放 path/state。调用方不能在 provider 销毁后继续保存 API pointer。

Provider 负责 namespace/key 编码、transaction 边界、statement reset/finalize、SQLite result 到 PAL result 的映射，以及失败路径 cleanup。Database 放置位置由 platform composition 决定；Desktop `app_support` 使用每个 executable 的隔离 storage root，provider 不读取环境变量或 bundle policy。

## Validation

```sh
bazel test --config=macos_arm64 //libs/pal/providers/sqlite:all
bazel test --config=linux_x86_64 //libs/pal/providers/sqlite:all
```

测试使用临时 database 覆盖 path copy、read/write/remove、transaction/cursor cleanup、重复 create/destroy 和失败恢复。
