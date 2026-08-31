# KICKPI K4B 应用烧录

本页说明如何通过标准 ADB 在已经运行 Linux 与 `adbd` 的 KICKPI K4B 上烧录和验证 GizOS App。它不负责安装 K4B 操作系统、rootfs 或设备端 `adbd`。

操作真实设备前先读取当前 OS user 的 `.env/users/<os-user>.md`，确认板型和当前端口映射；历史记录不能替代实时 `adb devices -l`。

## 目标合同

- Board：KICKPI K4B / Allwinner T113-S3。
- ABI：ARMv7 EABI5 hard-float。
- Dynamic loader：`/lib/ld-linux-armhf.so.3`。
- Display：1024×600 framebuffer，逻辑输入 RGB565，当前 K4B image native layout 为 ARGB8888。
- Touch：evdev stable device name `gt9xxnew_ts`，逻辑 viewport 1024×600，single-contact down/move/up；source range 由 `EVIOCGABS` 读取，K4B orientation 为 `swap_xy=0`、`invert_x=0`、`invert_y=0`。
- Audio：ALSA `default` playback route，AAC-LC 由静态链接的 FDK-AAC 解码。
- 用户按键：GPIO character-device chip label `pio`，line `PD14`（offset 110），active-low，BSP peripheral `action_button`。
- 当前测试装配网络：Quectel EC801E-CN USB modem 以 `cdc_ether` 暴露为 `eth1`；地址、route 与 DNS 由目标 Linux 网络服务管理。
- Display 临时目录：`/tmp/h2-display`。
- MP4 Player 临时目录：`/tmp/h2-mp4-player`。
- Touch Smoke 临时目录：`/tmp/h2-touch`。
- 持久化 binary：`/opt/h2/bin/display`、`/opt/h2/bin/mp4-player` 与 `/opt/h2/bin/touch`。
- CedarX runtime libraries：`/opt/h2/lib/*.so`。
- MP4 Player media：`/opt/h2/share/mp4-player/test_1024x600_h264_aac.mp4`。
- systemd units：`/etc/systemd/system/display.service`、`/etc/systemd/system/mp4-player.service` 与 `/etc/systemd/system/touch.service`。

| App | Ready marker | Stopped marker |
| --- | --- | --- |
| Display | `H2_SMOKE_DISPLAY_READY board=kickpi_k4b width=1024 height=600` | `H2_SMOKE_DISPLAY_STOPPED board=kickpi_k4b` |
| MP4 Player | `H2_SMOKE_MP4_PLAYER_READY board=kickpi_k4b width=1024 height=600` | `H2_SMOKE_MP4_PLAYER_STOPPED board=kickpi_k4b` |
| Touch | `H2_TOUCH_SMOKE_READY board=kickpi_k4b touch=gt9xxnew_ts` | `H2_TOUCH_SMOKE_STOPPED board=kickpi_k4b` |

Display、MP4 Player 与 Touch 都直接占用 framebuffer，同一时间只能运行一个。烧录前必须分别记录三个 service 是否 active，停止当前 owner；验证结束后只恢复操作前实际 active 的 service。

MP4 Player demux H.264/AAC track，通过 CedarX VE 解码视频、FDK-AAC 解码音频，并通过 ALSA 连续播放 PCM。它只声明当前 launcher 使用的 Display、Video Decoder、Audio Decoder、Audio、Task、Queue、Time、Memory、Log 与 Filesystem 能力，不表示 K4B 已实现全部 PAL。

## 连接与预检

先按[通用 ADB 文档](./index)加载 `$ADB`，再显式选择设备：

