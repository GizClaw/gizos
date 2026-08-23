# BK3633 BinConvert

本目录保存 portable C BinConvert，实现 BK3633 firmware 使用的 vendor `BinConvert -oad` 兼容格式。它在 macOS 和 Linux host 上直接编译运行，不依赖 Docker、firmware-devenv 中的 host executable 或已提交二进制。

支持的命令形态为：

```sh
binconverter -oad boot.bin stack.bin app.bin \
  -m 0x1f00 -l 0x2e200 -v 0x2a -rom_v 0x0f \
  -e 00000000 00000000 00000000 00000000
```

工具始终生成与 application input 同目录的 `app_merge_crc.bin` 和
`app_oad.bin`。只有 stack + application 的完整 OAD 长度能够由协议中的
16-bit word count 表示时，才生成 `app_stack_oad.bin`；超限时会把 merged
image 中的全量 OAD header 保持为 erased 状态并明确报告省略该文件，避免像
vendor executable 一样静默截断长度并输出不可验证的 package。只实现
GizOS 需要的 zero-key `-oad` 模式；其他 vendor mode 和非零 encryption
key 会明确失败。

从仓库根目录构建并测试：

```sh
bazel build //tools/bk3633_binconverter:binconverter
bazel test //tools/bk3633_binconverter:test_binconverter
```

BK3633 launcher 通过 Bazel graph 构建并执行 host tool，再使用 launcher 自己
`build/` tree 中的输入生成 recovery image；生成的 executable 和 image 都不会提交。
