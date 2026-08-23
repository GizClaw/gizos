# 开发后自我审查

开发后自我审查发生在实现完成后、提交或更新 Pull Request 前。它不是一次性 checklist，而是开发者必须执行的 `review -> fix -> verify -> re-review` 循环。

## 输入

开始前确认：

- Review scope：working tree diff、staged diff、指定目录或当前 branch。
- Requirements：用户要求、issue、设计结论、开发指引和代码风格。
- Validation：与改动风险对应的 build、test、flash、serial、manual inspection 或 coredump evidence。

Scope 没有明确指定时，先检查 staged changes；没有 staged changes 时检查当前 working tree。不要把用户尚未准备提交的无关改动纳入修复范围。

## 审查项目

每一轮 self review 都先阅读[审查项目](./review_items.md)，找到本次改动适用的文档，再对实际 diff 逐条检查全部项目。不适用的项目需要说明原因。

## Review Loop

重复以下流程：

1. 从最新 diff 开始 fresh review，不沿用上一轮“应该已经修好”的结论。
2. 只记录会影响 correctness、contract、ownership、portability、maintenance 或 validation 的 negative finding。
3. 修复所有可以在既定需求内安全修复的问题。
4. 根据风险补充或更新测试、文档和验证。
5. 执行 focused validation。
6. 把 validation failure 当作新的 finding。
7. 对修复后的完整 diff 重新开始下一轮 review。

修复会改变产品行为、contract 或已确定设计时停止并请求决策，不能在自我审查中擅自扩大需求。

## Round 记录

每轮可以使用：

```text
Round N

Findings:
- [severity] path/symbol: problem and impact

Fixes:
- change made

Validation:
- command: result

Decision:
- continue / complete / blocked
```

Severity 表示影响，不代替证据。可选 polish 不进入 blocking finding。

## 完成条件

结束前必须逐条确认以下条件：

- 最新一轮 fresh review 没有新的 negative finding。
- Relevant validation 已通过，或无法执行的验证及风险已经明确记录。
- 没有仍应阻塞提交的 ownership、contract、logic、resource、Git content 或 testing 问题。

完成后在 PR description 或 handoff 中记录改动范围、关键设计结论、验证命令和未执行的硬件验证。
