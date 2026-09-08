# SemVer

`libs/semver` 拥有跨平台版本校验和排序，只依赖 C 标准库。调用方通过 `//libs/semver` 引用，C 和 C++ 都包含 `h2_semver.h`；版本来源、升级决策和持久化由调用方负责。

## 排序规则

合法版本按 [SemVer 2.0.0](https://semver.org/spec/v2.0.0.html) 排序：依次比较 major、minor、patch，再比较预发布标识符。预发布版本低于对应正式版本；构建元数据需要满足语法，但不参与排序。

作为本库的应用约定，空字符串、`NULL` 和其他非法版本全部归入最低等级，彼此相等，并且低于所有合法版本，包括 `0.0.0-0`。比较成功不代表输入合法；需要拒绝非法输入的调用方先使用严格校验接口。带 `v` 的 tag、空白、缺失版本段和不允许的数字前导零都属于非法输入；tag 前缀的去除由调用方显式完成。

## 内存和调用边界

输入为借用的 NUL 结尾字符串或 `NULL`，在同步调用期间保持有效且不可变。库不修改、复制或保留输入，不分配堆内存，不使用全局可变状态，也不等待外部资源。数字使用长度和字节顺序比较，不转换为机器整数，因此不会因版本数字超过整数宽度而溢出。执行时间随输入长度线性增长，工作内存为常量。

本库不解析版本范围，不提供升级策略，不访问 PAL、board、SDK、filesystem 或网络。

## API Reference

[API Reference](/references/semver) 从生产 Public Header `libs/semver/include/h2_semver.h` 生成，参数、返回值和 ownership 以该 header 的 Doxygen 注释为准。

## 构建与测试

在仓库根目录运行：

```sh
bazel test --config=macos_arm64 //libs/semver:all
```

Linux 和 Windows host 分别使用 `--config=linux_x86_64` 和 `--config=windows_x86_64`。测试覆盖标准预发布排序、元数据、非法输入最低等级、超长数字和 C++ 链接。
