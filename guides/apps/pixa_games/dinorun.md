# DinoRun

DinoRun 是横向自动奔跑 game library，ownership 位于 `projects/pixa_games/libs/dinorun/`。Host 负责 Runtime event 和产品流程，DinoRun 只负责 charge jump、地形滚动、障碍、碰撞、distance 和 game result。

通用接入边界见 [PIXA Games](/apps/pixa_games)。

## Required Button

| Role | Gesture | Game behavior |
| --- | --- | --- |
| `action` | press | 在地面开始蓄力，并保持 jump hold 状态 |
| `action` | release | 根据蓄力等级起跳；空中松开会缩短跳跃 |

DinoRun gameplay 只要求 `action`。Host 的 back、left、right、confirm 或其他 button 不属于 DinoRun required input；退出 route 和 game-over popup 操作由 host 处理。

H106 引用 DinoRun 时，把选定的 H106 button component 映射为 `action`。DinoRun 不知道该 component 的 H106 ID 或 board `periph_id`。

## Gameplay

- Player 自动向前奔跑，场景通过循环 ground tile 和向左移动的 obstacle 表达前进。
- 起始 safe zone 不生成 obstacle，也不累计 distance。
- Obstacle 包含 block、pit 和 spike；block 可以堆叠，player 可以站在 block 顶部。
- Action press 在地面开始三级蓄力，release 根据 charge level 设置起跳速度。
- 按住 action 会延长上升阶段；提前 release 使用更强下落 gravity。
- 难度随运行时间提高，表现为 obstacle speed 增加，并受最大速度限制。
- Player 撞到 spike、落入 pit、被推到 screen edge 或离开有效区域时 game over。
- Distance 使用有效运行时间计算，safe zone 不进入 distance。

Physics constant、hitbox、spawn interval、safe-zone duration、speed progression 和 collision tolerance 都属于 DinoRun library，不由 H106 config 重写。

## Visual Assets

DinoRun 运行时只接收 PIXA Games shared `player.pixa`。

| PIXA | Provider | Required clips | Contract |
| --- | --- | --- | --- |
| `player.pixa` | Host 提供并由 PIXA Games 共用 | `run_right`、`jump`、`game_over` | `run_right` 对应当前角色的 `run_right.gif`；当前 legacy source 没有独立 jump GIF，生成器显式用 `run_right` frame 生成 non-loop `jump` clip，后续角色资源可以无代码替换成专用动画；`game_over` 对应 `dead.gif`。各 clip 使用自己的 frame、duration、loop metadata、visible bounds 和 anchor。 |

`player.pixa` 由 H106 等 Host 按当前角色提供，并与 DinoDive、DinoBounce 共用。它使用能够容纳最大角色动画的统一 canvas；较小的 clip 使用透明区域和 anchor 保持原 visible size 和 gameplay alignment。DinoRun 根据 PIXA metadata 播放动画，不读取 GIF，也不自行假设固定 frame count、duration 或 canvas size。

Player 在地面使用 `run_right`；实际离地时切换到 `jump`，landing 时切回 `run_right`。进入 game over 后停止当前 clip，切换到 `game_over` 并按其 clip metadata 播放。Game-over popup 和产品操作仍由 Host 负责。Background、ground、charge bar、distance text 和 debug overlay 使用 PixelRoot primitive；block 和 spike 等小型静态贴图由 DinoRun 以代码内 ARGB4444 持有，不进入 public config。

## Implementation

- Reusable library：`projects/pixa_games/libs/dinorun/`。
- Desktop host：`projects/pixa_games/targets/cc_binary/dinorun/`。
- Portable App：`projects/pixa_games/apps/dinorun/`。
- Tiga V4.2 image：`projects/pixa_games/targets/h2loader_tar_zlib/dinorun/tiga_esp_v4_2/`。
- Player 与 embedded environment assets：由 game package 直接拥有并作为 committed build inputs 审查；没有依赖个人 legacy checkout 的公开 converter 入口。

旧的 `projects/dinorun/` 混合了 gameplay、generated asset 和已失效 launcher，已经删除；不得再作为 build 或 ownership 入口。

## Events and Result

DinoRun 输出：

- `jump`：player 实际离地时产生。
- `game_over`：碰撞或离开有效区域导致 run 结束时产生。
- Final distance。

Game library 不扣除 H106 pet energy，也不直接增加 XP。H106 根据 `game_over` 和 final distance 执行产品 policy。

## Acceptance

相同初始随机种子和 action press/release 序列下，PixelRoot 版本必须保持 `run_right`、`jump` 和 `game_over` animation timing、takeoff/landing/game-over clip 切换时机、charge level、jump trajectory、obstacle sequence、collision result、difficulty progression、distance 和 event timing 一致。
