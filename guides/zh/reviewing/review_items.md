# 审查项目

这份文档定义 Issue 审查、开发后自我审查和 PR Agent 审查共用的审查项目。每次审查都必须逐条检查；某项确实不适用时，需要说明审查对象和不适用原因，不能把整组内容概括为“已检查”。

## 使用方式

同一个项目在三类审查中的对象不同：

| 审查 | 如何使用审查项目 |
| --- | --- |
| Issue 审查 | 检查 issue 的 title、Issue Type、结构、relationship、目标、ownership、contract、失败行为、验收和验证 |
| 开发后自我审查 | 对本地 diff 或 branch 检查实际实现，发现问题后修复、验证并重新逐条检查 |
| PR Agent 审查 | 对 PR 最新 head 独立检查实际实现，把适用问题作为有证据的 finding |

审查深度由改动风险决定，但项目本身不能跳过。只修改文档时，硬件验证可以不适用；修改 board driver 时，不能因为 unit test 通过就跳过 firmware build 和硬件行为检查。

## 查找适用文档

审查开始时按以下顺序找到规则：

1. 阅读仓库根目录 `AGENTS.md`，确认文档入口和仓库级强制规则。
2. 阅读 [目录结构](/zh/developing/repo_layout)，根据目标路径或 changed directory 判断 ownership。
3. 根据文件语言读取 [命名](/zh/coding-styles/naming)、[C](/zh/coding-styles/c)、[Go](/zh/coding-styles/go) 或 [Markdown](/zh/coding-styles/markdown)。
4. 根据 ownership root 读取对应开发指引。
5. Contract 跨越多个 module 时，同时读取 provider、adapter 和 consumer 的文档与实现。

| Path 或 ownership | 开发指引 |
| --- | --- |
| `libs/pal/include` | [平台抽象层](/zh/developing/platform_abstract_layer) |
| `libs/runtime` | [Runtime](/zh/developing/runtime) |
| `libs/<library>` | [Library](/zh/developing/library) 和对应 library 页面 |
| 任意 `BUILD.bazel`、`.bzl` 或 `third_party/*.BUILD.bazel` overlay | [Bazel 代码规范](/zh/coding-styles/bazel) 和 [Bazel CI 依赖图](/zh/developing/bazel) |
| `libs/pal/providers/desktop` | [Components](/zh/developing/components) 和 [Desktop](/zh/developing/components/desktop) |
| `native_component_src/esp-idf6.x` | [Components](/zh/developing/components) 和 [ESP-IDF 6.x](/zh/developing/components/esp_idf6_x) |
| `native_component_src/bk7258` | [Components](/zh/developing/components) 和 [BK7258](/zh/developing/components/bk7258) |
| `boards/<board>` | [开发指引总览](/zh/guide)、对应 target 和 H2Loader 指引 |
| H2Loader common、app 或 launcher | [H2Loader](/apps/h2loader/) 及其子文档 |
| 根 `BUILD.bazel`、`REPO.bazel`、`tools/bazel` 或 Bazel CI | [Bazel 代码规范](/zh/coding-styles/bazel) 和 [Bazel CI 依赖图](/zh/developing/bazel) |
| `guides/` | [Markdown](/zh/coding-styles/markdown) |

一个审查对象涉及多个 ownership root 时，合并所有适用文档。文档、issue 和生产代码互相冲突时必须明确指出，不能自行选择更方便的一边作为结论。

## 目标和范围

- Issue title 符合 `prefix: Subject`，GitHub Issue Type 与 Tracking 或 Implementation 工作形态一致。
- Tracking issue 使用 `Task` 且有原生 sub-issue；Implementation issue 使用 `Bug` 或 `Feature`，没有把直接实现范围放进 `Task`。
- Issue body 使用对应类型的规定结构，relationship 文本格式正确，原生 parent/sub-issue 关系真实存在。
- Implementation issue 的 Code Changes Tree 为每个 file leaf 提供具体、可执行的 inline comment，没有裸 filename、占位描述或代替多个文件职责的 group-level 注释。
- 目标与用户要求、linked issue、PR description 和 acceptance criteria 一致。
- Non-goal 和明确排除的平台、目录、兼容工作或 cleanup 没有被带入范围。
- 审查对象完整：Issue 审查包含目标 issue 及其关系；实现审查包含完整 diff、关联 config、build wiring、tests、文档和 committed consumer。
- Parent、sub-issue、dependency 和相关 PR 的关系真实存在且方向正确。
- 没有用临时 workaround 替代已经确定的最终 contract。

## Ownership 和依赖

- 文件或计划路径位于正确 ownership root。
- PAL 保持平台无关 contract 和 `user + vtable` 形态。
- Portable library 不依赖 board、target SDK、launcher、partition 或 boot policy。
- Component、BSP、`boards/main`、app 和 H2Loader 的职责符合开发指引。
- App 保持跨平台，只消费 Runtime 和 portable library。
- Multi-chip、AP/CP 和 target-specific code 位于正确 execution unit 和层级。
- Deprecated 或 experimental tree 没有被误当作稳定实现入口。
- 新依赖是必要的，方向正确，没有形成反向依赖或隐藏 global coupling。
- `third_party/*.BUILD.bazel` 默认只暴露 upstream source/header/file group；只有明确直接使用纯 upstream contract 且不需要 GizOS source/config/dependency 时才保留 compiled target。
- External repository 的 `load` 和全部 target attributes 没有反向引用 `@gizos//...`、`@//libs`、`@//native_component_src`、`@//projects`、`@//boards` 或 `@//tools`；GizOS config 和 platform variant 由 first-party owner 定义。
- Bazel variant 只因 source、compile config、toolchain compatibility 或 dependency graph 的真实差异而拆分；公共 library 没有按 App 命名或复制。
- `_embed`、`_desktop`、`_mobile`、`_web` 后缀描述实际运行环境，默认 target 在无需拆分时保持无后缀；最终 artifact 只链接一个兼容 variant。

