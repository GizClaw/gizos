# AMOLED Starboy

Starboy 只支持原版 Waveshare ESP32-S3-Touch-AMOLED-1.8：SH8601、FT3168 `0x38`、QMI8658 `0x6b`/`0x6a`、ES8311 和 AXP2101 `0x34`。CO5300/CST820 V2 不在该 target 的合同内。

## 构建

```sh
bazel build \
  --config=esp32s3 \
  --//tools/bazel:firmware_version=<version> \
  //projects/example/targets/h2loader_tar_zlib/starboy/amoled:package
```

输出位于 `bazel-bin/projects/example/targets/h2loader_tar_zlib/starboy/amoled/package/example-starboy-amoled.update.tar.zlib`。安装、确认和恢复必须遵循 [H2Loader CLI 使用流程](/zh/using/h2loader/cli)。

## 控制与动效

- FT3168 接触点控制球面朝向和瞳孔注视。
- 每次启动根据随机种子生成一组高对比配色；BOOT 每次短按重新生成眼框与瞳孔色差明显的新配色，并平滑过渡。配色和瞳孔轮廓是彼此独立的状态。
- PWR 是 AXP2101 POWERON 的低有效输入；board provider 读取 PMIC `0x49` 的下降沿/上升沿状态并执行 30 ms debounce。启动时眼睛以轻微步态从远处接近；持续按住两秒后先播放走远退场，再由 App 请求 AXP2101 关机。PMIC 配置为持续按住两秒开机，并保留四秒硬件关机兜底。
- AMOLED target 私有适配先把顺时针安装的 QMI8658 加速度计与陀螺仪 X/Y 轴映射到竖屏坐标，再由重力投影控制整组眼睛相对地面保持竖直。突然翻转时转动先快后慢，缓慢滚动时连续跟随；快速运动、自由落体或屏幕接近水平且平面内重力不足时冻结最后可靠方向。
- QMI8658 是六轴器件，不提供绝对航向；Starboy 只使用重力确定屏幕平面内的竖直方向，不承诺磁北或绕重力轴的绝对角度。
- 每次独立摇晃在 `dot`、`circle`、`cat` 和 `acorn` 四种瞳孔轮廓间切换；持续摇晃不会继续改变配色。ES8311 麦克风的持续高音量触发 anxious 响应，眼睛平滑形变为缓慢旋转的圆角五角星并伴随轻微颤动，当前配色保持不变。

## 验收

启动日志应包含 `H2_STARBOY_READY` 和随机配色结果 `H2_STARBOY_LOOK`，并声明 FT3168、QMI8658、ES8311、按键、AXP2101 和 `orientation=gravity`。验收至少覆盖触摸注视、连续随机配色及色差、四种摇晃瞳孔、开机入场、两秒退场关机、两秒 PMIC 开机、设备竖直时眼睛保持竖直、顺时针与逆时针物理滚转方向正确、姿态连续跟随、麦克风星星眼、H2Loader 确认与恢复。
