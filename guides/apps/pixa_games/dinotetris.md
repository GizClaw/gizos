# DinoTetris

DinoTetris 是 10 × 20 board 的 falling-block game library，ownership 位于 `projects/pixa_games/libs/dinotetris/`。Host 负责 Runtime event 和产品流程，DinoTetris 负责 piece generation、movement、rotation、drop、line clear、score、level 和 game result。

通用接入边界见 [PIXA Games](/apps/pixa_games)。

## Required Buttons

| Role | Gesture | Game behavior |
| --- | --- | --- |
| `left` | click | Current piece 向左移动一格 |
| `right` | click | Current piece 向右移动一格 |
| `action` | click | Current piece 顺时针旋转 |
| `action` | long press | 开始 fast drop |
| `action` | release | 结束 fast drop |

Action 的 click 与 long press 必须互斥：一次 long press 不能在 release 时再触发 rotate。Reset、退出 route 和 game-over popup 操作由 host 处理。

H106 引用 DinoTetris 时，把自己的逻辑 button component 映射成 `left`、`right` 和 `action`。Host 需要向 Game Runtime 保留 click、long press 和 release 语义；DinoTetris 不自行读取 raw PAL button。

## Gameplay

- Board 固定为 10 column × 20 row。
- Piece 使用 I、O、T、S、Z、J、L 七种 shape，并显示 next-piece preview。
- Left/right 按下时立即移动一格；持续按住约 160 ms 后开始连续横移，之后约每 80 ms 移动一格，release 立即停止；collision 时保持原位。
- Action click 顺时针旋转；原位旋转失败时按既定 offset 尝试 wall kick。
- Action 持续按下约 320 ms 启用 fast drop，release 恢复当前 level 的 normal drop interval；长按 release 不再触发 rotate。
- Piece 落地后保留约 480 ms lock delay；期间仍可横向移动或旋转，成功操作会重新开始 lock delay。
- Lock delay 结束后锁定 piece、清除完整行并生成 next piece。
- 一次清除 1、2、3、4 行的 base score 分别为 100、300、500、800，再乘以当前 level。
- 每累计清除 5 行提升一级，并缩短 normal drop interval，直至最小 interval。
- 新 piece 在初始位置即发生 collision 时 game over。
- Host 在创建 game 时从平台 PAL Crypto random capability 提供初始 seed；同一 game instance 重试时继续当前 random sequence，不回到初始 seed。测试和 Desktop host 可以显式使用固定 seed 以获得可复现结果。
- Game over 使用旧版全屏半透明遮罩和约 300 ms 进入/退出动画；动画结束后由 Host 映射的 retry action 重新开始。

Shape definition、random sequence、wall-kick order、drop interval、score table 和 level progression 都属于 DinoTetris library。

## Visual Assets

DinoTetris 不显示 player，因此不接收 PIXA Games shared `player.pixa`，也不接收其他外部视觉资源。Legacy `240 × 240` 星空 background 转换为 DinoTetris 自有的 embedded ARGB4444 数据；board cell、七种 piece、grid、score、level 和 next preview 使用 PixelRoot primitive。Host 不传入环境路径。

## Events and Result

DinoTetris 输出：

- `rotate`：rotation 或 wall kick 成功时产生。
- `fast_drop`：action long press 实际进入 fast drop 时产生。
- `piece_locked`：current piece 写入 board 后产生。
- `game_over`：new piece 无法 spawn 时产生。
- Final score、level 和 cleared-line count。

H106 根据 result 决定 XP、reward 和结果页面；DinoTetris 不访问 H106 pet state。

## Acceptance

相同 piece seed 和输入序列下，PixelRoot 版本必须保持 piece order、movement、rotation、wall kick、drop timing、line clear、score、level、next preview、game-over condition 和 event timing 一致。
