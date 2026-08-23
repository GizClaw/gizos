# Starboy

Starboy 是一个 platform-independent Example App。它在纯黑背景上以程序化扫描线绘制两只眼睛，并用时间驱动的球面姿态投影实现注视、眨眼、空闲扫视和重力相对姿态。默认眼框是略微纵向、向外倾斜的蝴蝶翼形；空闲时会轻微扇动，Touch 或 Desktop 全局鼠标跟踪会立即接管球面姿态和瞳孔注视。

每次启动和 BOOT 短按都会生成一组新的高对比眼框与瞳孔配色，并平滑完成颜色过渡。独立摇晃依次切换 `dot`、`circle`、`cat` 和 `acorn` 四种瞳孔轮廓；默认 `acorn` 是略微向内、整体偏下并由眼框裁掉下沿的纵向椭圆。持续高音量会把眼框平滑形变为缓慢旋转、轻微颤动的圆角五角星。启动和两秒关机均使用时间驱动的接近/退场动画。App 不访问 camera，也不使用环境温度或 SoC die temperature。

## Targets

Desktop target 使用 target-private global mouse-to-Touch adapter。鼠标无需点击，移出窗口后仍可控制注视：

```sh
bazel run \
  --config=macos_arm64 \
  //projects/example/targets/cc_binary/starboy:example-starboy
```

AMOLED target 只支持原版 Waveshare ESP32-S3-Touch-AMOLED-1.8。构建命令与板级验收流程见 [AMOLED Starboy guide](/apps/h2loader/boards/amoled/starboy)。

## Capability fallback

Display、Memory 和 monotonic Time 是运行所需能力。Touch、Audio、Motion/IMU、Button 和 Power 都是 optional capability：任何一个不可用时只关闭对应交互，其他动画和设备恢复通道继续运行。AMOLED entry 会把探测失败记录为 `H2_STARBOY_DEGRADED`；Display、实体按键和 Power 启动验证失败仍会阻止 image 确认。

## Validation

```sh
bazel test \
  --config=macos_arm64 \
  //projects/example/apps/starboy/app:starboy_test \
  //projects/example/targets/cc_binary/starboy:mouse_touch_adapter_test

bazel build \
  --config=macos_arm64 \
  //projects/example/targets/cc_binary/starboy:example-starboy

bazel build \
  --config=esp32s3 \
  --//tools/bazel:firmware_version=<version> \
  //projects/example/targets/h2loader_tar_zlib/starboy/amoled:package
```
