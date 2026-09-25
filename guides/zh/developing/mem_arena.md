# Memory Arena

`libs/mem_arena` 把调用方的一块内存作为有边界的分配域，通过标准 Memory PAL 借给 consumer。Core 只依赖 PAL 和已有的 portable TLSF，不选择平台内存、不创建锁，也不替换默认 allocator。

TLSF 的 build adapter 强制包含 `h2_tlsf.h`，把内部依赖的公共符号统一映射为 `h2_tlsf_*`，避免与 SDK 自带 TLSF 冲突。Windows MSVC 使用 `/FI`，其余工具链使用 `-include`；arena host test 同时验证该符号映射能够完成链接。

## API Reference

[API Reference](/references/mem_arena) 由生产 Public Header `libs/mem_arena/include/h2_mem_arena.h`、可选 census 的 `h2_mem_arena_census.h` 和诊断的 `h2_mem_arena_diagnostics.h` 生成，参数、统计字段和返回值以头文件为准。

## 分配与生命周期

调用方在创建前独占 backing block，提供成对 lock/unlock；arena instance 与 TLSF metadata 都存放在块内。按请求大小优先选择 small 或 large 独立池；small pool 大小为零时只使用 large。首选池无法满足请求时先尝试另一池，两池都无法满足才尝试可选 fallback；没有 fallback 时返回 NULL。分池优先隔离大小对象，但不保留硬性容量配额，空闲空间可以被另一请求类别借用。

每块记录分配基址、请求大小与 pool/fallback owner；free 和 realloc 必须经过同一个 arena Memory PAL。Realloc 先尝试首选池，再尝试另一池；尝试到旧块实际所属池时可原地扩展，也可跨池、fallback 迁移，保留 payload 对齐和原有数据；失败保持旧块有效。Fallback 可以只提供 alloc/free，不要求 realloc。

所有可变操作与查询在调用方锁内完成，fallback 也在该锁内执行，因此回调不得重入 arena。RTOS 锁必须支持优先级继承；单线程环境可提供 no-op callbacks。日志在查询返回、锁释放后由调用方输出。

销毁前先停止并 join 所有 borrower，再释放全部 pool/fallback allocation。存在 live allocation 时 destroy 拒绝且保留实例；成功后句柄/API 失效，调用方才可释放 backing block、fallback context 与锁。

## 统计和诊断

轻量 stats 区分各池的 reserved、payload live/peak、最大请求和累计 fallback 尝试；fallback live 单独记录，不与池内 payload 混算。Live/peak 归属实际提供块的池；`borrowed_count` 累计本池替另一请求类别成功服务的 alloc/realloc 次数，原地 realloc 也计数，free 不递减，计数饱和于 `UINT64_MAX`。最大请求涵盖本请求类别及本池成功接收的借用；只有两池都未满足请求时才按原请求类别累计 fallback_count/bytes，失败不改变旧块的 live 记账。诊断 inspection 只在显式请求时遍历 TLSF，提供 raw free total、largest free block 与 consumed。Live block 查询要求调用方排除其并发 free/realloc；fallback consumed 是请求字节数加 arena overhead 的下界，无法包含底层系统 allocator 的隐藏开销。

### 可选分配 census

`//libs/mem_arena:census` 是独立可选 target；不链接它的 firmware 不增加 core 的分配路径工作。调用方提供 arena、同一 arena 的 Memory PAL（可经过 board wrapper）、arena 外 metadata allocator、支持优先级继承的 Sync PAL、唯一标签表，以及固定的块/调用点容量和探测上界。`tag_mem(census, tag_index)` 借出每个标签的 Memory PAL；同一 census 的任意 view 都可 free/realloc。创建时一次性申请元数据和两个 mutex，分配路径不再申请诊断元数据、不格式化或输出日志。表满仍转发业务分配；无法归属的块计入 `unattributed_overflow` 和 site 0，此部分的 per-tag 统计不再精确；内存不足沿用底层 arena 结果。销毁前停止并 join 全部 borrower，仍有 live block 时拒绝销毁。

