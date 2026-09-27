# PAL Storage E2E

独立验证 FileSystem 的 11 个 vtable 操作和 Preferences 的 17 个操作（包括 namespace 上的 16 个方法），共 28 项。Disk 的六个接口操作裸分区擦写，单独归入后续 `pal-disk`；本 App 不擦写真实固件分区。

Portable App 只借用 Runtime 中的 FS、Preferences 和 Memory，以及宿主独占的测试路径/namespace。宿主负责建立隔离的数据目录、真实 provider、进程重启和结果汇总。禁止把旧的混合 PAL App 当作本 App 的测试实现或把缺失接口当作通过。

验收包含二进制/空文件、短读和 EOF、seek、truncate、rename/remove、递归 clear、相邻路径隔离、反复打开关闭；Preferences 包含五种值类型、覆写、类型错配、namespace 隔离、只读保护、迭代/提前关闭、分配失败及 commit。持久化验收分 seed/verify 两个独立进程，第二个进程必须用全新 provider 重新读取第一阶段提交的数据；关闭再打开一个 handle 不能代替重启验证。

只有全部必选 case PASS、两个阶段身份一致、全部 handle/cursor 回收且测试数据清理成功才允许 qualified。平台不支持某个操作时保持 BLOCKED，不能从清单移除。断电原子性、恶意宿主并发修改和物理介质寿命不由正常进程重启证据证明。

第一阶段目标为 Desktop 的真实 OS FileSystem 和 SQLite Preferences。Web、移动端、ESP/BK 的适配在独立 launcher 中接入同一契约；未运行的平台不宣称通过。
