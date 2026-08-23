# 命名

命名必须体现代码的 owner、抽象层和用途。同一概念在目录、文件、类型、函数和文档中使用同一个稳定名称，不把迁移历史或临时实现写进最终名称。

## 通用规则

- 使用完整、可识别的领域名称；只有仓库已稳定使用的缩写才可以保留，例如 `PAL`、`BSP`、`NFC`、`IMU`、`KCP` 和 `HTTP`。
- 名称必须能回答“属于哪个 module、target、board、package 或 protocol”。
- 避免单独使用 `common`、`support`、`helper`、`manager`、`misc` 或 `util`；如果确实是公共实现，由父目录或 module prefix 明确 owner。
- 不复用 deprecated system 的名称表示新系统，例如 H2Loader 代码不使用 `apploader` 命名。
- 不在名称中加入 `new`、`old`、`v2`、`tmp` 或迁移 issue 编号；协议或上游版本本身是 contract 时除外。

## 目录和文件

- 仓库拥有的目录和文件使用小写名称。
- C package、C 文件和 Markdown 文件使用 `_` 分隔单词，例如 `h2_runtime_event.c`、`platform_abstract_layer.md`。
- 只有工具链、上游项目或既有稳定名称要求时才使用 `-`，例如 `third_party` 中的原始 package 名。
- Public C header 使用 `h2_<module>.h` 或 `h2_<module>_<area>.h`。
- Private header 放在 implementation-owned 目录，并使用 `_internal.h`、`_private.h` 或明确的 implementation 名称；不能通过 public include path 暴露。
- 测试目录统一使用 `tests/`。测试文件使用 `test_<subject>.c` 或 Go 的 `<subject>_test.go`。

目录的 ownership 形状遵守仓库布局：

```text
libs/<library>/
native_component_src/<target-or-sdk>/
boards/<board>/<chip-or-target>/
projects/[<group>/]<app>/
```

## C Symbol

Public C symbol 使用 owner prefix：

```text
h2_<module>_*
h2_pal_<capability>_*
h2_runtime_*
h2_loader_*
```

- Function、field、local variable、struct tag、enum tag 和 typedef 使用 lower snake case。
- Public typedef 以 `_t` 结尾。
- Vtable 和 API object 分别使用 `<owner>_vtable_t` 与 `<owner>_api_t`。
- Config、event、state、stats 和 result type 分别使用 `_config_t`、`_event_t`、`_state_t`、`_stats_t` 和 `_result_t`。
- Opaque handle 使用 `typedef struct <owner> <owner>_t;`，不要暴露 private field。
- Enum value 和 macro 使用 uppercase，并带完整 owner prefix。
- Output 参数以 `out_` 开头；callback context 使用 `user`，除非 contract 已稳定使用更具体的名称。
- File-local `static` symbol 也必须可读，不能因为作用域小就使用模糊名称。

示例：

```c
typedef struct h2_pal_example_vtable {
    h2_pal_result_t (*read)(void *user, h2_pal_example_value_t *out_value);
} h2_pal_example_vtable_t;

typedef struct h2_pal_example_api {
    void *user;
    const h2_pal_example_vtable_t *vtable;
} h2_pal_example_api_t;
```

## Go Identifier

- Package name 使用简短的小写单词，并与目录 owner 一致。
- Exported identifier 使用 Go MixedCaps，只导出其他 package 需要使用的 contract。
- Initialism 遵守 Go 通常写法，例如 `ID`、`HTTP`、`URL`、`JSON`。
- 单方法 interface 通常使用 `-er` 名称，例如 `Reader`、`Writer`。
- 不使用 Java 风格 getter；优先使用 `Owner()`，而不是 `GetOwner()`。
- Error string 以小写开头，除非开头是 proper noun、command 或 protocol token。

## ID 和配置

- App-facing identifier 使用 `component_id`；board physical peripheral identifier 使用 `periph_id`，两者不能混用。
- Board、target、chip、image 和 app 是不同概念，config field 必须使用准确名称。
- Boolean field 使用能表达条件的名称，避免含义不明的 `enable`、`flag` 或 `status`。
- Timeout 名称带时间单位，例如 `timeout_ms`；buffer 和 payload 长度使用 `_size` 或 `_len`，并在 contract 中明确单位。

## Issue、PR 和 Commit

Commit 和 PR title 使用：

```text
{module}: {subject}
```

- Module 使用稳定的小写 owner，可以包含 `/` 层级，例如 `runtime`、`pal/ble`、`guides`。
- Subject 使用简洁的动词短语，不加句号。
- Title 必须描述实际变化，不能使用 `update code`、`fix issue` 或 `cleanup` 这类泛化描述。
- Issue title 描述问题或期望结果；具体 implementation task 放在 issue body 或 sub-issue 中。