`snapshot` 在短分配锁内复制标签、调用点和 overflow/失败计数，在锁外把借用的结构化快照交给调用方；另一个 mutex 串行化快照访问，visitor 不得递归 snapshot。块表溢出时业务分配仍完成；无法记录 owner 的块计入独立 `unattributed_overflow` 和 site 0，任何 tag view 都可安全 free/realloc，aggregate live 保持准确，但该部分不再宣称 per-tag 精确。GizOS 不规定标签名、采样周期、top-N 排序、平台日志格式或 `NO_MEMORY` 策略，这些由产品 owner 持有。调用点地址是 best-effort：Xtensa windowed ABI 可附加 outer 返回地址，其他工具链可只有直接 caller；内存占用和标签计数不依赖符号化。

### 可选内容诊断

`//libs/mem_arena:diagnostics` 是全平台可编译的可选 C11 target；公共核心不选择 OS、芯片或编译器。调用方提供保留块大小、small pool 阈值/容量、标签名与 stack 标签索引、trace 开关，以及固定 block/site/peak/duplicate/queue/top 容量。公共实现创建 `mem_arena`，通过 census 的标签 Memory PAL 转发分配，并使用注入的 Memory、Sync、Task、Queue、Time、FS、Log PAL 输出调用点、同时峰值、allocator overhead、未触及尾部估计与内容 hash 重复候选。Task/Queue 不可用时同步生成报告；内容安全读取或回溯探针不可用时只丢该项证据，业务分配继续工作。标签名、phase、日志策略和报告目录由产品持有。

所有业务分配无论诊断表是否满都继续走 arena/fallback，free/realloc 可从任意 tag view 安全转发。公共诊断的 block/site/peak/duplicate 表分别受配置容量限制；report queue 满时在 case/report 边界等待 worker，而不是无限积压快照。`OVERFLOW` 记录诊断丢失及 census unattributed live；溢出后的 per-tag 内容/峰值只能视为下界，census aggregate 仍准确。Trace 开启时，非 stack block 以 `0xd3` 初始化，并在释放/采样时估算未触及尾部；这会改变诊断模式的初始化内容与运行开销，不能把尾部模式或 hash 相等当作泄漏/重复对象证明。Darwin/Linux provider 的 kernel copy 对并发内容修改只给 best-effort 片段，分配锁只防止同一块同时被 free/realloc。报告 FS 仅允许调用方授权的目录挂载，不得向 App 暴露 host root。所有 borrower 停止并 join 后再销毁实例；arena 或 census 尚有 live allocation 时明确报错并拒绝静默清理。

## 平台集成

[ESP adapter](./components/esp_idf6_x.md#psram-arena) 提供 PSRAM reservation、默认关闭的可选 system spill 与内部 mutex；使用它的最终 firmware 必须把 `//libs/mem_arena` 加入自身唯一 `firmware_lib_component` 的 archive dependencies。Provider 的可选 PSRAM stack allocator 属于 task policy 配置，PAL task options 保持不变。

[Desktop](./components/desktop.md#task-栈记账) 可用同一 Memory PAL 记录 placeholder 栈。它们只是 allocator 占位，不是宿主 OS thread 实际使用的 stack。[LVGL](./lvgl.md#arena-allocator) 通过现有 platform binding 借用 arena，完整退出后才能销毁。

## 验证

```sh
bazel test //libs/mem_arena/... //libs/lvgl:arena_test --test_output=errors
```

Host tests 验证首选分池、双向借池、借池后的 free/realloc 记账、alignment、owner 路由、realloc 数据保留和失败原子性、逐池统计与按需诊断。ESP SDK fake tests 和 desktop provider tests 位于各自 owning package；实际 PSRAM/XIP 行为与调度延迟需设备验证。
