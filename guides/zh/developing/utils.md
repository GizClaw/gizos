# Utils

`libs/utils` 保存体量较小、平台无关且不属于其他明确 library 的通用能力。新增内容前应先确认它不能归属到更具体的 owner，避免把 `utils` 变成无边界的 helper 集合。

## API Reference

[API Reference](/references/utils)

`libs/utils/include` 中参与项目构建的头文件是 Utils 的生产 Public API contract。Utils 提供根据 IMSI 查找 APN 的能力，以及共享的 binary32 除法辅助；它不访问 modem、SIM、filesystem 或网络，IMSI 的获取和 APN 应用由调用方负责。

## Binary32 除法辅助

`h2_f32_math.h` 中的 `h2_f32_div` 是单份 `static inline` 实现，不是仅用于链接的声明。Lua 笔画光栅与其他 portable native 数值计算可使用同一个头，不在应用或 Lua 私有目录复制算法。它保留 reciprocal seed、四次显式 FMA 修正和商残差修正，在指定异常范围回退到 C 除法；完整参数和浮点语义来自公共头的 Doxygen。

调用方必须使用 binary32 存储和求值、round-to-nearest，关闭 fast-math、重结合与 flush-to-zero。头文件拒绝编译器能显式报告的不兼容模式；这不能检测运行时浮点环境的全部变化。算法不承诺全域正确舍入，精化路径可能把负零结果变成正零，不保证 NaN payload/sign 或异常标志。数值测试比较旧算法和固定随机样本；样本最大 ULP 只描述该编译配置下的测量，不构成所有输入的误差上界。不要用它替换未审核的双精度物理计算。

## 依赖和边界

Utils 不能放 board helper、SDK wrapper、业务 workflow、全局状态或可以归属于其他 library 的功能。只有稳定、可独立测试且跨平台的最小工具能力适合放在这里。

## 构建与测试

```sh
bazel test //libs/utils:all
```