```sh
"$ADB" devices -l
export K4B_ADB_SERIAL='<adb-serial>'
"$ADB" -s "$K4B_ADB_SERIAL" get-state
"$ADB" -s "$K4B_ADB_SERIAL" shell uname -m
"$ADB" -s "$K4B_ADB_SERIAL" shell 'test -e /lib/ld-linux-armhf.so.3'
"$ADB" -s "$K4B_ADB_SERIAL" shell 'test -c /dev/fb0 && test -w /dev/fb0'
"$ADB" -s "$K4B_ADB_SERIAL" shell 'for chip in /dev/gpiochip*; do [ -c "$chip" ] && echo "$chip"; done'
"$ADB" -s "$K4B_ADB_SERIAL" shell 'grep "gpio-110" /sys/kernel/debug/gpio 2>/dev/null || true'
"$ADB" -s "$K4B_ADB_SERIAL" shell 'ip -brief address; ip route'
```

预检还要确认 `id`、目标目录写权限和 service manager。设备不使用 systemd 时只允许临时验证；不能安装仓库提供的 `.service` 后声称持久化完成。

当前 K4B image的 `adbd` 会在一次 `adb shell` 结束时清理它的普通 background child；`nohup ... &` 与 `setsid ... &` 都不能建立可复查的临时进程。以下流程使用 image自带的 BusyBox `start-stop-daemon -b -m` 完成 daemonize与 PID file；缺少该工具时应保持一个 foreground `adb shell` session，不能假装后台进程仍在运行。

当前装配中的 EC801E-CN data interface 是普通 Linux `eth1`。GizOS 的 Netif/System Event PAL 只观察内核已经建立的 interface、address 与 default-route state；它不负责 USB mode switching、SIM/APN、AT command、DHCP policy 或 rootfs 网络服务。只有目标 image 暴露并验证可用的 modem control node 后，才能另行实现 Modem PAL；不得把存在 `eth1` 误报为 Modem PAL 已完成。当前装配没有 `wlan*` interface，因此也不能声称 Wi-Fi PAL 可用。

## 构建

构建 host 必须是 Linux x86_64，因为当前 hermetic ARM GNU toolchain distribution 的 compiler executable 是 Linux x86_64。先从 sibling `firmwares-devenv` 加载 CedarX compact SDK 路径：

```sh
REPO_ROOT=$PWD . .env/devenv
test -f "$K4B_CEDARX_INCLUDE_DIR/vdecoder.h"
test -f "$K4B_CEDARX_LIB_DIR/libvdecoder.so"
bazel build -c opt --config=kickpi_k4b  //boards/kickpi_k4b/t113-s3:button_runtime_smoke  //projects/example/targets/cc_binary/display/kickpi_k4b:display  //projects/example/targets/cc_binary/mp4-player/kickpi_k4b:mp4_player  //projects/example/targets/cc_binary/touch/kickpi_k4b:touch
```

Bazel 从固定 URL 与 SHA-256 下载 ARMv7 hard-float compiler/sysroot；`K4B_CEDARX_INCLUDE_DIR` 与 `K4B_CEDARX_LIB_DIR` 只提供目标专有 CedarX headers/shared libraries。`-c opt` 是 K4B 音视频性能验收的必要条件，toolchain 会固定传入 `-O2 -DNDEBUG`。两个产品 App 产物分别是：

```text
bazel-bin/projects/example/targets/cc_binary/display/kickpi_k4b/display
bazel-bin/projects/example/targets/cc_binary/mp4-player/kickpi_k4b/mp4_player
bazel-bin/projects/example/targets/cc_binary/touch/kickpi_k4b/touch
```

`button_runtime_smoke` 位于 `bazel-bin/boards/kickpi_k4b/t113-s3/button_runtime_smoke`，仅用于真实 BSP→PAL→Runtime input 验收，不是持久化产品 App。

## Button Runtime 验收

目标 Linux image 必须把 PD14 留给 userspace GPIO character-device consumer，不能同时把它声明为 LED、key 或其它内核设备。如果 `/sys/kernel/debug/gpio` 显示 `gpio-110` 已被其它 consumer 占用，smoke 必须 fail closed 并报告 `H2_PAL_ERR_BUSY`；应修正目标 image 的 device tree 或 board configuration，不能把 userspace 强制解绑写进 App、PAL provider 或持久化 service。

把 smoke executable 推入临时目录并保持 foreground shell：

