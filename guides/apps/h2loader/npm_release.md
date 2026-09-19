# npm Release

`@gizclaw/h2loader` 的 npm tarball 与固件属于同一个 GizOS snapshot Release。`.github/workflows/release.yml` 在 `v*` tag push 时发布该 Release；手动 `workflow_dispatch` 只构建 Actions artifact，发布步骤仍只允许 `github.event_name == 'push'`。

## Slice 与本地构建

`catalog` 解析 Release batch version 后，`npm-packages` 与 ESP/BK7258 构建并行。npm job 只需要 checkout、Bazel 和已有 remote-cache auth，不依赖 ESP-IDF 或 firmware-devenv。`release-npm-bundle` artifact 直接进入最终 `release-bundle`；`firmware-bundle` 显式下载 catalog 和三个固件 producer artifact，不能消费 npm slice。两个 assembly job 都保留 artifact 子目录，避免下载时覆盖重名文件，使后续 duplicate-basename 校验能够拒绝冲突。

在仓库根目录执行：

```sh
make bazel-release RELEASE_SLICE=npm-packages RELEASE_VERSION=0.1.0 RELEASE_STAGING_DIR=build/release/npm-packages
bazel test //tools/bazel:npm_release_test //tools/bazel:release_test //tools/bazel:release_bundle_test //projects/h2loader/targets/npm_package/h2loader:release_tarball_test
```

`npm-packages` 是不接受 `RELEASE_INPUT_DIR` 的 source producer。`//tools/bazel:npm_release_bundle` 构建当前声明的 package 列表，staging 目录只包含每个 package 的 `.tgz` 和一个 `npm-index.json`，不生成 `SHA256SUMS`。当前 H2Loader package version 为 `0.2.2`，对应文件是 `gizclaw-h2loader-0.2.2.tgz` 与 `npm-index.json`。

合并已下载的两个 bundle 时，保留各自子目录即可；最终输入扫描拒绝 symlink 和重复 basename：

```sh
mkdir -p build/release/input
cp -R build/release/firmware-bundle build/release/input/firmware
cp -R build/release/npm-packages build/release/input/npm
make bazel-release RELEASE_SLICE=release-bundle RELEASE_VERSION=0.1.0 RELEASE_INPUT_DIR=build/release/input RELEASE_STAGING_DIR=build/release/final
```

`release-bundle` 要求 `firmware-index.json`、固件 bundle 的 `SHA256SUMS` 和 `npm-index.json` 同时存在。它保持原有固件 identity、checksum coverage 和 exact-set 校验，并校验 npm identity、索引中每个 tarball 的 SHA-256 与字节数；多余或缺少任一 asset 都失败。最终重算的顶层 `SHA256SUMS` 覆盖全部固件、npm tarball、`firmware-index.json` 和 `npm-index.json`，但不包含自身。

## Tarball 合同

`npm_release_tarball` 消费 `npm_package` tree artifact 与该 package 的 `package.json`。Bazel 在分析阶段声明一个只含单个 `.tgz` 的 tree artifact；实际 basename 在构建动作中读取 manifest 后确定，不在 `.bzl` 或 release script 中硬编码 package name/version。

命名遵循 npm pack：去掉 scoped name 的开头 `@`，把 scope/name 中的 `/` 改为 `-`，再加 `-<package version>.tgz`。例如 `@gizclaw/h2loader` 的 `0.2.2` 生成 `gizclaw-h2loader-0.2.2.tgz`；unscoped package 保留原名。缺少或非法的 name/version 导致构建失败。

解包后只有 `package/` 根目录，文件集合严格等于 manifest 的 `files` 加 `package.json`，可直接 `npm install <tgz>`。这里的 `files` 使用显式相对文件路径，可包含子目录内的文件；不使用 glob、目录简写、忽略规则或 lifecycle script。tree 中多出或缺少文件、tree 内 manifest 与输入 manifest 不一致时，构建失败。README 和 LICENSE 同样由 `files` 显式声明。Bazel sandbox 暴露的文件 symlink 按内容写为普通文件，tar 内不保存 symlink；目录 symlink 与特殊文件被拒绝。

归档固定使用 PAX tar，所有 entry 按归档路径排序，mtime 为 `0`、uid/gid 为 `0`、uname/gname 为空、文件权限为 `0644`、目录权限为 `0755`。gzip 固定 compression level `9`、mtime `0`，不记录原始文件名。相同构建输入产生相同字节，源文件权限或时间戳不影响归档。

## npm-index.json

这是 `GizClaw/deploy` 的稳定消费合同，沿用 `firmware-index.json` 的 `format: 1` 与 Release batch `version` 约定。JSON object keys 排序、缩进为两个空格、末尾保留一个换行。下例 digest 和 size 仅用于展示字段形状，真实值由 tarball 字节计算：

```json
{
  "format": 1,
  "package_count": 1,
  "packages": [
    {
      "name": "@gizclaw/h2loader",
      "sha256": "0000000000000000000000000000000000000000000000000000000000000000",
      "size": 123,
      "tarball": "gizclaw-h2loader-0.2.2.tgz",
      "version": "0.2.2"
    }
  ],
  "version": "0.1.0"
}
```

| 字段 | 合同 |
| --- | --- |
| `format` | integer `1`；字段名不是 `schema_version` |
| 顶层 `version` | 与其它 slice 完全相同的 `RELEASE_VERSION`，独立于 npm package version |
| `package_count` | 正整数，等于 `len(packages)` |
| `packages` | 非空数组，按 `name` 排序，同一 name 只允许一个 entry |
| `packages[].name` | package 自己的非空 npm name，保留 scope |
| `packages[].version` | package 自己的 SemVer |
| `packages[].tarball` | 唯一的裸 basename，等于 GitHub Release asset name，不允许目录部分 |
| `packages[].sha256` | tarball 全部字节的 64 位小写十六进制 SHA-256 |
| `packages[].size` | tarball 的正整数 byte size |

增加第二个 package 时，在其 owner 的 `BUILD.bazel` 中声明 `npm_release_tarball(package, manifest)`，再把 label 加入 `//tools/bazel:npm_release_bundle` 的 `packages` 列表即可；无需修改 Python 实现、slice 或 workflow。重复 package name、tarball basename 或与其它 Release asset 冲突都会失败。

下游 `GizClaw/deploy` 将以 tag 选择这个 Release，验证 GitHub asset digest、索引中的 SHA-256 与 size，再把同一 tarball 发布到香港分发 bucket 的 `npm/` prefix 并生成 static-registry packument。GizOS 提供源 asset 和索引，不在此流水线中上传 bucket 或生成 packument。

## GitHub Packages 并行发布

`.github/workflows/h2loader-npm-publish.yml` 继续按原逻辑运行：相关变更进入 `main` 后，只发布 GitHub Packages 中不存在的 package version。它与 snapshot Release 的 npm asset 路径并行保留，直到 TOS registry 完成验证且 `GizClaw/www` 的 `/tools/` 切换完成；本路径不替换或停用该 workflow。
