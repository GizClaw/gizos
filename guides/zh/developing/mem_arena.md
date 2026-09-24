# Memory Arena

`libs/mem_arena` 把调用方的一块内存作为有边界的分配域，通过标准 Memory PAL 借给 consumer。Core 只依赖 PAL 和已有的 portable TLSF，不选择平台内存、不创建锁，也不替换默认 allocator。

TLSF 的 build adapter 强制包含 `h2_tlsf.h`，把内部依赖的公共符号统一映射为 `h2_tlsf_*`，避免与 SDK 自带 TLSF 冲突。Windows MSVC 使用 `/FI`，其余工具链使用 `-include`；arena host test 同时验证该符号映射能够完成链接。

## API Reference

[API Reference](/references/mem_arena) 由生产 Public Header `libs/mem_arena/include/h2_mem_arena.h` 生成，参数、统计字段和返回值以该头文件为准。

## 分配与生命周期

调用方在创建前独占 backing block，提供成对 lock/unlock；arena instance 与 TLSF metadata 都存放在块内。按请求大小优先选择 small 或 large 独立池；small pool 大小为零时只使用 large。首选池无法满足请求时先尝试另一池，两池都无法满足才尝试可选 fallback；没有 fallback 时返回 NULL。分池优先隔离大小对象，但不保留硬性容量配额，空闲空间可以被另一请求类别借用。

每块记录分配基址、请求大小与 pool/fallback owner；free 和 realloc 必须经过同一个 arena Memory PAL。Realloc 先尝试首选池，再尝试另一池；尝试到旧块实际所属池时可原地扩展，也可跨池、fallback 迁移，保留 payload 对齐和原有数据；失败保持旧块有效。Fallback 可以只提供 alloc/free，不要求 realloc。

所有可变操作与查询在调用方锁内完成，fallback 也在该锁内执行，因此回调不得重入 arena。RTOS 锁必须支持优先级继承；单线程环境可提供 no-op callbacks。日志在查询返回、锁释放后由调用方输出。

销毁前先停止并 join 所有 borrower，再释放全部 pool/fallback allocation。存在 live allocation 时 destroy 拒绝且保留实例；成功后句柄/API 失效，调用方才可释放 backing block、fallback context 与锁。

## 统计和诊断

轻量 stats 区分各池的 reserved、payload live/peak、最大请求和累计 fallback 尝试；fallback live 单独记录，不与池内 payload 混算。Live/peak 归属实际提供块的池；`borrowed_count` 累计本池替另一请求类别成功服务的 alloc/realloc 次数，原地 realloc 也计数，free 不递减，计数饱和于 `UINT64_MAX`。最大请求涵盖本请求类别及本池成功接收的借用；只有两池都未满足请求时才按原请求类别累计 fallback_count/bytes，失败不改变旧块的 live 记账。诊断 inspection 只在显式请求时遍历 TLSF，提供 raw free total、largest free block 与 consumed。Live block 查询要求调用方排除其并发 free/realloc；fallback consumed 是请求字节数加 arena overhead 的下界，无法包含底层系统 allocator 的隐藏开销。

## 平台集成

[ESP adapter](./components/esp_idf6_x.md#psram-arena) 提供 PSRAM reservation、默认关闭的可选 system spill 与内部 mutex；使用它的最终 firmware 必须把 `//libs/mem_arena` 加入自身唯一 `firmware_lib_component` 的 archive dependencies。Provider 的可选 PSRAM stack allocator 属于 task policy 配置，PAL task options 保持不变。

[Desktop](./components/desktop.md#task-栈记账) 可用同一 Memory PAL 记录 placeholder 栈。它们只是 allocator 占位，不是宿主 OS thread 实际使用的 stack。[LVGL](./lvgl.md#arena-allocator) 通过现有 platform binding 借用 arena，完整退出后才能销毁。

## 验证

```sh
bazel test //libs/mem_arena/... //libs/lvgl:arena_test --test_output=errors
```

Host tests 验证首选分池、双向借池、借池后的 free/realloc 记账、alignment、owner 路由、realloc 数据保留和失败原子性、逐池统计与按需诊断。ESP SDK fake tests 和 desktop provider tests 位于各自 owning package；实际 PSRAM/XIP 行为与调度延迟需设备验证。
