# Partial Update

`partial-update` 是读取 Runtime filesystem 中 package data generation 的 Example，不依赖 H2Loader package 或 image API。当前 H2Loader-managed target 把 App generation 编译进 image，并读取 `/data/version.txt` 的 data generation，启动后输出：

```text
H2_PARTIAL_UPDATE_SMOKE result=PASS app=v2 data=v1
```

外部 H2Loader 验收流程可以用该 observation 验证完整 format 1 package 的选择性写入；该判定不属于 Example App。
