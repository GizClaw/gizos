# AMOLED Display

独立 PAL Display qualification App 为 `//projects/e2e/targets/h2loader_tar_zlib/pal-display/amoled:package`，使用本 board 的原有 H2Loader layout；只安装 App/P2，保留 Loader/P1、Stage 和 coredump 所有权。

## 预期表现

SH8601 368×448 QSPI；完成 SPI DMA chunk 逐像素验证只证明送入 panel 的数据，不能替代物理屏幕和亮度观察。共享 App 的 24 个 mandatory case 验证 open/info/draw/present/brightness/close，native RGB565、RGB888/RGB444、padding、局部更新、无效输入、borrowed source 释放、重复 present 和两次 reopen。

测试图为带白色边框的红、绿、蓝、白四象限，带一个小的 RGB primary patch。亮度序列为 0%、50%、100%；越界值必须失败并保留实际亮度。BK 背光 PWM1 的 board GPIO map 使用 GPIO7，不得使用占用 LCD_R7 的 SDK 默认 GPIO19。

## managed 安装与验收

先检查 UID、当前 port occupancy、P1/P2、Stage 和 coredump 基线。构建 managed package 后 `send --file`，核对 staged identity，再 `reboot upgrade --monitor`。必须看到新 BOOT、run ledger 和 `H2_DISPLAY_READY rc=0 confirm=0`；失败不 confirm。随后独立 `reboot app --monitor` 重跑，不能把 replay ledger 当成新执行。最终 P1 不变、Stage empty、running/next=App、coredump 保持基线。

源文件和实际 package/report hash 写入 compact evidence，raw serial log/dump 保存在本地。物理屏幕图案及稳定性需相机/readback fixture 或人工明确确认；没有该证据时只能记录 driver qualification。真实 SH8601 亮度命令和 mandatory case ledger 是亮度控制证据，不能宣称测得了物理亮度，也不能把图案确认扩大为每个亮度档都已人工观察。
