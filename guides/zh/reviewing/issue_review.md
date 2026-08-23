# Issue 审查

Issue 审查发生在实现开始前，用于确认 GitHub issue 的标题、原生类型、关系、目标、边界、ownership、设计约束、验收和验证已经达到对应 issue 类型所需的清晰度。它不是只检查文字格式，也不在设计尚未确定时强行补出实现细节。

本文同时定义本仓库的 issue 编写格式。创建或更新 issue 时遵守本文；执行 review 时再按[审查项目](./review_items.md)逐条验证。

## 操作边界

- 用户要求创建、编写、更新或整理 issue 时，直接操作 GitHub 上的真实 issue；只有用户明确要求草稿或讨论、禁止运行 `gh`，或者 repository/issue identity 不可用时，才只输出草稿。
- 用户只要求 review、audit、检查或判断 readiness 时保持 read-only，不修改 title、body、Issue Type、relationship、label 或其他 metadata。
- 修改已有 issue 前先读取 live title、body、Issue Type、parent、sub-issue、dependency 和相关 PR。
- 修改完成后重新读取 live body、relationship 和 metadata，确认 GitHub 上的最终状态；临时文件和本地预期内容不能代替回读结果。
- 不根据旧 issue、旧文档或历史验证结果推断当前 repository 和 GitHub 状态。

## 标题

Issue title 使用以下格式：

```text
prefix: Subject
```

- `prefix` 中的英文字母使用小写，不含空格，可以用 `/` 表达稳定的 category、workflow、module 或 module path。
- `/` 分隔的每个 token 都必须非空，其中的英文字母保持小写。
- 冒号后有且只有一个空格。
- `Subject` 非空，描述 issue 完成后的目标，不写临时迁移状态。

示例：

```text
tracking: Split OTA recovery work
bug: Fix token refresh race
research: Evaluate LVGL XML project path
firmware/ota: Add recovery status page
```

以下格式不合法：

```text
Tracking: Split OTA recovery work
admin ui: Add workspace template editor
admin/ui:Add workspace template editor
admin//ui: Add workspace template editor
/admin/ui: Add workspace template editor
admin/ui/: Add workspace template editor
```

Title prefix 与 GitHub Issue Type 是两个独立字段。Prefix 表达主题或领域，Issue Type 表达工作形态。

## Issue Type

本仓库使用 GitHub 原生 `Task`、`Bug` 和 `Feature`：

### Tracking Issue

Tracking issue 的 GitHub Issue Type 必须是 `Task`。它表达产品或项目边界，并组织原生 sub-issue。

- `Task` 必须有原生 sub-issue。
- `Task` 只保存总体目标、Non-goal、子问题关系、dependency、状态和完成条件。
- `Task` 不直接拥有 Code Changes Tree、implementation design 或具体实现验收；这些内容由 sub-issue 负责。
- Child 的完成必须共同构成 parent 的完成条件。
- `tracking` prefix 只有在正文是纯容器 outline 时才对应 `Task`。

### Implementation Issue

Implementation issue 对应一组可以独立完成和验证的改动。GitHub Issue Type 按工作内容设置：

- 缺陷或非预期行为使用 `Bug`。
- 新的用户可见能力、architecture capability、research、refactor、infra、docs 或 cleanup，在不是纯容器时使用 `Feature`。
- 有 Code Changes Tree、直接 implementation design 或直接验收条件的 issue 不能使用 `Task`，即使它有 sub-issue。

实现者必须能从 issue 确定代码放在哪里、contract 如何变化、哪些行为不在范围内，以及如何判断完成。

## Background 关系

Issue 存在关系时，正文 `## Background` 的开头使用：

```markdown
## Background

- Parent: #103
- Prerequisite of:
  - #107
  - #134
- Follow up to:
  - #157

Short context paragraph here.
```

- `Parent` 使用单个 inline list item。
- `Prerequisite of` 和 `Follow up to` 始终使用 nested list，即使只有一个 issue。
- 关系只写 `#123`，不重复 issue title。
- 不使用 `Related`；松散引用写入普通背景说明。
- 正文关系用于解释，不能代替 GitHub 原生 parent/sub-issue relationship。
- Reviewer 必须验证原生 relationship 真实存在且方向正确。

## Tracking Issue 结构

Tracking issue 使用以下顶层结构：

1. `## Background`
2. `## Goal`
3. `## Sub-issues`
4. `## Completion Criteria`

`Goal` 同时说明产品或架构边界和 Non-goal。`Sub-issues` 只保存原生 child 的 outline、dependency、顺序、可并行关系和状态，不复制 child 中容易漂移的路径、API 或实现细节。`Completion Criteria` 说明所有 child 如何共同构成 parent 完成条件。

## Implementation Issue 结构

Implementation issue 必须按以下顺序使用五个顶层 section：

1. `## Background`
2. `## Goal`
3. `## Code Changes Tree`
4. `## Design`
5. `## Test And Acceptance Criteria`

正文描述合并后的最终状态，不记录无关的迁移历史。不得为了套格式删除已有的具体 API、流程、dependency、test 或 acceptance 信息。

### Background

说明为什么需要修改，并按本文格式列出相关 parent、prerequisite 和 follow-up。松散引用和历史上下文放在关系列表后的短段落中。

### Goal

说明本 issue 直接拥有的最终结果、用户或系统可观察行为和 scope boundary。使用 `### Non-goals` 明确不包含的能力、平台、目录、兼容工作和 cleanup。

### Code Changes Tree

