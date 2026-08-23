# Tuxemon

Tuxemon 是一个基于 PixelRoot32、Game Runtime 和 PIXA 的 2D tile RPG 原型。Reusable game ownership 位于 `projects/pixa_games/libs/tuxemon/`；Desktop host、H2Loader App 和 Tiga V4.2 launcher 只负责 Runtime、输入、显示和资源接线。

当前原型导入 Tuxemon 的 Taba Town 及相连室内地图，不链接或运行 Tuxemon 的 Python runtime。地图、tileset 和 traversal metadata 在构建前转换成静态 4bpp C++ 数据，设备端不需要 PNG、TMX/XML、zlib 或 Python decoder。

## Gameplay

- 64 × 60 tile 的室外世界使用固定的 240 × 240 camera viewport；角色在中央 dead zone 内移动时 camera 保持不动，跨出 dead zone 后使用 PixelRoot Camera2D 连续跟随，并在世界边界处 clamp。
- 不超过 viewport 的室内房间按实际尺寸居中显示；更高或更宽的室内使用固定、按 tile 对齐的 viewport，确保入口和移动中的角色可见。
- 房屋入口、楼梯和出口通过 portal 在地图之间切换。
- 树、栅栏、水面和 collision object 不可穿越。
- 单向土坡只允许向下跳跃，不能从下方逆向穿越。
- 方向键的 button down/up edge 会保留为 held state，长按时按 260 ms/tile 连续行走。
- 角色使用 Host 提供的 `player.pixa`，需要 `run_left` 和 `run_right` clips。

## Input Roles

| Role | Runtime event | Game behavior |
| --- | --- | --- |
| `up` | button down/up | 向上持续移动 |
| `down` | button down/up | 向下持续移动 |
| `left` | button down/up | 向左持续移动 |
| `right` | button down/up | 向右持续移动 |

具体 board 把物理按键映射到这些逻辑 role。Tiga V4.2 使用 `ok`、`back`、`left`、`right`。

## Asset Pipeline

`tools/tuxemon_import/generate_assets.py` 读取固定版本的 Tuxemon TMX/TSX 和图片资源，并生成：

- 每个房间的 tile layer 和 4bpp tileset；
- collision 与四方向 traversal mask；
- 单向土坡的 entry/exit direction；
- 房间 portal 及目标坐标；
- 背景与 sprite palette。

支持的 Tiled 子集和重新生成命令见 `tools/tuxemon_import/README.md`。上游版本与资源许可记录在 `projects/pixa_games/apps/tuxemon/assets/ATTRIBUTIONS.md`。

## Validation

Portable game test 覆盖世界与室内切换、camera dead-zone follow、连续移动、水面阻挡和单向土坡：

```sh
bazel test //projects/pixa_games/libs/tuxemon:all
```

Desktop build：

```sh
bazel build --config=<host-config> //projects/pixa_games/targets/cc_binary/tuxemon:tuxemon
bazel run --config=<host-config> //projects/pixa_games/targets/cc_binary/tuxemon:tuxemon
```

Tiga V4.2 的构建、安装和按键说明见 Tuxemon on Tiga V4.2。