```sh
button_smoke=$PWD/bazel-bin/boards/kickpi_k4b/t113-s3/button_runtime_smoke
"$ADB" -s "$K4B_ADB_SERIAL" push "$button_smoke" /tmp/h2-button-runtime-smoke
"$ADB" -s "$K4B_ADB_SERIAL" shell \
  'chmod 0755 /tmp/h2-button-runtime-smoke; exec /tmp/h2-button-runtime-smoke'
```

观察 `H2_BUTTON_RUNTIME_READY chip=pio line=PD14(110) active_low=1` 后，对右侧 `User Key` 完成一次明确 press/release。smoke 在 Runtime 初始化前要求连续的 released baseline，避免 line ownership 切换或启动时 held 状态被误认为用户点击。验收必须按顺序出现 `DOWN`、`UP`、`ACTION` 和最终 `H2_BUTTON_RUNTIME_PASS`；不能用 host fake test 替代实机 Runtime 链路验收。

对尚未修正 device tree、仍把 PD14 绑定到 `leds-gpio` 的旧 image，只能在受控的一次性硬件诊断中临时 unbind，并使用 shell trap 恢复原 driver。这只证明实体按键、GPIO provider 和 Runtime event 链路，不表示未修正的 image 已满足产品运行前置条件：

```sh
"$ADB" -s "$K4B_ADB_SERIAL" shell '
restore_leds() {
  if [ -e /sys/bus/platform/drivers/leds-gpio/bind ] &&
     [ ! -L /sys/bus/platform/devices/leds/driver ]; then
    echo leds > /sys/bus/platform/drivers/leds-gpio/bind
  fi
}
trap restore_leds EXIT HUP INT TERM
echo leds > /sys/bus/platform/drivers/leds-gpio/unbind || exit 1
/tmp/h2-button-runtime-smoke
'
```

诊断退出后必须回读 `/sys/bus/platform/devices/leds/driver` 和 `/sys/kernel/debug/gpio`，确认原 driver 已恢复且 PD14 回到操作前状态。

## Touch Runtime 验收

Touch smoke 同时验证 Linux evdev Touch provider、Touch PAL、LVGL pointer indev，以及 LVGL widget 写入 mapped `PUSH_EDGE` Runtime Button periph 的链路。它不把 raw Touch 做成 Runtime pull input：Touch edge 由 LVGL indev 消费，widget 只向 Runtime 注入 down/up，Runtime 生成客观 phased action；click 和 long press 由 App 判断。

先停止其它 framebuffer owner，再把 executable 推入临时目录并保持 foreground shell：

```sh
touch_bin=$PWD/bazel-bin/projects/example/targets/cc_binary/touch/kickpi_k4b/touch
"$ADB" -s "$K4B_ADB_SERIAL" shell \
  'systemctl stop display.service mp4-player.service touch.service 2>/dev/null || true; rm -rf /tmp/h2-touch; mkdir -p /tmp/h2-touch'
"$ADB" -s "$K4B_ADB_SERIAL" push "$touch_bin" /tmp/h2-touch/touch
"$ADB" -s "$K4B_ADB_SERIAL" shell \
  'chmod 0755 /tmp/h2-touch/touch; exec /tmp/h2-touch/touch'
```

看到 ready marker 后，先触摸 viewport 四角和中心。Provider 把 driver 报告的 X/Y minimum 映射为 `(0, 0)`，maximum 映射为 `(1023, 599)`，中点映射为 `(floor((raw_x-min_x)*1023/(max_x-min_x)), floor((raw_y-min_y)*599/(max_y-min_y)))`；超出 source range 的值分别 clamp 到对应边界。K4B 不交换或反转 axis，因此左上、右上、左下、右下和中心实际触点必须分别落在 `(0,0)`、`(1023,0)`、`(0,599)`、`(1023,599)` 和约 `(511,299)`，允许手指接触面积造成的小范围偏差，但不能出现翻转、旋转或非线性缩放。确认红色 marker、屏幕坐标与 stderr 中的 `Touch down/up x=... y=...` 一致后，再在屏幕中央蓝色按钮上完成一次短按和一次按住超过 1 秒的长按。两次按压都必须按序记录 `event=down`、`event=up`、`event=action`，按住期间不能出现任何 event；Runtime 不定义长按阈值，长按只体现为 action 的 `pressed_at_ms`/`released_at_ms` 间隔。屏幕计数与 stderr log 必须一致。缺少任一 ABS axis 或 source range 退化时，Touch open 必须以 `H2_PAL_ERR_UNSUPPORTED` 失败，不能假设 source range 等于 viewport。