根据 live repository、根目录和受影响路径的 `AGENTS.md`、[目录结构](/zh/developing/repo_layout)及对应开发文档，列出本 issue 直接拥有的计划文件范围。每个文件 leaf 后面都必须使用 inline `#` comment，具体说明这个文件直接承担的改动：

```text
src/
└── feature/
    ├── include/
    │   └── feature.h              # 新增 public API 和 ownership contract
    └── src/
        └── feature.c              # 实现参数校验和 bounded execution flow
tests/
└── cases/
    └── feature/
        └── test_feature.c         # 覆盖成功、invalid input 和 partial failure
guides/
└── feature.md                     # 同步最终 API、lifecycle 和 failure behavior
```

- Source、header、config、test、script、generated、vendored 和 documentation file 都必须逐文件注释；裸 filename 不完整，不能依赖 tree 外的 group-level 说明。
- 注释使用具体、可执行的文件级描述，不能使用 `update code`、`add tests`、`related changes` 或同类占位语句，也不能用一条注释代替多个文件的职责。
- 只列 owned source、config、tests、docs、scripts、vendored dependency 和 committed generated file。
- 不列 build output、cache、downloaded dependency、temporary file、log 和无关路径。
- 按 ownership boundary 分组，并明确禁止进入的层。
- 删除项标记 `(delete)`；需要提交的生成文件标记 `(generated, committed)`。
- 代码修改 public contract、architecture、ownership、product flow、lifecycle、validation flow 或其他被仓库文档约束的行为时，必须把所有受影响的 source-of-truth guide 作为带注释的 file leaf 列入 tree；只列 `guides/` 或 `docs/` 路径而不说明同步内容不完整。
- Live repository evidence 确认没有文档影响时，在 tree 后立即说明不修改文档的具体理由，不能静默省略 documentation scope。
- 路径无法从 repository 和文档确定时，在 `Design` 中增加 `### Open Design Questions`，不能编造目录。

### Design

按任务需要使用具体 subsection，覆盖适用的 contract：

- Public API、command、route、config、schema、file format 和 board/platform hook。
- Entrypoint、data flow、state transition、buffer ownership、async、cleanup 和 lifecycle。
- Provider、adapter、consumer 和 dependency 的 ownership。
- Desktop、ESP、BK、browser、service 或 SDK 的平台差异。
- Invalid input、partial failure、timeout、retry、unsupported、disconnect 和 recovery。
- 明确 out-of-scope boundary。

未决问题使用 `### Open Design Questions`。会改变 public behavior、目录归属、平台范围或验收方式的问题未解决时，issue 不能标记为 ready。

### Test And Acceptance Criteria

使用两个 subsection：

- `### Acceptance Criteria`：列出逐项可观察、可判定的 close condition。
- `### Validation`：列出具体 build、test、flash、serial、manual inspection、protocol interoperability 或 coredump evidence。

Validation 必须覆盖适用 target、失败路径和高价值边界条件。无法执行的硬件、网络或 SDK 验证需要记录 `SKIP` 或 `UNSUPPORTED` 原因和 residual risk，不能描述为通过。

## 审查流程

Issue reviewer 必须：

1. 读取 live issue 的 title、body、Issue Type、parent、sub-issue、dependency、comments 和相关 PR。
2. 阅读根目录及计划路径适用的 `AGENTS.md`。
3. 阅读[审查项目](./review_items.md)、[目录结构](/zh/developing/repo_layout)、对应 ownership 文档和适用 coding style。
4. 先判断 Tracking Issue 或 Implementation Issue，再按对应结构和粒度审查。
5. 对 `review_items.md` 每项给出结论；不适用项说明审查对象和原因。
6. 对照 live repository 验证 issue 中的 path、public API、build wiring、test 和 command。
7. 只有用户明确要求更新时才修改安全且有 repository evidence 支持的内容。

可以直接更新的内容仅限机械整理或有明确证据的修正，例如调整 section 顺序、保留语义地移动内容、补充已确认的路径和验证命令。不得发明产品行为、API contract、storage format、hardware behavior、migration plan 或 compatibility guarantee。

## 审查输出

Issue review 输出：

```text
Checklist results
- review item: pass / not applicable because ...

Blocking gaps
- ambiguity or conflict that prevents safe implementation

Suggested edits
- concrete section, wording, path, flow or validation to add

Ready status
- ready / not ready
```

`Checklist results` 按 `review_items.md` 的顺序记录每一项结论。适用且没有 finding 的项目写 `pass`；不适用的项目写明审查对象和原因；存在问题的项目在这里引用对应 `Blocking gaps`，不重复长篇 finding。

如果用户明确要求更新，在 `Suggested edits` 中区分已应用修改和仍需决定的建议，并报告修改后的 issue URL 和 live 回读结果。

## 写入输出

创建、编写、更新或整理 issue 后输出：

```text
Issue
- #123 URL

Applied changes
- title, type, relationship or body change

Open decisions
- unresolved decision or none

Live verification
- title, Issue Type, native relationship and body reread result

Ready status
- ready / not ready
```

不得只报告本地临时文件或预期修改。创建后仍有 Open Design Questions 时，明确标记 `not ready`。

## Ready 条件

Tracking issue 在 title、Issue Type、边界、Non-goal、child coverage、原生 relationship、dependency 和完成条件清楚时 ready。

Implementation issue 在 title、Issue Type、五段结构、每个计划文件的具体改动、需要同步的 source-of-truth guide、核心 contract、ownership、Non-goal、acceptance 和 validation 都不需要实现者猜测时 ready。没有文档影响时必须有 live evidence 支持的明确理由；可选实现细节尚未决定不一定阻塞。
