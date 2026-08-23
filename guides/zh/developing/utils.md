# Utils

`libs/utils` 保存体量较小、平台无关且不属于其他明确 library 的通用能力。新增内容前应先确认它不能归属到更具体的 owner，避免把 `utils` 变成无边界的 helper 集合。

## API Reference

[API Reference](/references/utils)

`libs/utils/include` 中参与项目构建的头文件是 Utils 的生产 Public API contract。Utils 提供根据 IMSI 查找 APN 的能力；它不访问 modem、SIM、filesystem 或网络，IMSI 的获取和 APN 应用由调用方负责。

## 依赖和边界

Utils 不能放 board helper、SDK wrapper、业务 workflow、全局状态或可以归属于其他 library 的功能。只有稳定、可独立测试且跨平台的最小工具能力适合放在这里。

## 构建与测试

```sh
bazel test //libs/utils:all
```
