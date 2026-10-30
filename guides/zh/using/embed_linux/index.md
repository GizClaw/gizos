# Embedded Linux 与 ADB

GizOS 使用标准 ADB 对已经运行 Linux userspace 和 `adbd` 的设备执行应用烧录。ADB 与 H2Loader 是并列的设备工具：ADB 自己负责 scan、设备选择、文件传输、远程执行和状态查询；Bazel 只负责生成 executable 并声明资源、service metadata 与 target runtime libraries。

本流程不安装操作系统、rootfs 或 `adbd`。目标设备没有可用的 Linux 和 `adbd` 时，应先按板卡厂商或产品 recovery 流程准备系统，不能把失败降级成 launcher 内的隐式刷机。

## 准备 ADB

GizOS 与 `firmwares-devenv` 必须是同级 checkout。首次安装或验证固定版本 ADB：

```sh
make -C ../firmwares-devenv adb
make -C ../firmwares-devenv adb-check
```

从 GizOS 仓库根目录加载 shared environment：

```sh
REPO_ROOT=$PWD . .env/devenv
test -x "$ADB"
"$ADB" version
```

`firmwares-devenv` 导出的 `ADB` 是经过版本和可执行性检查的绝对路径；同一 `platform-tools` 目录也会进入 `PATH`。自动化和文档命令使用 `"$ADB"`，避免误用机器上其它版本。

## Scan 与显式设备选择

```sh
"$ADB" devices -l
export EMBED_LINUX_ADB_SERIAL='<adb-serial>'
"$ADB" -s "$EMBED_LINUX_ADB_SERIAL" get-state
```

只有状态为 `device` 才能继续。`unauthorized`、`offline`、没有设备或同时存在多个设备都必须停止；不能隐式选择列表第一项。

## 烧录前检查

具体 board 使用页决定允许的 ABI、动态加载器、安装位置和 service manager。通用检查至少包括：

```sh
"$ADB" -s "$EMBED_LINUX_ADB_SERIAL" shell uname -m
"$ADB" -s "$EMBED_LINUX_ADB_SERIAL" shell id
"$ADB" -s "$EMBED_LINUX_ADB_SERIAL" shell 'test -d /tmp && test -w /tmp'
```

还必须确认目标 executable 所需共享库存在、最终目录可写、资源完整，并记录当前运行的冲突 service。缺少任一前置条件时 fail closed，不执行部分安装。

## 临时烧录与运行

先传输到设备的唯一临时目录，再校验并执行。示例中的本地产物和远端命令必须由 board 使用页具体化：

```sh
"$ADB" -s "$EMBED_LINUX_ADB_SERIAL" shell 'rm -rf /tmp/h2-app && mkdir -p /tmp/h2-app'
"$ADB" -s "$EMBED_LINUX_ADB_SERIAL" push '<local-binary>' /tmp/h2-app/app
"$ADB" -s "$EMBED_LINUX_ADB_SERIAL" shell 'chmod 0755 /tmp/h2-app/app && /tmp/h2-app/app'
```

临时目录只能使用明确的 App-owned path。不要递归删除 `/tmp`、`/opt` 或其它共享目录。

## 持久化应用烧录

持久化安装是显式设备变更。先从 Bazel output 与 runfiles 收集 board guide 指定的 executable、资源、service unit 和 runtime libraries，生成可检查的 App-owned staging tree，再通过 ADB push 到设备临时目录；确认所有文件和权限后，才在设备端原子替换最终路径并通知 service manager。不要从 host 逐个覆盖正在运行的 binary 或 service unit。

安装完成后必须验证：

- 远端文件、权限和必要 hash/readback。
- App-specific ready marker 或 service active state。
- 正常停止使用规定信号并产生 stopped marker。
- 失败时清理临时目录并恢复安装前运行的冲突 service。

Board-specific 命令与验收以对应使用页为准；KICKPI K4B 见 [K4B 应用烧录](/zh/using/embed_linux/kickpi_k4b)。
