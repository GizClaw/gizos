# 代码风格约定

代码风格约定用于保持 GizOS 中的 public contract、portable library、target component、host tool 和文档一致。规则的目标是明确 ownership、生命周期、错误语义和平台边界，不要求为了视觉统一重写与本次改动无关的代码。

## 文档

- [命名](./naming.md)：仓库目录、文件、public symbol、类型、issue、PR 和 commit 的命名规则。
- [Bazel](./bazel.md)：BUILD target 边界、平台 variant、依赖方向和 graph review 规则。
- [C](./c.md)：C public header、PAL contract、portable implementation、target implementation 和测试规则。
- [Go](./go.md)：host tool、generator、protocol probe 和 local service 的 Go 规则。
- [Markdown](./markdown.md)：开发指引、审查指引、README、issue 文档和 API 文档的写作规则。

## 适用顺序

修改代码或文档时按以下顺序确定约束：

1. 先用目录确认 ownership，并阅读对应的开发指引。
2. 所有命名同时遵守命名规则。
3. 按文件语言应用 C、Go 或 Markdown 规则；修改 Bazel 文件时同时应用 Bazel 规则。
4. Public contract、协议格式和 target SDK 有更具体约束时，同时遵守对应 contract 或上游规范。

`third_party/` 中未经修改的上游代码保持上游风格。只在 GizOS adapter、patch 或仓库拥有的 wrapper 中应用本规范，不为了统一格式批量改写 vendored source。

## 基本原则

- 名称体现 owner，不使用无法说明归属的 `common`、`manager`、`helper` 或 `util`。
- Public API 明确参数方向、ownership、生命周期、阻塞行为和错误语义。
- Portable code 不包含 board、target SDK 或 launcher implementation detail。
- 注释解释 contract 和约束，不复述代码。
- 开发文档描述合并后的最终结构和行为；代码与文档之间的待解决差异记录在问题清单中。
- 修改范围保持聚焦，不顺手格式化无关文件。

## 验证

每次改动至少运行对应 formatter、build 或 test，并检查 diff：

```sh
git diff --check
```

C/C++ library 使用其 Bazel package 提供的 build 和 test；Go package 运行 `gofmt` 并通过对应 Bazel test；VitePress 文档从仓库根目录运行 `make guides-build`。
