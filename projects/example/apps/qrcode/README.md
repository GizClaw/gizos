# QR Code App

`qrcode` 把一段文本编码成 QR Code Model 2 符号，并居中绘制到 Display PAL 上。
App 只使用 Runtime 的 Memory 与 Display capability，不初始化 LVGL，也不包含
board、H2Loader 或 target SDK header。

编码和栅格化由 [`libs/qrcode`](/zh/developing/qrcode) 完成。App 按 16 行一组
渲染 RGB565 band 并逐条 `h2_pal_display_draw_bitmap()`，因此不需要整屏
framebuffer；modules、scratch 与 band 缓冲全部来自 Runtime Memory PAL，并在返回
前释放。

阻塞入口是 `h2_qrcode_example_run(h2_runtime_t *, const h2_qrcode_example_config_t *)`。
它只绘制一次：编码、绘制、present、调用可选的 `on_ready`，然后返回，不循环也不
持有状态。Payload、error correction level、最大 version、quiet zone 和亮度由调用
方通过 config 提供。

Expected inspection:

- 屏幕显示黑白 QR Code，四周是白色 quiet zone。
- 符号在屏幕上居中，按整数倍模块尺寸放大，边缘没有被裁切。
- 手机相机扫描该符号得到 config 中配置的文本。
- 画面保持静止，不进入下一屏。
