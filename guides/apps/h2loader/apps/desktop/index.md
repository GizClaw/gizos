# H2Loader Desktop

H2Loader Desktop 是 GizOS 自有的通用 Tauri 桌面客户端。React 界面、Rust 文件和进程生命周期、原生串口会话适配器与 H2Loader Host Core 在同一仓库维护。h6infra 只消费网页 SDK 和桌面安装包，不维护另一份桌面实现。

构建、验证、文件校验与安装包发布方式见 [桌面项目 README](https://github.com/GizClaw/gizos/tree/main/projects/h2loader/apps/desktop)。界面支持串口枚举、持久连接、固件选择、批量操作和独立日志窗口；设备能力与操作许可始终以连接后的 H2Loader 状态为准。

固件格式、版本号和校验规则由 Host Core 的生产实现定义，桌面通过原生适配器调用同一解析器。Rust 负责不可变文件快照与有界子进程，不维护另一套固件格式规则。已连接设备的升级和重启仍通过 H2Loader 完成。