## Public Contract 和数据

- Public Header、实现、构建暴露内容、Doxygen 和 committed consumer 一致。
- 参数方向、ownership、lifecycle、blocking、timeout 和 error semantics 明确。
- API object、state object、opaque handle、config 和 callback 的角色没有混淆。
- Provider、adapter 和 consumer 使用同一类型、ID、单位和状态语义。
- `component_id`、`periph_id`、board identity、image identity 和 connection identity 没有混用。
- Wire format、persistent format 和跨平台数据明确 byte order、长度、版本和 validation。
- Event、state、mapping、protocol field 和 enum 只有一个 source of truth。
- Public API 没有暴露 private SDK handle、board wiring 或 borrowed buffer lifetime detail。

## 行为和边界条件

- 正常流程、invalid input、partial failure 和重复调用行为明确且正确。
- Allocation、partial initialization、cleanup 和 shutdown 不泄漏资源。
- Callback registration、task、queue、timer、transport 和 subscription 有对称关闭路径。
- Callback lifetime、buffer ownership、并发访问和 thread/ISR context 安全。
- Timeout、would-block、unsupported、disconnect、retry 和 backpressure 有明确语义。
- State machine 覆盖 restart、rollback、reconnect、cancel、close 和 recovery 中适用的路径。
- Capacity、length、overflow、empty/full、边界值和 malformed input 已处理。
- Platform-specific behavior 没有被错误描述为跨平台 guarantee。

## Test 和验证

- 新行为、失败路径和高价值边界条件有对应测试或验收步骤。
- Portable library 有自己的 build 和 host-runnable test。
- Go package 经过 formatter、test 和适用的 static check。
- Firmware 改动构建对应 target、board 和 image。
- Hardware 行为有适用的 flash、reset、serial、manual inspection 或 coredump evidence。
- Protocol 有适用的互通、round-trip、malformed input 和 recovery 验证。
- Generated output 可以从明确输入重复生成，结果稳定。
- 每个 CI execution class 的 Build/Test 都从当前 head 请求完整 compatible graph；host job 可以顺序执行并复用同一个 Bazel server 与 output base，mobile Build/Test 可以保持独立 task。Platform
  compatibility、统一的 `-manual` test filter 和 action cache 只影响实际 action，
  不得跳过 graph analysis、build 或自动 test。普通测试不声明 tag；外部服务、
  真实设备或人工环境 E2E test 只声明 `manual`。`ci-required` 对任一 execution class 的 failure、
  skipped 和 cancelled 状态正确收敛。
- 无法执行的验证、原因和 residual risk 已明确记录。
- 验证 evidence 对应正在审查的版本或 PR head，不使用旧结果替代。

## 文档和构建接线

- 开发文档描述合并后的最终结构和行为，不记录临时迁移状态。
- Issue 计划或实际代码修改 public contract、architecture、ownership、product flow、lifecycle、validation flow 或其他文档约束行为时，所有受影响的 source-of-truth guide 都已逐文件纳入 scope 并说明同步内容；没有文档影响时有 live evidence 支持的明确理由。
- API Reference 来自生产 Public Header，没有维护第二份 declaration。
- 新文件已加入正确 build source list、manifest、package 或 generator input。
- 删除和移动同步更新 include、link、navigation、AGENTS 索引和旧路径引用。
- Mermaid、anchor、sidebar 和 internal link 在 VitePress 中正常渲染。
- Build、test、flash 和使用命令可以复制执行，并在正确工作目录运行。

## Markdown 源码

- Issue 审查检查 issue specification，开发后自我审查检查 local diff 或 branch，PR Agent 审查检查最新 PR head；三类审查都对各自对象中的新增或修改手写 Markdown 应用同一源码换行规则。
- 手写 Markdown 的新增或修改普通 prose 段落如果只因 80 列或其它固定 print width 插入物理换行，必须作为 documentation-format finding；reviewer 报告前必须确认该换行不是 Markdown 结构、显式 hard break 或 generated 内容的一部分。
- 改动触及已有固定列宽换行的普通 prose 段落时，可以检查并整理完整的当前段落，但不能要求重排相邻未改段落、无关文件或范围外的已有违规内容。

## Git 内容

- Diff 或计划改动只包含本任务相关内容。
- 没有误加入 build output、cache、log、temporary file、secret、local config 或异常 binary。
- 没有无意义 permission、line ending 或批量格式变化。
- Symlink、submodule 和 Git LFS 变更是有意且目标正确的。
- Generated file、vendored source 和手写 source 的边界清楚。
- 删除的文件确实不再被构建、引用或作为 source of truth。

## Finding 标准

- Finding 必须指向具体 requirement、路径、symbol、line、调用路径或失败场景。
- Finding 必须说明影响和可执行的修复方向。
- Blocking finding 聚焦 correctness、contract、ownership、portability、lifecycle、security、testing 和 validation。
- 可选 polish、个人偏好和等价实现不作为 blocking finding。
- 没有执行的验证不能描述为通过；需要单独记录 residual risk。
