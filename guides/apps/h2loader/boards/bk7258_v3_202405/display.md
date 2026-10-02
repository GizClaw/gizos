# BK7258 V3 202405 Display

独立 PAL Display qualification App 为 `//projects/e2e/targets/h2loader_tar_zlib/pal-display/bk7258_v3_202405:package`，使用本 board 的原有 H2Loader layout；只安装 App/P2，保留 Loader/P1、Stage 和 coredump 所有权。

launcher 选择独立的 Display GPIO profile，保留已知可工作的 board RGB clock/control/data 共 29 个引脚，GPIO0/1 保留为 H2Loader UART1。SDK 没有启用 RGB GPIO 初始化，且 runtime GPIO mapper 不接受 profile 中缺失的引脚；因此不能使用仅有 UART/SDIO 的普通 H2Loader GPIO profile。board Display 每次 open 检查真实 RGB mux，仅恢复不正确的映射；H050IWV 没有 SPI 初始化回调，禁用可选 SPI control bus，避免占用 UART1。

## 预期表现

H050IWV 800×480 RGB；LCD 活跃 DMA source/refresh 观测证明 controller scanout source，不能替代物理屏幕和亮度观察。共享 App 的 24 个 mandatory case 验证 open/info/draw/present/brightness/close，native RGB565、RGB888/RGB444、padding、局部更新、无效输入、borrowed source 释放、重复 present 和两次 reopen。

测试图为带白色边框的红、绿、蓝、白四象限，带一个小的 RGB primary patch。亮度序列为 0%、50%、100%；越界值必须失败并保留实际亮度。BK 背光 PWM1 的 board GPIO map 使用 GPIO7，不得使用占用 LCD_R7 的 SDK 默认 GPIO19。

PWM 只有成功 start 后才记为 running。通道初始化或启动失败时释放已取得资源；释放失败保留待清理状态，后续重试必须先完成清理再重新初始化和启动，不能仅修改 duty 后报成功。close 同样传播背光释放错误并保留 Display 供重试。故障注入验证与实际板上图案/启动记录分别保留。

Display 每 32 次 frame submission，以及关闭前剩余的 submission，输出 `H2_BK_DISPLAY_PERF`：时间窗口、分配/整帧复制平均耗时、SDK submit 平均耗时和单次最大耗时。它区分 CPU copy 与提交阻塞，不把队列接受次数写成 LCD 实际显示 FPS；播放流畅度仍需结合视频帧时间戳和真实 scanout 观测。

## managed 安装与验收

先检查 UID、当前 port occupancy、P1/P2、Stage 和 coredump 基线。构建 managed package 后 `send --file`，核对 staged identity，再 `reboot upgrade --monitor`。必须看到新 BOOT、run ledger 和 `H2_DISPLAY_READY rc=0 confirm=0`；失败不 confirm。随后独立 `reboot app --monitor` 重跑，不能把 replay ledger 当成新执行。最终 P1 不变、Stage empty、running/next=App、coredump 保持基线。

源文件和实际 package/report hash 写入 compact evidence，raw serial log/dump 保存在本地。物理屏幕图案及稳定性需相机/readback fixture 或人工明确确认；没有该证据时只能记录 driver qualification。真实 PWM 更新和 mandatory case ledger 是亮度控制证据，不能宣称测得了物理亮度，也不能把图案确认扩大为每个亮度档都已人工观察。