用 `SIGTERM` 正常停止并确认 stopped marker，再删除临时目录。恢复 service 时只能恢复操作前实际 active 的 owner，不能默认启动某个 App。

用 toolchain `readelf` 或等价检查确认三个 App executable 都是 ELF32 ARM EABI5 hard-float，interpreter 为 `/lib/ld-linux-armhf.so.3`。MP4 Player 的 Bazel runfiles 还包含 CedarX shared library closure、测试媒体和 service unit；应用烧录必须传输它们，不能只复制 executable 后把 dynamic loader failure 当作 App failure。

## Display 临时烧录与验收

以下命令不修改持久化 service：

```sh
display_bin=$PWD/bazel-bin/projects/example/targets/cc_binary/display/kickpi_k4b/display
"$ADB" -s "$K4B_ADB_SERIAL" shell \
  'systemctl is-active display.service 2>/dev/null || true; systemctl is-active mp4-player.service 2>/dev/null || true'
"$ADB" -s "$K4B_ADB_SERIAL" shell \
  'systemctl stop display.service mp4-player.service 2>/dev/null || true; rm -rf /tmp/h2-display; mkdir -p /tmp/h2-display'
"$ADB" -s "$K4B_ADB_SERIAL" push "$display_bin" /tmp/h2-display/display
"$ADB" -s "$K4B_ADB_SERIAL" shell \
  'chmod 0755 /tmp/h2-display/display; start-stop-daemon -S -b -m -p /tmp/h2-display/display.pid -x /bin/sh -- -c "exec /tmp/h2-display/display >/tmp/h2-display/display.log 2>&1"'
"$ADB" -s "$K4B_ADB_SERIAL" shell \
  'remaining=15; until grep -F "H2_SMOKE_DISPLAY_READY board=kickpi_k4b width=1024 height=600" /tmp/h2-display/display.log; do [ "$remaining" -gt 0 ] || exit 1; sleep 1; remaining=$((remaining - 1)); done'
```

验收屏幕显示 RGB color bars 后，用 `SIGTERM` 正常停止并检查 stopped marker：

```sh
"$ADB" -s "$K4B_ADB_SERIAL" shell \
  'pid=$(cat /tmp/h2-display/display.pid); kill -TERM "$pid"; remaining=15; while kill -0 "$pid" 2>/dev/null && [ "$remaining" -gt 0 ]; do sleep 1; remaining=$((remaining - 1)); done; ! kill -0 "$pid" 2>/dev/null || exit 1; grep -F "H2_SMOKE_DISPLAY_STOPPED board=kickpi_k4b" /tmp/h2-display/display.log'
"$ADB" -s "$K4B_ADB_SERIAL" shell 'rm -rf /tmp/h2-display'
```

## MP4 Player 临时烧录与验收

从 Bazel executable、portable App 测试媒体和环境声明的 CedarX library closure 组装 App-owned 临时目录：

