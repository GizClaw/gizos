# npm Release

`@gizclaw/h2loader` 的 npm tarball 与固件属于同一个 GizOS snapshot Release。`.github/workflows/release.yml` 只由 `workflow_dispatch` 触发，自动生成 UTC `RELEASE_BATCH=YYYYMMDD-HHMMSS` 与 `v<batch>` tag；不再接受 version 输入。上传 draft Release 后重新下载并逐文件 `cmp`，成功才公开；公开前失败会回滚生成的 draft 和 tag。

## Slice 与本地构建

`catalog` 生成批次后，`npm-packages` 与 `esp32s3`、`esp32p4`、`bk7258` 三个固件 slice 构建并行，每个固件 slice 恰好对应一块发布板。npm job 只需要 checkout、Bazel 和已有 remote-cache auth，不依赖 ESP-IDF 或 firmware-devenv。`firmware-bundle` 只接收 catalog 与三个固件 producer，`package` 再将它压成一个 ZIP。`release-npm-bundle` 与 `release-firmware-package` 最后汇入 `release-bundle`；各输入保留独立子目录，避免下载时覆盖重名文件。

在仓库根目录执行：

```sh
make bazel-release RELEASE_SLICE=npm-packages RELEASE_BATCH=20260920-120000 RELEASE_STAGING_DIR=build/release/npm-packages
bazel test //tools/bazel:npm_release_test //tools/bazel:release_test //tools/bazel:release_bundle_test //projects/h2loader/targets/npm_package/h2loader:release_tarball_test
```

`npm-packages` 是不接受 `RELEASE_INPUT_DIR` 的 source producer。`//tools/bazel:npm_release_bundle` 构建当前声明的 package 列表，staging 目录只包含每个 package 的 `.tgz` 和一个 `npm-index.json`，不生成 `SHA256SUMS`。当前 H2Loader package version 为 `0.3.0`，对应文件是 `gizclaw-h2loader-0.3.0.tgz` 与 `npm-index.json`。

合并已下载的两个 bundle 时，保留各自子目录即可；最终输入扫描拒绝 symlink 和重复 basename：

```sh
mkdir -p build/release/input
cp -R build/release/package build/release/input/firmware
cp -R build/release/npm-packages build/release/input/npm
make bazel-release RELEASE_SLICE=release-bundle RELEASE_BATCH=20260920-120000 RELEASE_INPUT_DIR=build/release/input RELEASE_STAGING_DIR=build/release/final
```

`release-bundle` 要求 `firmware-release-v<batch>.zip`、`npm-index.json` 与索引声明的全部 npm tarball。它重新校验 ZIP 内固件 identity、checksum coverage、SHA-256/size 和 exact-set，并校验 npm identity 与每个 tarball 的 SHA-256、字节数；多余或缺少任一 asset 都失败。当前恰好四个顶层资产：`firmware-release-v<batch>.zip`、`gizclaw-h2loader-0.3.0.tgz`、`npm-index.json`、重新计算的 `SHA256SUMS`。顶层校验和仅覆盖前三个文件；`firmware-index.json` 与固件自身的 `SHA256SUMS` 位于 ZIP 内。固件 ZIP 的布局和本地 `package` 命令见 [Firmware Release](./index#firmware-release)。

## Tarball 合同

`npm_release_tarball` 消费 `npm_package` tree artifact 与该 package 的 `package.json`。Bazel 在分析阶段声明一个只含单个 `.tgz` 的 tree artifact；实际 basename 在构建动作中读取 manifest 后确定，不在 `.bzl` 或 release script 中硬编码 package name/version。

命名遵循 npm pack：去掉 scoped name 的开头 `@`，把 scope/name 中的 `/` 改为 `-`，再加 `-<package version>.tgz`。例如 `@gizclaw/h2loader` 的 `0.3.0` 生成 `gizclaw-h2loader-0.3.0.tgz`；unscoped package 保留原名。缺少或非法的 name/version 导致构建失败。

解包后只有 `package/` 根目录，文件集合严格等于 manifest 的 `files` 加 `package.json`，可直接 `npm install <tgz>`。这里的 `files` 使用显式相对文件路径，可包含子目录内的文件；不使用 glob、目录简写、忽略规则或 lifecycle script。tree 中多出或缺少文件、tree 内 manifest 与输入 manifest 不一致时，构建失败。README 和 LICENSE 同样由 `files` 显式声明。Bazel sandbox 暴露的文件 symlink 按内容写为普通文件，tar 内不保存 symlink；目录 symlink 与特殊文件被拒绝。

归档固定使用 PAX tar，所有 entry 按归档路径排序，mtime 为 `0`、uid/gid 为 `0`、uname/gname 为空、文件权限为 `0644`、目录权限为 `0755`。gzip 固定 compression level `9`、mtime `0`，不记录原始文件名。相同构建输入产生相同字节，源文件权限或时间戳不影响归档。

## npm-index.json

这是 `GizClaw/deploy` 的稳定消费合同，保留 `format: 1` 与顶层 `version` 字段，后者现在存放 UTC `RELEASE_BATCH`，以保持现有 npm index 字段合同；固件索引则使用顶层 `batch`。JSON object keys 排序、缩进为两个空格、末尾保留一个换行。下例 digest 和 size 仅用于展示字段形状，真实值由 tarball 字节计算：

```json
{
  "format": 1,
  "package_count": 1,
  "packages": [
    {
      "name": "@gizclaw/h2loader",
      "sha256": "0000000000000000000000000000000000000000000000000000000000000000",
      "size": 123,
      "tarball": "gizclaw-h2loader-0.3.0.tgz",
      "version": "0.3.0"
    }
  ],
  "version": "20260920-120000"
}
```

| 字段 | 合同 |
| --- | --- |
| `format` | integer `1`；字段名不是 `schema_version` |
| 顶层 `version` | 与其它 slice 完全相同的 `RELEASE_BATCH`，独立于 npm package version |
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
