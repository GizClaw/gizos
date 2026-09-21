# Encoding

`libs/encoding` 拥有跨平台的二进制到文本编解码，设计参照 Go 的 `encoding/hex`、`encoding/base32`、`encoding/base64` 和 `encoding/ascii85`，只依赖 C 标准库。调用方通过 `//libs/encoding` 引用，C 和 C++ 都包含 `h2_encoding.h`。它不是 PAL capability，也不进入 Runtime。

## 编码描述

所有编码共用一组函数，通过预先准备好的 `h2_encoding_t` 描述选择编码：

- `kind` 选择算法族，决定进制和分组：hex（每字节 2 个符号）、base32（5 字节对应 8 符号）、base64（3 字节对应 4 符号）、base85（4 字节对应 5 个大端 85 进制数字）。
- 字母表按数字值给出符号，长度必须恰好等于进制，符号互不相同且都是 0x21–0x7e 的可打印 ASCII。
- `padding` 是 base32/base64 的块填充符号，`'\0'` 表示不填充。
- `zero_group` 是 base85 的全零分组缩写（Ascii85 的 `z`），`'\0'` 表示不使用。

描述里同时保存字母表和 256 字节的解码表，共 348 字节，是可以按值复制的普通数据。库预置 `h2_encoding_hex`、`h2_encoding_base32_std`、`h2_encoding_base32_hex`、`h2_encoding_base64_std`、`h2_encoding_base64_url`、`h2_encoding_base64_raw_std`、`h2_encoding_base64_raw_url`、`h2_encoding_base85_ascii85` 和 `h2_encoding_base85_rfc1924`，分别对应 RFC 4648、Adobe Ascii85 数字部分和 RFC 1924 字母表（与 Python `base64.b85encode` 输出一致）。预置描述的解码表写在只读数据中，测试逐项核对它们与 `h2_encoding_init()` 的结果一致。

自定义编码调用 `h2_encoding_init()`，它只在这一次校验字母表并建表，相当于 Go 的 `NewEncoding`；传入预置描述的 `alphabet` 并换一个 `padding`，相当于 `WithPadding`。描述字段只读；手工修改的描述会得到未定义的输出，但库不会读写调用方缓冲区之外的内存。

## 严格解码

编码输出总是规范形式。解码只接受规范输入：空白、NUL、解码表外符号、错位或缺失的 padding、不完整的末尾分组、末尾非零填充位、超过 `0xffffffff` 的 base85 分组、非规范的 base85 末尾分组，以及配置了 `zero_group` 时用数字写出的全零分组都视为损坏。Hex 例外地接受字母表字母的另一种 ASCII 大小写（该字符本身不在字母表中时）。本库不处理换行、Ascii85 的 `<~ ~>` 帧或其他外层格式，这些由调用方先行剥离。

## 内存和调用边界

输入和描述都是借用的，只在同步调用期间有效；输出缓冲区由调用方提供且不能与输入重叠。空间不足时 `out_len` 返回精确所需大小，因此可以先以零容量查询大小；输入损坏时 `out_len` 返回第一个出错字节的偏移，输入提前结束时为输入长度；损坏优先于空间不足报告。出现这两种错误时输出缓冲区内容未定义，参数非法时输出不被触碰。

编码和解码都只扫描输入一遍，每次调用没有建表或校验描述之类的固定开销，逐块调用也不会额外变慢。库不分配堆内存，不使用可变全局状态，不等待外部资源，栈上只有常数大小的局部变量。

分块编码时，除最后一块外每块长度取块大小的整数倍（base64 3 字节、base32 5 字节、base85 4 字节，hex 任意），拼接结果与一次性编码相同。解码可以按完整分组逐组调用，例如边读边解的 base85 消费方每次传入 5 个字符。本库不提供流式 reader/writer。

## 性能

`//libs/encoding:benchmark_encoding` 用 `clock()` 统计 CPU 时间，每个样本处理 512 KiB 原始字节，预热 4 次、采样 64 次，按原始字节报告 p50/p95 的 ns/byte 和 MiB/s。它覆盖 hex、base32、base64、两种 base85 在 64 B、4 KiB、64 KiB 下的编码和解码，base64 按 4 字符、base85 按 5 字符逐组解码，以及不做任何校验的 base85 查表循环作为下限参考。

Host 结果只用于比较实现和发现回退，不等于嵌入式设备上的结果。设备测量需要记录 board、CPU 频率、toolchain、数据所在内存和优化级别。

## API Reference

[API Reference](/references/encoding) 从生产 Public Header `libs/encoding/include/h2_encoding.h` 生成，参数、返回值和 ownership 以该 header 的 Doxygen 注释为准。

## 构建与测试

在仓库根目录运行：

```sh
bazel test --config=macos_arm64 //libs/encoding:all
bazel run -c opt --config=macos_arm64 //libs/encoding:benchmark_encoding
```

Linux 和 Windows host 分别使用 `--config=linux_x86_64` 和 `--config=windows_x86_64`。测试覆盖 RFC 4648 与 Python 生成的向量、每类损坏输入及其偏移、损坏优先于空间不足、空间不足时的精确大小、非法描述与非法参数、尺寸溢出、预置解码表与 `h2_encoding_init()` 一致、自定义字母表、手工修改描述不越界、分块编码、全部预置编码 0–70 字节往返和 C++ 链接。
