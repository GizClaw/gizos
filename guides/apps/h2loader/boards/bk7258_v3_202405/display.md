# BK7258 V3 202405 Display

独立 PAL Display qualification App 为 `//projects/e2e/targets/h2loader_tar_zlib/pal-display/bk7258_v3_202405:package`，使用本 board 的原有 H2Loader layout；只安装 App/P2，保留 Loader/P1、Stage 和 coredump 所有权。

launcher 选择独立的 Display GPIO profile，保留已知可工作的 board RGB clock/control/data 共 29 个引脚，GPIO0/1 保留为 H2Loader UART1。SDK 没有启用 RGB GPIO 初始化，且 runtime GPIO mapper 不接受 profile 中缺失的引脚；因此不能使用仅有 UART/SDIO 的普通 H2Loader GPIO profile。board Display 每次 open 检查真实 RGB mux，仅恢复不正确的映射；H050IWV 没有 SPI 初始化回调，禁用可选 SPI control bus，避免占用 UART1。

RGB Display 在 open 时保留一个 canonical shadow 和两个完整 scanout buffer，共三个 native framebuffer。每个 scanout buffer 独立累计自上次更新以来的 dirty 矩形；只有 SDK release callback 返回其写入权后，才从 shadow 复制该区域并重新提交。首次提交覆盖整屏，后续 240×240 区域更新只复制对应像素，未变化的边缘保留。扫描中的像素与 metadata 不被修改；无空闲 buffer 时有界等待，close 先关闭并 drain controller，再释放三个 buffer。QSPI 路径继续使用原来的提交方式。

## 预期表现

H050IWV 800×480 RGB；LCD 活跃 DMA source/refresh 观测证明 controller scanout source，不能替代物理屏幕和亮度观察。共享 App 的 24 个 mandatory case 验证 open/info/draw/present/brightness/close，native RGB565、RGB888/RGB444、padding、局部更新、无效输入、borrowed source 释放、重复 present 和两次 reopen。

BK 背光 PWM1 的 board GPIO map 使用 GPIO7，不得使用占用 LCD_R7 的 SDK 默认 GPIO19。

PWM 只有成功 start 后才记为 running。通道初始化或启动失败时释放已取得资源；释放失败保留待清理状态，后续重试必须先完成清理再重新初始化和启动，不能仅修改 duty 后报成功。close 同样传播背光释放错误并保留 Display 供重试。故障注入验证与实际板上图案/启动记录分别保留。

诊断 image 可以仅为 `h2_bk7258_board` component 显式启用 `H2_BK7258_DISPLAY_DIAGNOSTICS=1`。默认不携带该诊断；启用时在 open、close、亮度命令和有界频率的 present 后输出真实 GPIO/mux、LCD refresh/source 与 shadow/scanout 的稀疏像素 sample。它不分配 framebuffer、不等待刷新、不修改引脚或显示状态；异步 controller 可以仍在扫描上一帧，sample 也不是完整 readback 或光学通过证据。结合 consumer 的页面切换时间，判断是否仍提交、像素是否变化及 RGB controller 是否在扫描。

## managed 安装与验收

按 [PAL Display 公共验收](/apps/e2e#pal-display) 执行 managed 安装、独立 App reboot、测试图与亮度检查，并核对最终分区、Stage 和 coredump。
