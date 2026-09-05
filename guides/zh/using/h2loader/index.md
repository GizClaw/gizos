# H2Loader 使用入口与环境

H2Loader 使用说明的两个入口是正式维护的工厂 Batch Loader 和 repository-only CLI。Batch Loader 通过浏览器 Web Serial 执行多设备扫描与统一 Stage/reboot 生命周期；CLI 提供 package、可靠串口设备操作与独立 BLE iKCP baseline。

## 选择入口

工厂批量安装使用 H2Loader Batch Loader 的浏览器发布物。Current package 弹窗只校验并保存当前页面 session 中选择的本地 APP 或 Loader package，不上传固件。Devices table 会恢复当前 origin 已授权的 Web Serial port，并允许继续追加；Scan 刷新 authoritative status。批量操作必须使用当前 `stage`、`abortStage` 与 `rebootApp`/`rebootLoader`/`rebootUpgrade` SDK 合同；旧 Flash/Rollback/Restart API 不属于 `0.2.0`。localStorage 中的历史显示信息在重载后始终为 stale，不能替代新的 live Scan。

在仓库中生成 package、自动化可靠串口操作或运行诊断命令时使用：

```sh
bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader -- -h
```

完整命令见 [H2Loader CLI](./cli)。

裸 `make` 只显示 target help，不解析 Bazel dependency、不加载 environment、不安装依赖，也不启动进程或 listener：

```sh
make
make help
```

## CLI environment

CLI 是由 Bazel 构建和运行的 native C11 App，不需要 Python、uv、venv、pyserial 或 Bleak。直接执行 CLI 只使用调用方继承的 environment；它不安装或探测 Web、tray、Node.js 或 OpenAPI dependency。

只检查本机 repository、SDK、toolchain、Bazel 和 Wi-Fi 配置：

```sh
make cfg-doctor
```

`make cfg-doctor` 按以下顺序加载 shell environment，后加载的文件可以覆盖先加载的值：

1. 仓库 `.env/devenv`
2. `~/.config/h2loader/env`

`.env/devenv` 根据 GizOS 的仓库根目录解析 `../firmwares-devenv/export.sh`，不依赖启动命令的当前目录。所有开发者必须把 GizOS 与 `firmwares-devenv` 作为同级 checkout，并通过后者统一维护 ESP-IDF、BK SDK、toolchain 和 host tool。首次使用先初始化 devenv：

```sh
git clone --recurse-submodules git@github.com:h2vivi/firmware-devenv.git ../firmwares-devenv
make -C ../firmwares-devenv
```

不再加载 `.env/<os-user>`；Wi-Fi credential 和其他 secret 只能放在 `~/.config/h2loader/env`。

`.env/users/<os-user>.md` 是供用户和 Agent 阅读的本机 operator context，不是 shell env，`make` 不会 source 它。直接执行 `bazel run --config=<host> //projects/h2loader/targets/cc_binary/cli:h2loader --` 也不会主动加载 operation env，只使用当前进程已经继承的环境。

## Repository maintenance

启动 VitePress Documentation 开发服务器：

```sh
make guides-watch
```

生产 Documentation 由 `make guides-build` 通过 Bazel 构建并审计后同步到 `build/guides`。
Board 与 image 的产品合同见 [H2Loader Boards](/apps/h2loader/boards/)。设备仍可通过 H2Loader command transport 通信时，安装、更新、回退和恢复必须继续使用 H2Loader；只有确认 H2Loader 无法通信或自我恢复时，才进入对应 board 文档的底层 recovery。
