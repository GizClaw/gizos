# PIXA

PIXA 的格式、portable C runtime、通用工具和可分发动画资源由
[`GizClaw/pixa`](https://github.com/GizClaw/pixa) 统一维护。GizOS 通过固定
commit 的 `third_party/pixa` submodule 消费它，不再维护第二份格式实现或
`h2_pixa_*` 格式 API。

格式定义、兼容性和 C 平台接口以 submodule 中的文档为准：

- `third_party/pixa/docs/format.md`
- `third_party/pixa/docs/compatibility.md`
- `third_party/pixa/docs/c-osal.md`

## API Reference

[GizOS Bridge API Reference](/references/pixa)

PIXA core 的 production public headers 位于
`third_party/pixa/pkgs/c/include`。GizOS 自己的 public surface 只包含
`libs/pixa/include/h2_pixa_platform.h` 定义的 PAL bridge。

## Ownership

```text
third_party/pixa/
└── pkgs/c/                         # upstream format、reader、decode、blit、pack、extract
libs/pixa/
├── include/h2_pixa_platform.h      # PAL-backed OSAL/allocator bridge
└── src/h2_pixa_platform.c
libs/pal/providers/
├── esp-idf6.x/h2_pixa/             # ESP-IDF archive import
└── bk7258/h2_pixa/                 # BK7258 AP archive import
boards/**、<artifact-rule>/**       # mount、storage wiring 和 board policy
```

- `third_party/pixa` 是 PIXA format/core/tools/assets 的唯一 source of truth。
- `libs/pixa` 只把公开的 `h2_pal_fs_api_t` 和 `h2_pal_mem_api_t` 投影为
  upstream `pixa_osal_api_t` 和 `pixa_alloc_t`。
- `libs/pal/providers` adapter 只导入 Bazel 已生成的 archive 并传播 public include，不重新
  编译或复制实现。
- BSP 和 launcher 负责 filesystem mount、portable path、partition 与 backend
  wiring。bridge 不选择存储设备、不下载资源，也不拥有 Display。
- `libs/bundle` 和 App 是 consumer；它们不能形成另一个 PIXA format contract。

## 使用边界

Memory-only 的 reader、decode、blit 和 directory API 直接使用 upstream
`pixa_*` API，不需要初始化 platform bridge。

Pack 和 extract 需要 filesystem 与 allocator。调用方创建一个
`h2_pixa_platform_t`，从自己已经持有的 PAL API 初始化，并把 accessor 返回的
upstream 对象显式传入操作：

```c
h2_pixa_platform_t platform = {0};
const h2_pixa_platform_config_t config = {
    .fs = runtime->fs,
    .mem = runtime->mem,
};

if (h2_pixa_platform_init(&platform, &config) != H2_PAL_OK) {
    return H2_PAL_ERR_INVALID_ARG;
}

int pixa_result = pixa_extract_memory(
    data,
    data_len,
    output_path,
    h2_pixa_platform_osal(&platform),
    h2_pixa_platform_allocator(&platform),
    NULL);

h2_pixa_platform_deinit(&platform);
```

Bridge 和它借用的 PAL API 必须覆盖整个 upstream 调用。已初始化的
`h2_pixa_platform_t` 不能移动或复制，因为 OSAL/allocator 的 `user` 指回该对象。
Deinit 只使 accessor 失效，不关闭调用方拥有的文件，也不释放 PAL backend。

所有 OSAL 操作都通过 `h2_pal_fs_*` public wrapper。PAL 的 `OK`、
`INVALID_ARG`、`IO`、`NO_MEMORY`、`NO_SPACE` 和 `UNSUPPORTED` 映射到同名
PIXA OSAL 结果；其它错误统一映射为 `PIXA_OSAL_ERR_IO`。PAL sync 缺失时沿用
public wrapper 的 optional-sync 成功语义，其它缺失能力按 wrapper contract
返回 unsupported 或 invalid argument。

## 构建与测试

验证 upstream core、bridge 和直接 consumer：

```sh
cd third_party/pixa
make c-check

cd ../..
bazel test //libs/pixa:all
bazel test //libs/bundle:all
```

涉及 consumer 或 target wiring 的修改还必须按对应 Desktop、ESP-IDF 或 BK7258
guide 构建至少一个实际入口。

## 更新 submodule

PIXA pointer 只能在审查 upstream 变更后推进。选择 release 或 immutable commit，
不要在 GizOS 内 patch submodule：

```sh
git -C third_party/pixa fetch --tags
git -C third_party/pixa checkout <selected-release-or-commit>
git add third_party/pixa third_party/README.md
```

更新时同步记录 `third_party/README.md` 中的 version description 和完整 commit，
然后至少运行：

1. `cd third_party/pixa && make c-check`
2. `bazel test //libs/pixa:all`
3. `bazel test //libs/bundle:all`
4. 一个 Desktop PIXA consumer build
5. 一个 ESP firmware PIXA consumer build
6. 一个 BK7258 AP PIXA consumer build

如果 upstream format 或 public C API 改变，还要检查所有 `pixa_*` consumer 和
兼容性文档；不得通过新增本地 format wrapper 隐藏不兼容变化。
