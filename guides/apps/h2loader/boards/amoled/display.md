# AMOLED Display

独立 PAL Display qualification App 为 `//projects/e2e/targets/h2loader_tar_zlib/pal-display/amoled:package`，使用本 board 的原有 H2Loader layout；只安装 App/P2，保留 Loader/P1、Stage 和 coredump 所有权。

## 预期表现

SH8601 368×448 QSPI；完成 SPI DMA chunk 逐像素验证只证明送入 panel 的数据，不能替代物理屏幕和亮度观察。共享 App 的 24 个 mandatory case 验证 open/info/draw/present/brightness/close，native RGB565、RGB888/RGB444、padding、局部更新、无效输入、borrowed source 释放、重复 present 和两次 reopen。


## managed 安装与验收

按 [PAL Display 公共验收](/apps/e2e#pal-display) 执行 managed 安装、独立 App reboot、测试图与亮度检查，并核对最终分区、Stage 和 coredump。