```sh
mp4_bin=$PWD/bazel-bin/projects/example/targets/cc_binary/mp4-player/kickpi_k4b/mp4_player
mp4_media=$PWD/projects/example/apps/mp4-player/data/media/test_1024x600_h264_aac.mp4
"$ADB" -s "$K4B_ADB_SERIAL" shell \
  'systemctl stop display.service mp4-player.service 2>/dev/null || true; rm -rf /tmp/h2-mp4-player; mkdir -p /tmp/h2-mp4-player/lib'
"$ADB" -s "$K4B_ADB_SERIAL" push "$mp4_bin" /tmp/h2-mp4-player/mp4-player
"$ADB" -s "$K4B_ADB_SERIAL" push "$mp4_media" /tmp/h2-mp4-player/test.mp4
for library in \
  libMemAdapter.so libVE.so libaftertreatment.so libawh264.so \
  libcdc_base.so libcdx_ion.so libfbm.so libsbm.so \
  libscaledown.so libvdecoder.so libvideoengine.so; do
  "$ADB" -s "$K4B_ADB_SERIAL" push \
    "$K4B_CEDARX_LIB_DIR/$library" /tmp/h2-mp4-player/lib/
done
"$ADB" -s "$K4B_ADB_SERIAL" shell \
  'chmod 0755 /tmp/h2-mp4-player/mp4-player; start-stop-daemon -S -b -m -p /tmp/h2-mp4-player/mp4-player.pid -x /bin/sh -- -c "exec env LD_LIBRARY_PATH=/tmp/h2-mp4-player/lib:/usr/lib /tmp/h2-mp4-player/mp4-player /tmp/h2-mp4-player/test.mp4 >/tmp/h2-mp4-player/mp4-player.log 2>&1"'
"$ADB" -s "$K4B_ADB_SERIAL" shell \
  'remaining=20; until grep -F "H2_SMOKE_MP4_PLAYER_READY board=kickpi_k4b width=1024 height=600" /tmp/h2-mp4-player/mp4-player.log && grep -F "H2_MP4_PLAYER_AUDIO_READY" /tmp/h2-mp4-player/mp4-player.log; do [ "$remaining" -gt 0 ] || exit 1; sleep 1; remaining=$((remaining - 1)); done'
```

确认视频与声音连续循环、`/proc/asound/card0/pcm0p/sub0/status` 保持 `RUNNING`，且 kernel log 没有 ALSA underrun。循环墙钟时长必须与 media duration 一致；不得用能启动、首帧 marker 或 `fastbuild` 产物代替连续性验收。随后正常停止：

```sh
"$ADB" -s "$K4B_ADB_SERIAL" shell \
  'pid=$(cat /tmp/h2-mp4-player/mp4-player.pid); kill -TERM "$pid"; remaining=15; while kill -0 "$pid" 2>/dev/null && [ "$remaining" -gt 0 ]; do sleep 1; remaining=$((remaining - 1)); done; ! kill -0 "$pid" 2>/dev/null || exit 1; grep -F "H2_SMOKE_MP4_PLAYER_STOPPED board=kickpi_k4b" /tmp/h2-mp4-player/mp4-player.log'
"$ADB" -s "$K4B_ADB_SERIAL" shell 'rm -rf /tmp/h2-mp4-player'
```

最后只恢复预检时实际处于 active 的 service。若无法确认原状态，保持停止并报告，不猜测应启动哪个 framebuffer owner。

## 持久化烧录

持久化操作先把完整 staging tree push 到设备的 App-owned 临时目录，再由设备端具备权限的安装步骤原子替换：

- `/opt/h2/bin/display`
- `/opt/h2/bin/mp4-player`
- `/opt/h2/lib/*.so`
- `/opt/h2/share/mp4-player/test_1024x600_h264_aac.mp4`
- `/etc/systemd/system/display.service`
- `/etc/systemd/system/mp4-player.service`

`mp4-player.service` 使用 `/opt/h2/lib:/usr/lib` 作为 runtime `LD_LIBRARY_PATH`。AAC decoder 静态链接进 executable，不需要从目标 rootfs 或 CedarX SDK 复制 AAC `.so`。写入后执行 `systemctl daemon-reload`，明确选择一个 framebuffer owner，再通过 `systemctl is-active`、journal ready marker 和远端文件 readback 验证。任何一步失败都必须恢复旧文件与安装前 service 状态。

仓库不把这套流程塞进 Makefile，也不提供自定义 ADB wrapper；执行者使用标准 `"$ADB" -s "$K4B_ADB_SERIAL" ...` 完成应用烧录。
