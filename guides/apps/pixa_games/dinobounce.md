# DinoBounce

DinoBounce 是 paddle、fireball 和下降 brick 组成的 bounce game library，ownership 位于 `projects/pixa_games/libs/dinobounce/`。Host 负责 Runtime event 和产品流程，DinoBounce 负责 paddle movement、ball physics、brick lifecycle、difficulty 和 game result。

通用接入边界见 [PIXA Games](/apps/pixa_games)。

## Required Buttons

| Role | Gesture | Game behavior |
| --- | --- | --- |
| `left` | click | 将 paddle 持续移动方向切换为向左 |
| `right` | click | 将 paddle 持续移动方向切换为向右 |
| `action` | click | Ball 尚未 launch 时从 paddle 发射 |

Left/right direction 持续生效，不是单次位移。Game over 后只有 `action` 会重置游戏，左右键不会误触重试；退出 route 由 host 处理。

H106 引用 DinoBounce 时，把自己的逻辑 button component 映射成 `left`、`right` 和 `action`。DinoBounce 不知道 H106 component ID 或 board `periph_id`。

## Gameplay

- Paddle 与 player 角色一起水平移动，并限制在 screen 内。
- Paddle/player 以 4 px/frame 横向移动；ball 从 2.8 px/frame 开始，并随 level 逐步加速到 4.5 px/frame。
- Ball 在 launch 前停留在 paddle 上，`action` 只负责首次 launch。
- Ball 与左右 wall、ceiling、paddle 和 brick 碰撞后反弹。
- Paddle hit position 改变 ball horizontal velocity，paddle movement 也会给 ball 追加 horizontal influence。
- Ball 命中 brick 后 brick 消失。
- Brick 周期性整体下降，并在顶部生成新的 sparse row。
- 难度随运行时间提高，表现为 ball speed 增加和 brick descend interval 缩短。
- Ball 落出 screen 底部，或 brick 到达 paddle 区域时 game over。

Paddle size/speed、ball speed、bounce calculation、brick layout、descent progression 和 collision rule 都属于 DinoBounce library。

## Visual Assets

DinoBounce 运行时只接收 PIXA Games shared `player.pixa`。

| PIXA | Provider | Required clips | Contract |
| --- | --- | --- | --- |
| `player.pixa` | Host 提供并由 PIXA Games 共用 | `run_left`、`run_right` | Run clips 是 loop animation，并把旧版 130 ms frame 调整为 260 ms，使腿部动作与水平移动匹配。Game over 后保留当前方向的角色，与旧版行为一致。各 clip 保留对应旧版 GIF 的全部 frame、loop metadata、visible bounds 和 anchor。 |

Background、paddle、brick、纯文字和 debug overlay 使用 PixelRoot primitive。Fireball 等小型贴图由 DinoBounce 以代码内 ARGB4444 持有，保留透明像素、pivot 和 visible bounds，不进入 public config。DinoBounce 与其他 PIXA Games 共用 Host 已打开的 `player.pixa`，只验证并使用 `run_left` 和 `run_right`。

## Events and Result

DinoBounce 输出：

- `bounce`：ball 与 paddle 有效碰撞时产生。
- `game_over`：ball miss 或 brick 到达 paddle 区域时产生。
- Final survival time。

H106 根据 survival time 决定 XP、reward 和结果页面；DinoBounce 不访问 H106 pet state。

## Acceptance

相同 brick seed 和输入序列下，PixelRoot 版本必须保持 paddle direction、launch state、ball trajectory、brick spawn/descent、collision response、difficulty progression、survival time 和 event timing 一致。
